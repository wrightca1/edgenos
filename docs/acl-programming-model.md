# How ACLs Are Programmed Into the AS5610-52X (BCM56846 Trident+)

This documents the full path from an operator ACL rule down to the silicon, for the
Edgecore AS5610-52X (Broadcom BCM56846 "Trident+"). It unifies four views:

1. **Cumulus Linux** — the reference NOS's operator-facing model (public NVIDIA docs).
2. **Broadcom Trident+ Field Processor (FP/IFP)** — the hardware the rules land in.
3. **Our reverse-engineering** — exact memory/register values captured from a live
   Cumulus 2.5.0 rig (`cumulus-fp-live-capture/FP_DECODE.md`).
4. **EdgeNOS** — our re-implementation (`asic/bcm56846/acl.c`, `core/datapath/l3.c`).

Sources: NVIDIA Cumulus Linux ACL docs (v3.7, closest to our captured 2.5.0), Broadcom
Trident programming references, and our own on-chip captures. Links at the bottom.

---

## 1. Operator model (Cumulus Linux)

Cumulus expresses ACLs as **Linux netfilter rules** (iptables / ip6tables / ebtables)
and offloads the hardware-eligible ones to the ASIC.

- **Rule files:** `/etc/cumulus/acl/policy.d/*.rules`, included by
  `/etc/cumulus/acl/policy.conf`. Defaults: `00control_plane.rules`,
  `99control_plane_catch_all.rules`.
- **Rule sections:** `[iptables]` (IPv4), `[ip6tables]` (IPv6), `[ebtables]` (L2/MAC).
- **Install:** `cl-acltool -i` parses the files, writes the Linux kernel chains, AND
  syncs the hardware-eligible subset to the ASIC via **`switchd`**. (Editing iptables
  directly does NOT touch hardware — only `cl-acltool`/`switchd` do.)
- **Chains → direction:**
  - `INPUT` and `FORWARD -i <ingress>` → **ingress** TCAM (this is where dst-IP drops land).
  - `FORWARD -o <egress>` / `OUTPUT` → **egress** TCAM.
  - INPUT and ingress-FORWARD share the same ingress memory.
- **Actions:** `ACCEPT`, `DROP`, `LOG`, `SPAN`/`ERSPAN` (mirror), `POLICE`,
  `TRICOLORPOLICE`, `SETCLASS` (assign an internal CoS/CPU-queue class; non-terminating —
  continues to the next rule). All other rule actions are **terminating** (no fallthrough).
- **Ordering caveat:** the silicon **reorders** rules by slice/priority when switchd writes
  them; it is NOT the sequential iptables semantics. Ingress-interface FORWARD rules are
  evaluated before egress-interface ones.

### Default control-plane rules (these are what we captured in silicon)
`00control_plane.rules` protects the CPU. It maps 1:1 to the FP entries we dumped:

| Rule (Cumulus)                         | Silicon (our capture)                                   |
|----------------------------------------|---------------------------------------------------------|
| drop martians 240/5, 127/8, 224/4, 255.255.255.255 | FP_TCAM[1536+] F2=0xf000…, 0x7f00…, 0xe000…, G_DROP=1 |
| OSPF/BGP/BFD/MLAG → class 7 @ 2000 pps | FP_POLICY G_COS_INT_PRI=7 + FP_METER REFRESHCOUNT=0x7d0 (2000) |
| IGMP → class 6 @ 300 pps               | FP_POLICY COS_INT_PRI=6 + FP_METER 0x12c                 |
| ICMP/DHCP → class 2 @ 100 pps          | FP_POLICY COS_INT_PRI=2 + FP_METER REFRESHCOUNT=0x64 (100)|
| LOCAL → class 0 @ 1000 pps             | FP_POLICY COS_INT_PRI=0 + FP_METER 0x3e8                 |
| ebtables BPDU/LACP class7, LLDP class6, ARP class2 | separate ingress slice groups (L2 selcodes)  |

So the "CoPP" we reverse-engineered IS Cumulus's default netfilter policy, compiled to FP.

---

## 2. Hardware model (Broadcom Trident+ Ingress Field Processor)

The BCM chips implement ACLs as the **Field Processor (FP)** — a set of TCAM **slices**.

- **Slices & groups.** The IFP TCAM is divided into fixed-size **slices** (256 entries
  each on Trident+). Slices are assigned to **groups** (a "group" = one logical ACL table
  with a common key format / qualifier set). Trident3 (bigger sibling) exposes 12 slices in
  4 groups, 768 rules/group; Trident+ is the same architecture, fewer slices.
- **Virtual vs physical slices.** `FP_SLICE_MAP` maps operator/virtual slice numbers to
  physical slices and to a group id (GID). Allocation is **runtime/dynamic** — the same
  rule can land on different physical slices/EIDs across installs (we proved this: two
  identical installs shuffled EIDs/GIDs but produced byte-identical key DATA).
- **Double-wide.** A key wider than one slice (e.g. full IPv4 5-tuple, or our dst-IP group)
  **pairs two adjacent slices** (`SLICE_x_(x-1)_PAIRING=1`), so one logical entry spans a
  primary + secondary physical slice.
- **Per-entry pipeline (all must line up at the same physical index to match):**
  1. `FP_TCAM[idx]` — the ternary key/mask (VALID=3). Fields F1/F2/F3 selected per-port.
  2. `FP_POLICY_TABLE[idx]` — the action (G/Y/R_DROP, COPY_TO_CPU, COS, meter/counter binding).
  3. `FP_PORT_FIELD_SEL[port]` — per-ingress-port **selcodes** picking which packet fields
     land in F1/F2/F3 for each slice (+ the PAIRING bit).
  4. `FP_GLOBAL_MASK_TCAM[idx]` — first-stage **ingress-port gate** (IPBM = ingress port
     bitmap). Gates whether the slice is even consulted for this ingress port.
  5. `FP_GM_FIELDS[idx]` — the global-mask **field-select TCAM** (KEY/MASK, +X/Y halves).
- **Enables:** `FP_SLICE_ENABLE` (slice + lookup enable bits), `FP_TCAM_BLK_SEL` /
  `FP_GM_TCAM_BLK_SEL` (=0xfff), `PORT_TAB.FILTER_ENABLE` per port, `IFP_BYPASS=0`.
- **Meters/counters:** `FP_METER_TABLE` (token buckets — the pps/burst) and
  `FP_COUNTER_TABLE` (per-entry hit counter, bound via FP_POLICY COUNTER_MODE/INDEX).

### Atomic vs nonatomic TCAM update (operationally important)
- **Atomic (default):** switchd reserves **50% of the TCAM as standby**, builds the new
  table there, then flips — so a rule install never disrupts traffic, but usable ACL space
  is halved.
- **Incremental nonatomic** (`acl.non_atomic_update_mode = TRUE` in `switchd.conf`,
  **Broadcom-only**): updates in place, table by table (mirror → ipv4-mac → ipv6), doubling
  usable entries at the cost of momentary disruption on change. This is why the same rule
  set lands at different slice offsets run to run.

---

## 3. Exact captured recipe (our BCM56846, dst-IP drop group)

From `cumulus-fp-live-capture/FP_DECODE.md` (live Cumulus 2.5.0 @ our rig). The dst-IP /
martian / CoPP group is **GID 3**, virtual slices 8,9 → **physical slices 6,7** =
`FP_TCAM` indices **1536** (primary) and **1792** (secondary).

- **Key layout:** DstIp in the **top 32 bits of the 128-bit F2 field** (`F2[127:96]`,
  written LSW-first as word[3]). `FIXED_MASK=0x380`; paired half `PAIRING_FIXED_MASK=0x700`;
  secondary carries `F3_MASK=0x07fff80000`.
- **Selcodes** (`FP_PORT_FIELD_SEL`, all ports 0-52 **and 126=CPU**): SLICE8 F1=5/F2=1/F3=7,
  SLICE9 F1=0xc/F2=5/F3=0xa, `SLICE9_8_PAIRING=1`; SLICE2 5/2/7, SLICE3 0xc/3/0xa, pairing=1.
- **`FP_SLICE_ENABLE=0x000e33ff`**, `FP_TCAM_BLK_SEL=FP_GM_TCAM_BLK_SEL=0x00000fff`.
- **Action** (drop): `FP_POLICY` `G_DROP=Y_DROP=R_DROP=1`, `COPY_TO_CPU=3` (SwitchToCpuCancel).
- **First-stage gate** (the two we had to get exactly right):
  - `FP_GLOBAL_MASK_TCAM[1536]`: `IPBM=0x1fffffffffffff` (all 52 ports + CPU),
    `IPBM_MASK=0x02001fffffffffffff`. Secondary [1792] = VALID=1 only.
  - `FP_GM_FIELDS[1536]`: **`MASK=0x1fffffffff`, KEY=0x1fffffffe1`** (genuine ternary TCAM,
    distinct X/Y halves). Secondary [1792] = VALID=1 only.

---

## 4. EdgeNOS implementation (this repo)

EdgeNOS has no Broadcom SDK ACL layer; it programs the FP **by hand** through the CDK
(`cdk_xgs_mem_*`), mirroring the captured recipe.

- **Operator config:** `/etc/edged/acls.conf`. Format:
  `<name> <seq> <act> <proto> <src> <dst> [dport N]` plus a binding `<name> apply <port>`.
  `act` = `deny` (drop), `permit`, or `copy` (diagnostic copy-to-CPU). Loaded at startup and
  on **SIGHUP** (hitless — no link flap). See `core/datapath/edged.c:235,463`.
- **Two drop paths, both programmed per deny rule** (`asic/bcm56846/acl.c:edged_acl_load`):
  1. **L3 DST_DISCARD hybrid** (`l3.c:l3_v4_deny_add`) — **the shipped, working path.**
     Installs a kernel blackhole route + an `L3_ENTRY` DST_DISCARD so the chip's L3 lookup
     drops routed traffic to the dst. Independent of the FP; can't break forwarding/control
     plane. This is what actually enforces dst-IP ACLs today.
  2. **FP silicon-drop** (gated on `/etc/edged/acl_fp`) — `acl_setup_doublewide()` sets the
     slices/selcodes/pairing/enables once, then `acl_program_one()` writes FP_TCAM[1536]/[1792]
     + FP_POLICY + FP_GLOBAL_MASK_TCAM + FP_GM_FIELDS per dst. This is the aspirational
     "drop in silicon at ingress" path.
- **Diagnostics:** `SIGUSR1` → `edged_acl_diag()` dumps `FP_COUNTER_TABLE` (per-entry hit
  counts) and the programmed gate readback to `/tmp/edged-acl.log`.

### Status of the FP silicon-drop path (2026-07-17) — BLOCKED on IFP arming (definitive)
We fixed both real, SDK-verified bugs: (1) `FP_GM_FIELDS` was missing `KEY=0x1fffffffe1`
(added — matches trx/field.c:2107 for Trident), and (2) `FP_GLOBAL_MASK_TCAM` must be written
to the **X/Y pipe memories** via a PBMP-aware setter, not the combined view (added direct
`_Xm`/`_Ym` writes from the capture). **Neither made the IFP fire.**

The decisive test: a `/etc/edged/acl_gmask_any` diagnostic gate programs the global mask as
**match-any** (VALID=1, IPBM_MASK=0 = don't-care all ports), removing the port gate entirely.
With the gate wide open, the link confirmed (test-system pings swp5 0% loss → frames reach the
chip on the identical L2 path), the entry correct (`G_DROP=1`, `COUNTER_MODE=7` bound), and 500
frames flooded to the denied dst — **FP_COUNTER stayed 0** (nothing in the 2048-entry counter
table moved). Frames arrive + gate open + entry+counter correct + zero count ⇒ the **IFP lookup
engine is not processing frames at all**.

Conclusion: the blocker is the **IFP-arming wall**, not the entry/gate values. OpenMDK's minimal
init does not run the SDK's full `field_init` (hundreds of global arming writes). Static
register/memory replication cannot substitute. The `FP_GM_FIELDS` and X/Y fixes are correct and
kept (needed once the IFP is armed) but insufficient alone. **The production dst-IP drop is the
L3 `DST_DISCARD` hybrid (§4 path 1), which works end-to-end.** Reviving the FP silicon-drop
requires porting the SDK `field_init` (or the full SDK) to arm the ingress field pipeline.

---

## 4b. Rule → register derivation (the compiler, reverse-engineered)

We recovered the exact rule→register encoding by pairing the captured Cumulus
`00control_plane.rules` with the on-chip memory dump. Each rule field maps deterministically:

### (a) IP prefix `P/L` → a TCAM field (F1/F2/F3)
The prefix goes into the field the group's selcode assigns to that qualifier (SrcIp or DstIp).
In **GID 3** the value sits in the **top 32 bits of the 128-bit F2** (written LSW-first as
`F2 word[3]`):
```
value word[3] = P                         (32-bit network order)
mask  word[3] = 0xffffffff << (32 - L)     (/L prefix length)
```
Verified against the four martian **source** drops:
| Rule (`-s`)            | F2 value    | F2 mask     |
|------------------------|-------------|-------------|
| 240.0.0.0/5  (class E) | 0xf0000000  | 0xf8000000  |
| 127.0.0.0/8  (loopback)| 0x7f000000  | 0xff000000  |
| 224.0.0.0/8  (mcast)   | 0xe0000000  | 0xff000000  |
| 255.255.255.255/32     | 0xffffffff  | 0xffffffff  |

> NOTE proven on HW 2026-07-17: our EdgeNOS entry does NOT fire whether the value is
> interpreted as src OR dst (both tested, counter=0), so the src/dst selcode is *not* the
> current blocker — the ingress gate is (see §4 status).

### (b) IP type → FIXED field
`FIXED_MASK=0x380` (primary) / `PAIRING_FIXED_MASK=0x700` (paired) select the IpType/stage
bits; the value selects IPv4. (Leaving FIXED wrong was a historically-fixed edged bug —
`docs/acl-5610-double-wide-fp.md`.)

### (c) Action → FP_POLICY_TABLE[idx]
```
-j DROP           → G_DROP=Y_DROP=R_DROP=1, G/Y/R_COPY_TO_CPU=3, METER_PAIR_MODE_MODIFIER=1, COUNTER_MODE=7
-j SETCLASS N     → G/Y/R_COS_INT_PRI=N, G/Y/R_CHANGE_COS_OR_INT_PRI=5  (5 = "apply the change")
-j ACCEPT/permit  → no drop bits (entry just matches/counts)
```
Verified: martian idx 1536-1539 = `G_DROP=1,COPY_TO_CPU=3`; ICMP idx 1546 = `COS_INT_PRI=2,CHANGE=5`.

### (d) POLICE rate/burst → FP_METER_TABLE[mi] + FP_POLICY meter binding
```
-j POLICE --set-rate R --set-burst B →
   FP_METER_TABLE[mi]: REFRESHCOUNT = R  (pps, DIRECT),  PKTS_BYTES=1 (packet mode),
                       BUCKETSIZE ≈ B (token-scaled),    BUCKETCOUNT = BUCKETSIZE<<16
   FP_POLICY[idx]:     METER_PAIR_MODE=1, METER_PAIR_INDEX_{EVEN,ODD}=mi,
                       METER_TEST/UPDATE_{EVEN,ODD}=1, COUNTER_MODE=0xe, COUNTER_INDEX=<n>
```
Verified REFRESHCOUNT == rate exactly: 2000→0x7d0 (OSPF/BGP/BFD/MLAG), 300→0x12c (IGMP),
100→0x64 (ICMP/DHCP), 1000→0x3e8 (LOCAL), 400→0x190 (IPROUTER).

### (e) `--in-interface swp+` → FP_GLOBAL_MASK_TCAM[idx] + FP_GM_FIELDS[idx]
Ingress scope becomes the **IPBM** (ingress-port bitmap) gate: `IPBM` = bitmap of the matched
in-interfaces (`swp+` = all 52 front ports + CPU = `0x1fffffffffffff`), `IPBM_MASK` = the care
bits (`0x02001fffffffffffff`). `FP_GM_FIELDS` (KEY=0x1fffffffe1, MASK=0x1fffffffff) selects the
global-mask field. This pair is the first-stage gate; getting the multi-word IPBM encoding
wrong (still open in edged) leaves the lookup gated off.

### One-line summary of the compiler
```
rule = { match: prefix/len (+proto/l4), scope: in-iface, action: drop|class|police }
  →  F2/F1/F3 = prefix<<(field offset), *_MASK = prefixmask     (match)
     FIXED    = IpType bits                                      (match)
     FP_POLICY = drop-bits | (COS=class, CHANGE=5) | meter-bind  (action)
     FP_METER  = {REFRESHCOUNT=pps, BUCKETSIZE=burst}            (police)
     FP_GLOBAL_MASK_TCAM.IPBM = ingress-port bitmap              (scope)
```

## 5. Quick reference — where each thing lives

| Concept                | Cumulus                              | EdgeNOS                                  |
|------------------------|--------------------------------------|------------------------------------------|
| Operator rules         | `/etc/cumulus/acl/policy.d/*.rules`  | `/etc/edged/acls.conf`                   |
| Install/apply          | `cl-acltool -i`                      | edged startup / `kill -HUP $(pidof edged)`|
| Ingress dst-IP drop    | iptables INPUT/FORWARD -i DROP → FP  | L3 DST_DISCARD (shipped) + FP (gated)    |
| Hardware programmer     | `switchd` (Broadcom SDK)             | `edged` via CDK `cdk_xgs_mem_*`          |
| FP group for dst-IP    | GID 3, phys slices 6/7 (idx 1536/1792) | same (acl.c base 1536)                 |
| Read hit counters      | `bcmcmd "fp show"` / stats           | `SIGUSR1` → `/tmp/edged-acl.log`         |
| CoPP / control-plane   | default `00control_plane.rules` → FP | `soc_replicate` (cmp_regs) arms it        |

## Sources
- NVIDIA Cumulus Linux 3.7 — Netfilter ACLs:
  https://docs.nvidia.com/networking-ethernet-software/cumulus-linux-37/System-Configuration/Netfilter-ACLs/
- NVIDIA Cumulus Linux 3.7 — Default ACL Configuration:
  https://docs.nvidia.com/networking-ethernet-software/cumulus-linux-37/System-Configuration/Netfilter-ACLs/Default-Cumulus-Linux-ACL-Configuration/
- NVIDIA Cumulus Linux 4.4 — Netfilter ACLs (atomic/nonatomic, TCAM limits):
  https://docs.nvidia.com/networking-ethernet-software/cumulus-linux-44/System-Configuration/Netfilter-ACLs/
- Broadcom Trident 3 architecture (FleXGS / field processor):
  https://packetpushers.net/blog/broadcom-trident3-programmable-varied-volume/
- Our on-chip capture + decode: `cumulus-fp-live-capture/FP_DECODE.md`
