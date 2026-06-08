# Field Processor (FP) — what it is, and the scope to port it

Context: OSPF inbound multicast (224.0.0.5) never reaches the CPU on EdgeNOS. We
proved (via the OpenBCM SDK) the chip does **not** flood multicast to the CPU by
design; control traffic reaches the CPU only via explicit copy-to-CPU **traps**,
and for an IP-multicast-on-an-L3-port like OSPF the trap mechanism is the
**Field Processor**. This document explains what the FP is and scopes the work to
bring it up.

---

## Part 1 — What the Field Processor is

The Field Processor (FP) is the switch chip's **programmable packet classifier** —
the hardware ACL / policy engine. It's a set of TCAMs that match arbitrary packet
fields and, on a hit, apply actions (copy-to-CPU, drop, redirect, set CoS class,
police/rate-limit, count, mirror, …). Everything a NOS does with iptables-style
control-plane rules, ACLs, QoS classification, and CoPP is implemented in the FP.

### Stages
There are three FP stages, each a separate TCAM bank, applied at different points:

| Stage | When | Typical use |
|---|---|---|
| **VFP** (VLAN FP) | ingress, *before* L2/L3 lookup | VLAN assignment, pre-classification |
| **IFP** (Ingress FP) | ingress, *after* L2/L3 lookup | **control-plane traps (OSPF/BGP/…), ACLs, QoS** ← what we need |
| **EFP** (Egress FP) | egress | egress filtering/marking |

### IFP geometry on Trident+ (BCM56846, our chip)
- **9 slices**, **256 entries/slice** = 2,304 IFP rules.
- A **slice** is one TCAM block. Each slice produces a fixed-width **key** by
  extracting a chosen set of packet fields. *Which* fields a slice extracts is set
  by its **field selectors** (`FPF0/FPF1/FPF2/FPF3`, a.k.a. F1/F2/F3 in the
  register) — programmed in `FP_PORT_FIELD_SEL` (per ingress port, per slice).
- A **rule** = one TCAM entry: a `{KEY, MASK}` (in `FP_TCAM` + `FP_GLOBAL_MASK_TCAM`)
  plus an **action** (`FP_POLICY_TABLE`) and optional meter/counter
  (`FP_METER_TABLE`, `FP_COUNTER_TABLE`).

### The chip tables involved
| Table | Role |
|---|---|
| `FP_PORT_FIELD_SEL` | per-port: which field group (F1/F2/F3 selcode) each slice extracts |
| `FP_SLICE_MAP` | maps virtual→physical slices (priority ordering) |
| `FP_SLICE_KEY_CONTROL` | per-slice key sub-selectors (TOS/TTL/TCP/class) |
| `FP_GM_FIELDS` | global-mask field definitions (2048 entries) |
| `FP_GLOBAL_MASK_TCAM` | per-rule global key/mask + ingress-port-bitmap (IPBM) |
| `FP_TCAM` | per-rule key + mask (the match) |
| `FP_POLICY_TABLE` | per-rule action (copy-to-cpu, drop, redirect, …) |
| `FP_METER_TABLE` / `FP_COUNTER_TABLE` | rate-limit / stats |

### The SDK abstraction (and why it's non-trivial)
The OpenBCM SDK hides all that behind an API:
- **qset** (qualifier set): which fields you want to match (e.g. `IpProtocol`).
- **group**: a set of slices sharing a key layout, created for a qset at a priority.
- **entry**: a rule — `qualify_X(value, mask)` + `action_add(Y)`.
- **aset**: the action set.

The hard part is the **selcode engine**: `bcm_field_group_set()` auto-selects which
slice field-selector codes (`fpf0..fpf3`) satisfy the qset, and
`_bcm_field_trx_selcodes_install()` programs `FP_PORT_FIELD_SEL` accordingly. Then
each qualifier maps to a **bit offset within the slice key** — e.g. on Trident
(`src/bcm/esw/trident/field.c`) `bcmFieldQualifyIpProtocol` is in the **FPF2**
group at slice-key offset **`f2_offset(46) + 56 = 102`**, width 8. That slice-key
offset must still be translated to a **physical FP_TCAM key bit** by the key-
assembly logic — which is why raw-poking the tables by hand is error-prone.

### How OSPF maps onto this
The fix is a single IFP rule:
```
qualify IpProtocol == 89 (0x59)   →   action CopyToCpu
```
That's exactly what Cumulus's `00control_plane.rules` (`-p ospf -j SETCLASS class 7`)
compiles into. Today EdgeNOS writes `FP_TCAM` + `FP_POLICY` (replicated raw from a
Cumulus capture) but **never programs the slice infrastructure**
(`FP_PORT_FIELD_SEL`/`FP_SLICE_MAP`/…), so the chip builds keys in a different
layout than our entries expect and nothing ever matches.

---

## Part 2 — Why a port (not raw replication)

Two ways to get there; the port is the right long-term one:

1. **Raw replication** of Cumulus's captured FP state (slice config + ~2250 table
   entries + the 100 rules). Tractable but: the 100 `FP_POLICY` rows carry
   `DROP=1`, so a mis-replication **drops production traffic** on the one working
   switch (we already broke + recovered the datapath once via an EGR_MASK polarity
   mistake; FP DROP rules are worse — they persist across an edged restart). And it
   only gets us Cumulus's exact rule set, frozen.

2. **Port the SDK field module** — bring the OpenBCM `bcm_field` engine (group /
   entry / qualifier / action / selcode / key-assembly) into edged. Larger, but it
   makes the FP *programmable*: OSPF, BGP CoPP, ACLs, QoS, storm control all become
   a few API calls, correctly, with no hand-built keys. This is the foundation a
   full-featured NOS needs anyway.

We now have the complete `OpenBCM/sdk-6.5.27` source (incl. `src/bcm`), so the port
is a guided effort, not reverse-engineering.

---

## Part 3 — Port scope (phased, each phase independently verifiable)

Guiding principle: **no `DROP` actions until the engine is trusted** — every phase
uses `CopyToCpu` only, so a wrong key just fails to match (no datapath regression).
Keep a known-good `edged` backed up on the box; ping the Nexus after every deploy;
remember FP-enable/`APPLY_*` bits persist across an edged restart, so recovery must
explicitly clear them (or reboot).

### Phase 0 — Harness (low risk, do first)
- Add an `edged` FP read-back diag (SIGUSR1 or `/tmp` trigger): dump
  `FP_PORT_FIELD_SEL[port]`, `FP_SLICE_MAP[0]`, `FP_SLICE_KEY_CONTROL[0]`, and a
  given `FP_TCAM`/`FP_POLICY` index.
- Extend the existing `/tmp/regdump.in` path to cover these memories so we can diff
  live-vs-Cumulus-SOCMEM field-by-field.
- **Exit criteria:** we can read every FP table we intend to write.

### Phase 1 — Port the device stage init (slice setup) ✅ DONE (commit 2a5d860)
- From `src/bcm/esw/trident/field.c`: port `_field_trident_ingress_qualifiers_init`
  (the qualifier→offset table) and the IFP **stage/slice setup** that programs
  `FP_SLICE_MAP`, `FP_PORT_FIELD_SEL`, `FP_SLICE_KEY_CONTROL`, and the
  `FP_GM_FIELDS` defaults.
- Cross-check the resulting register values against the captured Cumulus SOCMEM
  (`dump_socmem.txt.gz`) — they should match field-for-field.
- **Exit criteria:** one IFP slice is configured for the **FPF2 (IP/L4)** group and
  reads back identical to Cumulus; ping still 0%.
- **Result:** implemented directly in `cumulus_replicate_fp()` (slice config
  replicated byte-for-byte from the capture rather than re-deriving via the SDK
  selcode engine — that's Phase 2). FP-DIAG read-back on 10.1.1.217 confirms
  `PORT_FIELD_SEL[1] S2[F1=0xc F2=2 F3=7] S3[F1=0xa F2=3 F3=6]` (was all-0),
  `GLOBAL_MASK_TCAM[256] VALID=1` (was 0), `POLICY[256] COPY_TO_CPU=3 DROP=0`,
  100 TCAM + 100 POLICY rows, errors=0. **All `FP_POLICY.*_DROP` forced to 0**
  (de-risk). Ping Nexus .2 and .9 both 0%.

### Phase 2 — Port the minimal selcode + key-assembly
- From `src/bcm/esw/field.c` + `field_common.c`: port the subset that, given a qset
  containing `IpProtocol`, (a) picks the FPF2 selcode, (b) computes the **physical
  FP_TCAM key bit** for the qualifier (translate slice-key `f2_offset+56` → physical
  key bit), and (c) builds `{KEY, MASK}`.
- Validate by reconstructing a **known captured rule** (e.g. the IGMP proto-2 or
  ICMP proto-1 rule) and confirming our computed key/mask matches the captured bytes.
- **Exit criteria:** we can produce a byte-exact `FP_TCAM` key for "IpProtocol == N".

### Phase 3 — The OSPF rule — IN PROGRESS, blocked on double-wide group install

Findings (commits 68155a7, 365627e):
- **OSPF is trapped by DESTINATION, not protocol.** Cumulus's IFP rule 1538
  (physical slice 6, selcode F2=1) matches `DstIP first-octet = 0xe0`
  (224.0.0.0/8 multicast) → `COPY_TO_CPU`. OSPF 224.0.0.5/6 falls in that
  range. The rule is **already in our replicated 100** — no new rule needed.
- **Two real activation gaps were found and fixed:**
  1. **IPBM (ingress-port-bitmap):** the captured global mask only covered
     Cumulus's uplink ports (0..52) and forced bit 65 to 0, EXCLUDING our
     uplinks (physical ports 65/66). Fixed → write match-any IPBM (KEY=0,
     MASK=0, VALID=1).
  2. **`FP_SLICE_ENABLE` was never set** by OpenMDK's `bmd_init` — the master
     IFP lookup enable. Set to `0x000e33ff` (Cumulus value; virtual-slice
     indexed; LOOKUP_ENABLE for VS2,3,7,8,9). The engine now activates.
- **Remaining blocker:** the OSPF rule's virtual slice (VS8) is an
  **inter-slice double-wide pair** (VS8=DstIP half in phys6, VS9=DstMAC half
  in phys7; `SLICE9_8_PAIRING=1`). A match-any test on the paired entry still
  copies ZERO frames over 55s with the engine on, while the chip is receiving
  the OSPF multicast (RMCA increments, no drops). So a double-wide group needs
  the SDK's group/key-assembly/mode-install sequence (Phase 2) beyond the raw
  visible table values. **This is the next step: port the double-wide group
  install, OR construct a single-wide OSPF trap in a slice we fully configure.**

Diagnostics added (read-only, safe): `packet_io` RX counters
(`g_rx_total/unmapped/delivered`); RX-DIAG dumps live `FP_PORT_FIELD_SEL` for
the real uplink ports 65/66, `FP_SLICE_MAP`, `FP_TCAM/POLICY/GLOBAL_MASK` for
rules 1538/1553, and FP-gating regs (`ING_BYPASS_CTRL`/`AUX_ARB_CONTROL[_2]`/
`MISCCONFIG`/`EFP_METER_CONTROL`). rsyslog is down on the box — read via
`journalctl -u edged`, not `/var/log/daemon.log`.

#### Original Phase 3 plan (still the target)
- Program one IFP entry: `FP_TCAM` key = IpProtocol 0x59 (+ the slice/IPBM bits),
  `FP_GLOBAL_MASK_TCAM` = permissive (all ingress ports), `FP_POLICY` =
  `COPY_TO_CPU` (CoS class 7, **no DROP**), `FP_METER` optional rate-limit.
- Enable the slice / `FP` for that entry.
- **Exit criteria:** `tcpdump -i swp1 ip proto ospf` shows the Nexus hellos at the
  CPU → `ospfd` adjacency reaches **Full** → routes install via the FIB→chip ECMP
  path we already have. Ping still 0%.

### Phase 4 — Generalize to the `bcm_field` API (full-featured)
- Wrap the ported engine in the OpenNSL-style API surface: `group_create(qset)`,
  `entry_create`, `qualify_*`, `action_add`, `group_install`.
- Re-implement the control plane as API calls: OSPF/BGP/BFD/IGMP/ICMP/ARP/DHCP CoPP
  (matching `00control_plane.rules`), each with a police meter.
- This unlocks ACLs and QoS as well.
- **Exit criteria:** the full `control_plane.rules` set is expressible and installed
  via the API (replacing the raw-replicated 100 rows), with meters.

---

## Part 4 — Files to port (from `OpenBCM/sdk-6.5.27`)
| File | What to take |
|---|---|
| `src/bcm/esw/trident/field.c` | device stage init, qualifier offset table, slice setup, `FP_SLICE_MAP` write |
| `src/bcm/esw/field.c` | generic group/entry/qualifier/action engine + selcode selection + key assembly (port the minimal subset) |
| `src/bcm/esw/field_common.c`, `field_reg_mem.c` | helpers, register/memory glue |
| `include/bcm/field.h`, `include/bcm_int/esw/field.h` | qualifier/action enums, structs |

## Cross-reference assets we already have
- **Cumulus SOCMEM/SOC dumps** — ground-truth values for every FP table.
- **The 100 captured `FP_TCAM`/`FP_POLICY` rows** (`asic/edged/generated/`) — to
  validate key construction against real rules.
- **Decompiled Cumulus `libopennsl`** (`edgecore-5610-reverse-engineering/.../ghidra-analysis/`,
  `FP_MMU_AND_REMAINING.md`) — the exact `bcm_field` call sequence for our chip.

## Risk register
- `FP_POLICY.DROP=1` and `APPLY_*`/FP-enable bits **persist across edged restart**
  (read-modify-write init preserves them). Recovery = explicit-write-0 or reboot.
- Keep `edged.goodbak` on the box; ping-verify each deploy; CopyToCpu-only until
  Phase 4. One physical switch — do not risk it on un-verified DROP rules.

---

## Part 5 — Session log: everything tried (2026-06-07 → 2026-06-08)

Chronological record of the OSPF-punt / Field-Processor bring-up effort so the
next session (or reader) does not repeat eliminated paths. **Headline result:
the FP engine works; the one unsolved piece is FP `COPY_TO_CPU` delivery to the
CPU RX DMA.**

### Phase 1 — slice infrastructure (DONE, commit 2a5d860)
Root cause of "FP never matched": `edged` wrote `FP_TCAM`/`FP_POLICY` but never
configured the slices. Replicated byte-for-byte from the Cumulus SOCMEM capture
(`cumulus_replicate_fp()`):
- `FP_PORT_FIELD_SEL` (all ports): SLICE2 F1=0xc/F2=2/F3=7, SLICE3 F1=0xa/F2=3/F3=6
  (3_2_PAIRING=1), SLICE8 F1=5/F2=1/F3=7, SLICE9 F1=0xc/F2=5/F3=0xa (9_8_PAIRING=1).
- `FP_SLICE_MAP[0]` (virtual→physical + group ids), `FP_SLICE_KEY_CONTROL[0]`
  (SLICE_2/9 DST_CLASS_ID_SEL=1).
- **De-risk: every `FP_POLICY.*_DROP` forced to 0** (captured rows carry DROP=1;
  COPY-only until trusted — protects the one working switch).
Verified live via the SIGUSR1 FP-DIAG read-back; ping 0% both uplinks.

### Key decode facts (authoritative)
- **OSPF is trapped by DESTINATION, not protocol.** Cumulus IFP rule 1538
  (physical slice 6, selcode F2=1) matches DstIP first-octet `0xe0` = 224.0.0.0/8
  multicast → `COPY_TO_CPU`. OSPF 224.0.0.5/6 is in range. **Already in our
  replicated 100** — no new rule needed for parity.
- Indexing: `FP_TCAM` index = physical slice × 256. `FP_PORT_FIELD_SEL` and
  `FP_SLICE_ENABLE` are **virtual-slice indexed**; `FP_SLICE_MAP` routes
  virtual→physical. Valid rules live at non-contiguous idx 256-279/384-407/
  1536-1561/1792-1817 = physical slices 1,6,7.
- Selcode meaning (empirical): F2=1 → DstIP at top of F2 (bits 96-127, first
  octet at 120-127); F2=5 → DstMAC+EtherType (LLDP/LACP/CDP MACs, ARP 0x0806).
- FPF2 IP/L4 layout (SDK trident/field.c): f2_offset=46; IpProtocol at slice-key
  bit 102 (= F2 bit 56), DstIp at +64, L4DstPort +24, etc.
- `FP_TCAM` subfield accessors place fields physically: `F2f_SET` = bits 48..175,
  `FIXEDf_SET` = bits 219..235 — so you can set DstIP/proto without hand-deriving
  the physical key.

### Activation registers `bmd_init` never sets (all now set, all verified-needed)
1. `FP_SLICE_ENABLE` = `0x000e33ff` (commit 365627e) — master IFP lookup enable
   (SLICE_ENABLE bits0-9 + LOOKUP_ENABLE for VS2,3,7,8,9). Later `0x000f33ff`
   (+bit16) to add our VS6 trap slice.
2. `FP_TCAM_BLK_SEL` = `FP_GM_TCAM_BLK_SEL` = `0x00000fff` (commit 2e237db) —
   enable all 12 key-TCAM + global-mask-TCAM blocks for search.
Gating confirmed ON live: ING_BYPASS=0, AUX_ARB_2=0x327f863 (FP refresh on),
PORT_TAB FILTER_ENABLE=1.

### The IPBM fix (commit 68155a7)
Captured per-rule global mask only covered Cumulus's uplink ports (0..52) and
forced bit 65 to 0 — which EXCLUDED our uplinks (physical ports **65/66**). Now
write a match-any IPBM (`KEY=0, MASK=0, VALID=1`) so traps apply on any ingress
port — correct semantics for a control-plane copy rule.

### THE breakthrough — FP matches; counters lie (commit 64bb9b0)
User-authorized decisive tests on a self-built single-wide rule (VS6 → phys
slice 4, idx 1024):
- **match-any DROP → BOTH uplinks 100% loss** (clean revert after). **The FP
  ENGINE MATCHES.** All activation above succeeded.
- **`FP_COUNTER_TABLE` is unreliable** without pool/base setup — read ALL-ZERO
  while the rule was definitively matching. So every earlier "counter=0 ⇒ no
  match" was a FALSE NEGATIVE (it had misdirected Path A/B).
- **match-any COPY → delivers nothing to the CPU RX.** Same rule DROPs
  everything but COPYs nothing. **⇒ the blocker is `COPY_TO_CPU` delivery, not
  matching.**
- Deploy gotcha: overwrite a running `/usr/sbin/edged` with `mv` (rename), not
  `cp` (Text file busy). Mgmt (end0, 10.1.1.217) is independent of the uplinks,
  so DROP tests are recoverable.

### DMA / delivery facts
- RX uses the XGS **packed** CMIC_DMA registers at **0x100** (single-DCB model,
  `bcm56840_a0_bmd_rx.c`). The **CMICm registers at 0x31xxx do NOT accept writes
  on this chip** — every 0x31xxx read returns 0 yet DMA works — so any
  `COS_CTRL_RX` (0x31168) / `cumulus_enable_cmicm_irq` diag is DEAD (bogus reads).

### Delivery leads ELIMINATED
- ❌ CPU-port egress membership — `EPC_LINK_BMAP[0]` already includes CPU port
  (bit 0); ping proves egress-to-CPU works.
- ❌ RX-channel CoS bitmap — set `CMIC_CONFIG.COS_RX_EN=0` (working 0x10c path,
  in `bcm56840_a0_bmd_rx_start`) so the single channel drains ALL CoS. No change.
- ❌ CoS-queue selection — `FP_POLICY CHANGE_CPU_COS=1 + CPU_COS=0` to steer the
  copy to CoS0. match-any COPY + tcpdump for Nexus OSPF (src .2) = 0 captured.
- ❌ Key layout — match-any is layout-independent and still doesn't deliver.

### Delivery leads STILL OPEN (next-session candidates)
1. **MMU CPU-port queue buffer/credit** — copy may be dropped at MMU enqueue if
   its queue has no buffers. Read CPU-port (port 0) THDI/THDO/queue-drop counters
   under a match-any COPY. (read-only, safe)
2. **Global copy/redirect-to-CPU enable** the IFP needs that `bmd_init` skips.
3. **`COPY_TO_CPU` encoding** — used value 3 (Cumulus's); confirm semantics.
4. **Structural**: the XGS single-DCB RX may only catch L3-dest-CPU traffic, not
   IFP copies → may require the proper CMICm RX (blocked by 0x31xxx-write issue)
   or a fresh Cumulus capture of the exact CPU-copy DMA/queue path.

### Current box state (10.1.1.217, EdgeNOS)
Clean + datapath-safe: FP fully activated (slice cfg, SLICE_ENABLE, BLK_SEL),
match-any IPBM, VS6 single-wide COPY trap (DstIP 224.0.0.0/8, CPU_COS=0,
COPY-only/no-DROP, harmless — non-delivering), `COS_RX_EN=0`. Ping 0% both
uplinks. `edged.goodbak` intact. (`COS_RX_EN` change lives in the gitignored
`asic/openmdk` tree — on-disk + in binary; capture as an openmdk patch if kept.)

### Caveats / gotchas for the reader
- `FP_COUNTER_TABLE` reads are NOT a reliable match signal here (need extra
  setup). Use a DROP test (forwarding-observable) or tcpdump for ground truth.
- rsyslog is DOWN on the box — read edged logs via `journalctl -u edged`, not
  `/var/log/daemon.log`.
- A match-any **COPY** "flood" test is misleading: most traffic is already
  CPU-bound (ping/ARP), so COPY adds little. Use tcpdump for the specific
  non-CPU flow (Nexus OSPF src .2) instead.
