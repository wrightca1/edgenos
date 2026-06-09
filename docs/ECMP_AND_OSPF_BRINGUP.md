# How ECMP and OSPF Were Brought Up on EdgeNOS (AS5610-52X)

A detailed account of getting **hardware ECMP** and **OSPF dynamic routing**
working on the Broadcom BCM56846 (Trident+) with the open-source OpenMDK stack —
the architecture that made it tractable, the chip programming required, the
~90-cycle blocker that stood between "OSPF sends hellos" and "adjacency FULL,"
and how it was finally cracked.

Audience: someone who knows L3 networking but not this chip. File/function/
register names are real and current.

---

## 0. The architectural key: edged mirrors the Linux FIB into the chip

The single most important design decision is that **edged owns no routing
policy.** It listens to the Linux kernel over netlink and programs whatever the
kernel decides into the ASIC's hardware tables:

```
ip / Quagga(OSPF) ──netlink RTM_*──▶ edged ──S-channel/table writes──▶ BCM56846
   (kernel FIB)                       (agent)                          (silicon)
```

Consequence: **any routing daemon that installs routes into the kernel FIB gets
hardware forwarding for free.** OSPF therefore reduced to two sub-problems:

1. Run an OSPF daemon that installs routes into the kernel (→ edged programs the chip). *Easy.*
2. Get OSPF's **control traffic** (hellos, DBD, LSAs) to the **CPU**, so the
   daemon can actually form an adjacency. *This was the hard part.*

ECMP fell out of (1): a multipath kernel route becomes a chip ECMP group.

---

## 1. ECMP (the easy win)

### 1.1 Path: kernel multipath route → chip ECMP group

`netlink.c` parses `RTM_NEWROUTE`. A route with an `RTA_MULTIPATH` attribute
(multiple next-hops) is handed to **`l3_route_add_paths(dst, prefix_len, gw[],
ngw)`** in `l3.c`; a single-next-hop route goes to `l3_route_add()`.

`l3_route_add_paths()` (l3.c):
1. Resolves each gateway IP to its **chip next-hop index** via
   `l3_neigh_nh_lookup(gw)`. That next-hop was programmed earlier by
   `l3_host_add()` when the kernel ARP-resolved the gateway (RTM_NEWNEIGH →
   ING/EGR_L3_NEXT_HOP + EGR_L3_INTF). Unresolved gateways are skipped.
2. Writes an **L3_DEFIP** TCAM entry for the prefix:
   - `ngw == 1`: `ECMP0=0`, `NEXT_HOP_INDEX0 = nh`.
   - `ngw > 1`: build an **L3_ECMP group** with `l3_ecmp_group_create(nh[], n)`,
     then `ECMP0=1`, `ECMP_PTR0 = group`.
3. The chip then **hashes each flow** across the group members → load-balance
   across the swp uplinks.

`l3_ecmp_group_create()` allocates a group slot and writes the member table
(the next-hop indices the hash selects among).

### 1.2 The ordering subtlety: resolve gateways *before* installing the route

A DEFIP/ECMP entry can only point at next-hops that already exist in the chip.
So `swp-l3-config.sh` (which loads `/etc/edged/swp-routes.conf`) **pings each
gateway first** to force the kernel to ARP-resolve it — which makes edged
program that next-hop — and only then runs `ip route replace … nexthop … nexthop
…`. Without the pre-resolve the route would install with missing paths.

`swp-routes.conf` format (one route per line, multiple gateways = ECMP):

```
<dst/plen>   <gw1>:<dev1>  [<gw2>:<dev2> ...]
# e.g.  10.200.200.0/24  10.101.101.2:swp1  10.101.101.9:swp2
```

### 1.3 Result

A two-way ECMP transit route load-balanced **9/9 flows across swp1/swp2**,
verified live. (See the memory `project_5610_ecmp`.) Once OSPF was running, the
same machinery means OSPF equal-cost routes auto-become chip ECMP groups.

---

## 2. OSPF: the daemon (straightforward)

**Quagga 1.2.4, cross-compiled static** (Cumulus 2.5 shipped Quagga; modern
FRR's libyang/cmake make a static PPC cross-build painful). The reproducible
recipe is in `scripts/build-quagga.sh`; the load-bearing details:

- `gawk` required (configure rejects mawk).
- Stub the two `crypt()` call sites (VTY password hashing, unused — no static
  libcrypt for the cross target).
- `CFLAGS="-O2 -fcommon"` — **`-fcommon` is essential** (GCC10+ `-fno-common`
  → "multiple definition of `__packed`" in prefix.h).
- `--enable-user=root --enable-group=root --enable-static --disable-shared`,
  only `zebra` + `ospfd` enabled.
- Quagga's own `-d` daemonize exits on this box → run **foreground under
  systemd `Type=simple`** (`zebra.service`, `ospfd.service`).

With the daemon up, `ospfd` immediately **sent hellos** out swp1 (verified:
`10.101.101.1 > 224.0.0.5`), and the Nexus showed us in `INIT`. Egress worked.
**The adjacency would not progress past INIT** — the inbound OSPF packets never
reached our CPU. That's the rest of this document.

---

## 3. The blocker: control traffic never reached the CPU

The chip is a hardware switch — it does **not** flood arbitrary traffic to the
CPU. Control packets reach the CPU only via **explicit copy/redirect-to-CPU
traps**. ARP worked because there's a dedicated `PROTOCOL_PKT_CONTROL`
ARP-to-CPU trap. OSPF had no such trap, and OSPF is awkward two ways:

- **Hellos** go to **224.0.0.5** (a multicast destination MAC) — L2-switched,
  not L3-routed, so the L3 host-route CPU punt doesn't catch them.
- **Adjacency packets** (DBD/LSR/LSU to our unicast swp IP) are **TTL = 1** — the
  chip drops TTL=1 unicast on L3 termination unless a specific trap allows it.

Diagnosis confirmed the chip **received** the OSPF multicast (an RMCA rx-multicast
counter incremented on swp1/swp2) but generated **no CPU copy** and did **not
drop** it (`rdbgc*`=0). The copy simply wasn't being produced.

### 3.1 What it took just to make the Field Processor *able* to match

OpenMDK's `bmd_init` under-initializes the chip vs. a full SDK `init all`; the
**Field Processor (IFP) was completely dead.** Activating it (in
`cumulus_replicate.c::cumulus_replicate_fp()`, replicated byte-for-byte from a
captured Cumulus SOCMEM dump) required:

- **Slice infrastructure**: `FP_PORT_FIELD_SEL` (per-slice key field selectors),
  `FP_SLICE_MAP` (virtual→physical slice map), `FP_SLICE_KEY_CONTROL`.
- **`FP_SLICE_ENABLE = 0x000f33ff`** — `bmd_init` never sets this, so the IFP
  held valid entries but looked up *no* slice.
- **`FP_TCAM_BLK_SEL = FP_GM_TCAM_BLK_SEL = 0x00000fff`** — enable the key-TCAM
  and global-mask-TCAM blocks for search (two more activation regs `bmd_init`
  skips).
- **Match-any IPBM** in `FP_GLOBAL_MASK_TCAM` (KEY=0/MASK=0/VALID=1) so the rules
  match on our uplink ports (the captured global-mask excluded ports 65/66).
- The **OSPF trap rule itself**: a single-wide VS6 entry matching **DstIP
  224.0.0.0/8** (F2 subfield `a[3]=0xe0000000`, mask `0xff000000`) →
  action `COPY_TO_CPU`.

A user-authorized **match-any DROP** test then proved the engine matched (both
uplinks went 100% loss, cleanly reverted). But the matching **COPY** action
delivered **nothing** to the CPU. So the wall wasn't matching — it was the
*copy delivery itself*.

### 3.2 The dead ends (so the next person doesn't re-walk them)

All of these were tried and **ruled out** (each is real chip config, left in as
SDK-aligned groundwork): L2 flood-to-CPU via `EGR_MASK`/`VLAN_PROFILE_2`
block-masks (this actively broke the datapath and had to be reverted —
`APPLY_EGR_MASK` persists across an edged restart, so recovery needs an explicit
write-0 or reboot); `L2_USER_ENTRY` exact-MAC traps for 01:00:5e:00:00:05;
`CPU_CONTROL_1` UMC/IPMC-miss bits; `CPU_PBM` (CPU-port replication bitmap);
`CPU_COS_MAP`; `COS_RX_EN`; CoS-queue forcing (`CHANGE_CPU_COS`); `EPC_LINK_BMAP`
CPU-port membership; and the FP action encoding (COPY_TO_CPU=3 vs SDK's 1).
~90 build/test/reboot cycles total. `FP_COUNTER_TABLE` reads were a recurring
**false negative** (all-zero even while a rule was definitively matching — it
needs pool/base setup), which invalidated several earlier "no match" conclusions.

---

## 4. The breakthrough: a *copy-to-CPU is a replication*

The fix came not from another one-off guess but from a **systematic full-register
diff**. A "REGDIFF" harness read **every one** of the ~335 config registers on
our chip *by the addresses from Cumulus's `dump_soc.txt`* (OpenMDK chip-def
addresses match the Cumulus dump addresses) and logged every gap. That one diff
surfaced, in minutes, a cluster of registers that were **non-zero on Cumulus and
zero on ours** — the **MMU multicast/copy-replication** block that *gates* the FP
`COPY_TO_CPU` action:

| Register | Cumulus value |
|---|---|
| `MC_CONTROL_1` | `0x10000000` |
| `MC_CONTROL_2` | `0x10001000` |
| `MC_CONTROL_3` | `0x10002000` |
| `MC_CONTROL_5` | `0x02001000` |
| `EGR_MC_CONTROL_1` | `0x10000000` |
| `EGR_MC_CONTROL_2` | `0x00002000` |
| `MCQ_CONFIG` | `0x0f000000` |
| `SW2_FP_DST_ACTION_CONTROL` | `0x0000000c` |

The insight: **a COPY_TO_CPU is internally a multicast *replication* to the CPU
port.** Without the MMU replication engine configured (`MC_CONTROL_*` /
`EGR_MC_CONTROL_*` / `MCQ_CONFIG`) and the FP destination-action plumbing
(`SW2_FP_DST_ACTION_CONTROL`), the chip accepts the FP rule, *matches* it (DROP
proves that), but **never generates the replicated copy** — which is exactly why
delivery was zero while drops were zero. Programming these to the Cumulus values
(in `cumulus_replicate.c`) made the **Nexus OSPF hellos (224.0.0.5) reach the
CPU** through the VS6 224/8 trap.

Then the last piece for the *unicast* adjacency exchange — the **TTL=1 traps**,
also found in the same diff (part of Cumulus's `CPU_CONTROL_1 = 0x18500600`),
added in `datapath.c::datapath_cpu_punt_init`:

```c
CPU_CONTROL_1r_L3UC_TTL1_ERR_TOCPUf_SET(cpu_ctrl1, 1);
CPU_CONTROL_1r_IPMC_TTL1_ERR_TOCPUf_SET(cpu_ctrl1, 1);
```

OSPF DBD/LSR/LSU packets are TTL=1 and L3-terminated on our swp IP; the chip was
dropping them as TTL-expired until these traps punted them instead.

**Result (commit `1e40a13`):**

```
ospfd: Neighbor 10.101.1.241 Negotiation done (Master), Loading -> Full
```

Stable, bidirectional, ping 0% on all uplinks throughout.

### The lesson (do this first next time)

When "Cumulus sets X that we don't" keeps recurring, **do the systematic full
register diff up front.** Reading our chip by Cumulus's dump addresses and listing
every delta found in minutes what ~90 one-at-a-time cycles could not.

---

## 5. OSPF as deployed (the network design)

Topology — all to the same Cisco Nexus, `vrf routed1`, `ospf 101 area 0`:

| Link | EdgeNOS | Nexus | Speed | OSPF cost |
|---|---|---|---|---|
| swp1 | 10.101.101.1/29 | .2 | 10 G | 40 |
| swp2 | 10.101.101.10/29 | .9 | 10 G | 40 |
| swp49 | 10.101.101.18/29 | .17 | **40 G** | **10** |
| lo | 10.101.101.241/32 | — | — | advertised |

`/etc/quagga/ospfd.conf`:
- **router-id = 10.101.101.241** (the loopback — stable, interface-independent).
- `network … area 0` for each /29 + the loopback /32; `passive-interface lo`.
- **Explicit `ip ospf cost`** per interface (not `auto-cost`): the `swpN` ports
  are **TAP** interfaces with no kernel-known speed, so `auto-cost
  reference-bandwidth` can't infer 40 G vs 10 G. Costs are set by hand —
  swp49=10 (primary), swp1/swp2=40 — a 1:4 ratio mirroring bandwidth, so traffic
  prefers the 40 G and falls back to the two 10 G as an **equal-cost (ECMP)
  pair** if it drops.

Because edged mirrors the FIB, any prefix the Nexus advertises (incl.
equal-cost ones) is programmed to the chip automatically — single-path as
L3_DEFIP, multipath as an L3_ECMP group (§1).

---

## 6. Foundations this depended on

- L3 hardware forwarding chain (MY_STATION_TCAM, L3_DEFIP, ING/EGR_L3_NEXT_HOP,
  EGR_L3_INTF) and local-host CPU punt — see `docs/DATAPATH_BRINGUP.md`.
- The FP engine activation + Cumulus chip-memory replication —
  `asic/edged/cumulus_replicate.c`.
- CPU-punt traps — `asic/edged/datapath.c::datapath_cpu_punt_init`.
- Route/ECMP programming — `asic/edged/l3.c` (`l3_route_add_paths`,
  `l3_ecmp_group_create`, `l3_host_add`).
- Daemon build — `scripts/build-quagga.sh`; services in the image overlay.

---

## 7. Timeline (condensed)

1. Quagga built + running; hellos egress; Nexus sees us INIT. Stuck.
2. Long investigation: chip RX's the mcast but no CPU copy, no drop.
3. Activate the FP engine (slices, `FP_SLICE_ENABLE`, `*_TCAM_BLK_SEL`, IPBM);
   build the 224/8 COPY trap. DROP proves match; COPY delivers nothing.
4. ~90 cycles eliminating flood/CoS/CPU_PBM/CPU_COS/key-layout/action-encoding.
5. **Systematic register diff → MMU copy-replication regs (`MC_CONTROL_*` etc.)**
   → hellos reach CPU.
6. **TTL=1 traps** → unicast DBD exchange completes → **adjacency FULL** (`1e40a13`).
7. Later: loopback router-id + per-interface cost (40 G primary / 10 G ECMP
   backup); baked into the image.

*See also: `project_5610_ospf_quagga` and `project_5610_ecmp` (memory),
`docs/EDGED_ARCHITECTURE_AND_OPERATIONS.md`, `docs/DATAPATH_BRINGUP.md`.*
