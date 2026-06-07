# Bringing up a Trident+ datapath without the SDK: the ordering problem

A technical deep-dive for engineers. The hard part of writing a switch NOS from
scratch isn't any single register — it's that the chip is a **dependency-ordered
state machine of several hundred register and table writes**, and the open toolkit
gives you maybe a tenth of them and *none* of the ordering. This is the story of
reconstructing that sequence on a **Broadcom BCM56846 (Trident+ / "trident")** in
EdgeNOS, using a working **Cumulus Linux** install on the same silicon as an oracle.

Redact internal IPs/MACs before sharing publicly.

---

## 0. Why "ordering" is the whole game

Broadcom's production SDK has `soc_init()` / what the CLI calls `init all`. It is not
a flat list — it's a partially-ordered graph with hard dependencies:

- You cannot insert into a hashed table before its **hash function/seed control** is
  programmed, or the chip computes a different bucket than your software lookup.
- You cannot get a TCAM hit before the **CAM is enabled** (`*_CAM_ENABLE`).
- You cannot program per-port tables meaningfully before the **port map** (physical
  SerDes lane → logical/device port) exists, or you scribble into the wrong rows.
- A lookup stage won't *run* until its **per-port and per-VLAN enables** are set —
  and those live in different tables than you'd guess.
- Memories often need **parity/ECC init** before the pipeline will trust them.
- The MMU/buffer/scheduler config must exist before frames can be enqueued to the CPU.

OpenMDK's `bmd_init` is a thin attach + minimal switching init. It links up ports and
does basic L2, but it **skips most of the L3/foundation writes `soc_init` does** — and
gives no hint about ordering. So the work is: *recover the missing writes AND the order
they must happen in.* Our reference: ~600 MB of captured Cumulus live state (register
dumps, table dumps, init `strace`/register traces) plus the full SDK source for intent.

---

## 1. The bring-up, in dependency order (as we reconstructed it)

Roughly the sequence we converged on (each stage gated by the previous):

1. **PCIe / register access path.** On this iProc-based design, CMICm registers above
   a window need a **PAXB AXI→BAR0 sub-window remap** (`IMAP0_7`, 8×4KB windows, with a
   read-back). Get this wrong and half your "writes" land nowhere — silently.
2. **Port map.** `P2L`/`L2P` — physical SerDes lane ↔ logical/device port. *Everything*
   per-port downstream indexes by **logical** port; the SerDes lane is a different
   number space. (This trap bit us twice — see §3.)
3. **SerDes / Warpcore PHY init** → PCS `block_lock` (10G) or AM-lock + deskew (40G).
4. **MAC config:** `XMAC_*` control, per-port frame-size limits, CRC mode.
5. **MMU / buffers / CPU queues:** thresholds, scheduling, CPU CoS queues, the RX DMA
   ring — so a "to CPU" decision actually has somewhere to go.
6. **VLAN / STG:** create VLANs, set membership + PVID, **STP = forwarding** in the
   right spanning-tree group.
7. **L2:** station MACs, learning mode, CPU punt entries.
8. **L3 enables + objects** (the part that ate days — §2).
9. **CPU punt / protocol traps:** `CPU_CONTROL_*`, protocol-packet control.
10. **DMA arm:** descriptors, controlled-interrupt mask, completion.

Miss a dependency and the symptom is almost never an error — it's a **silent drop**
three stages later.

---

## 2. The L3 datapath: the object model and every enable that gates it

The Trident L3 forwarding chain for a routed/terminated IPv4 packet:

```
ingress frame
  → MY_STATION_TCAM      (is dst MAC "one of mine"? {MAC, VLAN} → terminate to L3)
  → [L3 lookup runs ONLY if the port+VLAN L3-enables are set]
       → L3_ENTRY_IPV4_UNICAST   (hashed exact-match host table; KEY = IP<<3, {VRF})
       → L3_DEFIP                (TCAM LPM; subnets/defaults; PAIR mode = 2 routes/line)
  → ING_L3_NEXT_HOP / EGR_L3_NEXT_HOP   (COPY_TO_CPU, or dst MAC rewrite + egress port)
  → EGR_L3_INTF             (src MAC + egress VID)
  → egress
```

The tables are the easy part. The **enables that decide whether the lookup even
executes** are scattered and individually quiet about being wrong:

| Gate | Where it lives | Gotcha |
|---|---|---|
| Terminate to L3 | `MY_STATION_TCAMm` `{MAC, VLAN_ID, *_TERMINATION_ALLOWED, VALID}` | mask `ING_PORT_NUM_MASK=0` (any port); fields are *necessary but not sufficient* |
| Per-port "do IPv4 L3" | **`PORT_TABm[logical_port].V4L3_ENABLE/V6L3_ENABLE`** | **NOT `LPORT_TABm`** — see below |
| Per-VLAN "do IPv4 L3" | `VLAN_PROFILE_TABm[ptr].IPV4L3_ENABLE`, where `ptr = VLAN_TABm.VLAN_PROFILE_PTR` | profile-indexed, not VLAN-indexed |
| LPM TCAM searchable | `L3_DEFIP_CAM_ENABLE` (= `0x3ff`), `L3_DEFIP_128_CAM_ENABLE` | default-off paths exist |
| Hash agreement | `HASH_CONTROL`, `L3_AUX_HASH_CONTROL`, `RTAG7_*` | SW insert and HW datapath must use the same hash |
| Ingress pipeline hit-enables | `ING_CONFIG_64` `L2DST_HIT_ENABLE`, `L3SRC_HIT_ENABLE`, `APPLY_EGR_MASK_ON_L2/L3`, `ARP_RARP_TO_FP` | some bits require subsystems (FP/egress masks) you haven't built yet — turning them on *causes* drops |
| Ingress VRF | `L3_IIFm[iif].VRF` (default 0); on this chip the per-port default IIF/VRF comes from `SOURCE_TRUNK_MAP_TABLE`, **not** `VLAN_TAB` (no L3_IIF field there) | VRF mismatch → HW lookup misses a SW-visible entry |

### The PORT_TAB vs LPORT_TAB trap (the bug that cost the most)

Both `PORT_TABm` and `LPORT_TABm` have a `V4L3_ENABLE` bit at the **same bit offset**,
identical field layout. But:

- `PORT_TABm` (MAX 66) is the **per-logical-port** table the ingress pipeline reads.
- `LPORT_TABm` (MAX 127) is a **profile** table indexed by
  `SOURCE_TRUNK_MAP_TABLE.LPORT_PROFILE_IDX` — which is 0 for every un-provisioned port.

We were enabling L3 in `LPORT_TABm[logical_port]`. The chip never read those rows, so
`V4L3_ENABLE` was effectively 0 on the real ingress port → the **L3 route lookup never
ran**. We proved it cold: installed an `L3_DEFIP` entry that masks *every* key bit (a
match-everything route) and watched its hardware **HIT bit stay 0** under traffic. A
fully-masked TCAM entry can't fail a search that reaches it — so the search wasn't
happening. The fix was three characters of table name. The Cumulus dump settled it:
`PORT_TAB[1..52].V4L3_ENABLE=1`, `LPORT_TAB` all zero.

`l3_route_add()` being a stub (no `L3_DEFIP` programmed at all) was a parallel gap;
Cumulus puts the switch's own `/32`s in `L3_DEFIP → "to-CPU"` (a next-hop with
`PORT_NUM=0`, `MAC=0`, wildcard INTF), not in the host hash.

---

## 3. The indexing trap, generalized

Three different "port numbers" coexist and they are *not* interchangeable:

- **SerDes lane / "physical"** (e.g., 65 for swp1) — what PHY APIs take.
- **Logical / device port** (e.g., 1 for swp1) — what per-port *tables* index by.
- **MMU / counter port** — what RMON/`RDBGCn` stats index by (logical, here).

`bmd_port_vlan_set(unit, physical_lane, vid)` internally does `P2L()` — so the *API*
takes physical. But **direct table writes** (`WRITE_PORT_TABm`, `WRITE_LPORT_TABm`,
reading `RDBGCn`) take **logical**. We hit this twice: once writing `LPORT_TAB` at the
physical lane (empty row), and again in a diagnostic reading `PORT_TAB` at the physical
lane (read back zeros, sent us chasing a non-bug). Rule we adopted: *API calls take
physical; raw `WRITE_*m`/`READ_*r` take logical — verify the index space of every
direct register touch.*

---

## 4. The MAC/FCS/MTU layer — quiet corruption

Two bugs here that produce frames a sniffer counts as clean while the peer drops them:

- **CRC mode.** The egress MAC was in **CRC-replace** (not append): it overwrites the
  **last 4 bytes** of the frame with the FCS. For an exact-length frame this eats 4
  bytes of real IP payload; the wire frame has a *valid* FCS but a 4-byte-short IP
  packet, so the neighbor silently drops it (0 CRC errors, 0 checksum errors — nothing
  lights up). The tell was a frame-size bisection: payloads that left ≥4 pad bytes
  worked, exact-size ones didn't. Fix: append 4 bytes of FCS slack on TX.
- **Frame size.** `EGR_MTU.MTU_SIZE` and `XMAC_RX_MAX_SIZE` were 1522 while interfaces
  were configured for MTU 1600 → full frames dropped. Bumped to 1622.

Also relevant: on directed CPU injection, the TX path is a 2-DCB scatter-gather that
attaches a **stream-of-bytes module header** (`TX_DCB_HGf`, `MODULE_HEADER` words; the
3rd word = `P2L(port)`) to steer the frame out a specific port — and the chip
decrements TTL on that path (it L3-routes the injected frame), which is a useful tell
in captures.

---

## 5. DMA: getting frames to/from the CPU

This is its own ordered subsystem and a frequent silent wall:

- **Which engine:** we ended up on the **XGS packed-DMA** registers, not the
  per-channel CMICm registers — on this chip the CMICm channel registers wouldn't arm
  for our config (multi-session RX wall).
- **DCB format:** type-21 control word — `COUNT/CHAIN/RELOAD/DONE`. A single-DCB,
  no-reload ring polls one descriptor forever (the classic "RX never advances"); the
  fix is **chain + reload** for a continuous ring (64 DCBs), using `DESC_HALT_ADDR`.
- **Completion:** CMICm continuous-DMA completion uses a **controlled-interrupt mask**
  (e.g., `0x78000000`) routed to PCI, not the obvious per-COS bits — get it wrong and
  the ring fills but completion never fires.
- **Coherency:** the cache-flush macros are no-ops in our build (we rely on coherent
  DMA alloc); worth auditing if you see size-correlated corruption.

---

## 6. The 40G SerDes bring-up — SOLVED (2026-06-07)

For 10G, PCS `block_lock` was enough. For **40GBASE-R (CL82)** you need all **four PCS
lanes** to alignment-marker-lock and deskew. For weeks we only saw **2 of 4** — and
mis-diagnosed it as a missing-calibration wall. It was two stacked bugs:

1. **Frozen adaptation:** `fw_mode=0x1111` (SR4) *freezes* Warpcore RX auto-adaptation.
   Setting **`fw_mode=0`** lets the firmware adapt → all 4 lanes converge.
2. **Decode bug:** the link check required `am_lock==0xf`, but the alignment-lock field
   is a *state-machine value* and **`0x6` is the locked state** (matches Cumulus 4/4;
   `0xf` never occurs). The chip was locked while our code read it as unlocked.

What we ruled out (lane swap/remap via `PhyConfig_XauiRxLaneRemap`→`RXLNSWAP1`,
polarity, X4-vs-KR4 forced modes) were all correctly ruled out — none was the cause.

**The disproven theory:** we had pinned the root cause on OpenMDK lacking the full SDK's
per-lane RX-calibration layer (`_phy_wc40_independent_lane_init` / `FUN_015936ac` /
regs `0x8308`/`0x833c`). That was **false** — firmware auto-adapt handles all four lanes
once it isn't frozen. No cal-routine port or cold-init replay was needed. Verified by
raw-frame inject + tcpdump across the swp49↔swp50 loopback, both directions.

---

## 7. How you actually recover the ordering

No single trick — a loop:

1. **Capture the oracle.** Full register + table dumps from the working NOS on the same
   silicon, *idle and under load*, plus init-time register traces. Mine offline; don't
   curate.
2. **Diff intent from source.** When register-level guessing stalls, read the vendor
   SDK to learn *why* a write exists and what must precede it.
3. **Build visibility you control.** Our highest-leverage tool was an in-software
   chip diagnostic (signal-triggered) dumping per-stage hardware drop counters
   (`RDBGCn`, selected to RIPD4/RDISC/RFILDR/RDROP/...), table read-backs, and TCAM
   **HIT bits**. That converts "doesn't ping" into "the L3 lookup stage isn't executing
   — here's the counter, here's the un-set HIT bit."
4. **Bisect with deltas.** Snapshot counters → inject N packets → snapshot → attribute.
   Frame-size sweeps, before/after a single register change, neighbor-initiated tests
   the topology can't fake.
5. **Earn the conclusion.** "It forwards" only counts when routing tables, both ends'
   counters, and wire captures with the real peer MAC all agree — and you've proven it
   isn't leaking out the management port.

The meta-lesson: bringing up silicon without the vendor's `soc_init` is less about any
heroic register and more about **rebuilding the ordered init graph from evidence** —
and building the instruments that make the chip's silent failures visible.

---

## Appendix — register/table cheat-sheet touched in this effort

Access/DMA: `PAXB IMAP0_7`, `CMIC/CMICm`, `XGS packed DMA`, `TX_DCB`/`RX_DCB` (type 21),
`DESC_HALT_ADDR`, controlled-interrupt mask.
Ports/VLAN: `P2L/L2P`, `PORT_TABm` (PVID, `V4L3/V6L3_ENABLE`, `TRUST_INCOMING_VID`),
`LPORT_TABm` (profile; `VT_ENABLE`), `SOURCE_TRUNK_MAP_TABLE` (`LPORT_PROFILE_IDX`,
default `L3_IIF`/`VRF`), `VLAN_TABm` (`STG`, `VLAN_PROFILE_PTR`, `PORT_BITMAP`),
`VLAN_PROFILE_TABm` (`IPV4L3_ENABLE`, `L2_PFM`), `STG_TAB`, `EPC_LINK_BMAP`.
L3: `MY_STATION_TCAMm`, `L3_ENTRY_IPV4_UNICASTm` (hash; `HASH_CONTROL`),
`L3_DEFIPm` (+`_CAM_ENABLE`, PAIR mode), `L3_IIFm` (`VRF`, `ALLOW_GLOBAL_ROUTE`),
`ING_L3_NEXT_HOPm` (`COPY_TO_CPU`), `EGR_L3_NEXT_HOPm`, `EGR_L3_INTFm`,
`ING_CONFIG_64`, `LPORT_TABm.V4L3_ENABLE`.
MAC/MTU: `XMAC_*`, `XMAC_RX_MAX_SIZE`, `EGR_MTU`, `FRM_LENGTH`, CRC mode.
Punt/debug: `CPU_CONTROL_1`, `PROTOCOL_PKT_CONTROL`, `RDBGC0..8` + `*_SELECT`,
`FP_TCAM`/`FP_POLICY_TABLE`.
PHY (40G): Warpcore `independent_lane_init`, `RXLNSWAP1`, CL82 AM-lock/deskew,
KR4 vs X4 forced modes, per-lane RX cal (`~0x8308/0x833c`).
