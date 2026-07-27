# EdgeNOS on Arista DCS-7150S-52-CL (Intel/Fulcrum FM6000 "Alta") — Port Roadmap

Goal: an **open-source, clean-room** port that boots our own Linux on the 7150 and drives the FM6000 to
full L2/L3 forwarding — no vendor SDK linked, no proprietary blobs in the repo.

**This builds on existing EdgeNOS work, not a rewrite.** EdgeNOS already has:
- the ASIC **backend seam** `core/datapath/asic_ops.h` (`init / port_set / tx / rx_poll / intr_fd / shutdown`),
- proven ASIC ports in `asic/bcm56340`, `asic/bcm56846` (Broadcom, via OpenMDK) as the reference pattern,
- a substantial **clean-room FM6000 skeleton** in `asic/fm6000/` written from the RE writeups, and
- the `core/datapath/` daemon (`l2/l3/vlan/packet_io/netlink`) + `core/control-plane` (quagga) + `core/cli`.

The FM6000 is EdgeNOS's **first non-Broadcom, first clean-room datapath**: instead of linking a vendor SDK it
reimplements the procedures behind `asic_ops`. Our job is to **finish + live-validate** that existing skeleton.

Where an open GPL building block exists we reuse it as a **source** — e.g. the SCD kernel driver comes from the
SONiC tree, and SONiC's driver source informed how we reach the SCD/board. But EdgeNOS is its own NOS: the 7150
is **not** a SONiC-supported platform and none of SONiC runs here — that lack of support is part of why this
port is clean-room.

Status: ✅ done · 🔨 in progress · ⬜ todo · ⏸ deferred · ❓ decision

---

## The story so far (frontier: CPU-punt byte-mover)
The FM6000 enumerates on boot and BAR0 CSR access is proven. The dataplane frontier is the CPU-punt
byte-mover: CPU injects a frame → fabric forwards it to the CPU port → returns on the DMA RX ring.

**Breakthrough this session — the chip is now fully programmable without wedging (2026-07-27).** Every prior
attempt to program the forwarding tables (GLORT/DMASK/L2F) hard-wedged the chip. Root cause: the tables the
microcode never writes (GLORT_CAM/RAM, L2F_256, L2F_4K, LBS) are left **parity-invalid** — BIST marches the
*physical* SRAM but not the *logical* table contents, so any pipeline lookup that reads them faults. The fix is
the datasheet **step-12 "Initialize Memory" = CRM "Memory Set"** (`fm6000_crm`, CRM regs at MGMT2 0x1C000 from
EOS `fm6000_api_regs_int`): a HW walk that fills a block with a value **and HW-computed parity**. After
CRM-initing the whole forwarding path, programming GLORT/DMASK/L2F **no longer wedges**. Also fixed: the F64
tag must live in the **BD F64 field** (offset 0x0C), not the payload — the DMA inserts it (§7.11.1.4); and the
microcode tail's **MOD block wedges** → load the noMOD-tail (5326 lines).

**TX works, RX return is the open frontier.** With the config valid, a special-delivery inject **transmits**
into the fabric (tx desc 0x0c DONE+EOP). The golden EOS L2F 13-stage is replicated correctly (loaded as
COMPLETE 4-word atomic entries — wide tables §8.3 commit only on the MSW). But the frame **does not return to
the CPU RX ring** — narrowed (arista phase47) to source-port pruning (CPU is the ingress → pruned from dest),
VID-membership indexing, an unproven RX-capture path, or CPU-port egress scheduling. Next diagnostic: read the
**DROP_CODE** via the STATS block to pinpoint which pipeline stage drops the frame (see the RX row below).

## EdgeNOS milestone framing (M0 / M1 / M2)
| Milestone | Meaning | State |
|---|---|---|
| **M0** — netboot our kernel to a serial shell | boot chain | ✅ proven (Aboot-catch boot; `aboot-catch3.sh`) |
| **M1** — platform HAL | board/board-control bring-up | 🔨 M1 Linux boots; SCD/clocks/power ✅; optics/sensors/LED HAL ⬜ |
| **M2** — dataplane | FM6000 driver + forwarding | 🔨 **the frontier** — chip enumerates + BAR0 live; datapath next |

---

## M1 — platform HAL (`platform/arista-7150s-52`, `core/platform`)
| Part | Status | Where |
|---|---|---|
| M1 Linux boots (x86_64), serial + SSH | ✅ | `build/arista-7150/m1` |
| SCD/CPLD driver (`scd` + `scd-hwmon`, GPL — reused from the SONiC kernel tree) + 7150 `new_<object>` set | ✅ | in-kernel; SMBus masters, resetGpo, GPIO |
| Si5338 clock (Cotati map) — the enumeration root-cause | ✅ | `fm6000-up.sh` §3 |
| Power/VRMs (Chl822X, UCD90160) characterized | ✅ | undervolt refuted; no margining |
| Remote reboot / recovery (SCD power-cycle) | ✅ | `scdreg 0x7000 0xdead` |
| `platform.py` HAL: LEDs / reset / SFP-txdisable / DOM / sensors / PSU | ⬜ | `GAPS.md §C`; SFP/sensor i2c addrs are one `i2cdetect` (`GAPS.md §A`) |
| prefdl/idprom (MAC base, per-port SerDes tuning vars) | ⬜ | board EEPROM |

## M2 — FM6000 dataplane (`asic/fm6000/`, behind `asic_ops`)
Existing skeleton is cited per-line from the RE (phase7g, FPDMA.md). Status reflects **live** validation.

| Part | File | Status | Notes |
|---|---|---|---|
| BAR0 bind + CSR read/write/poll | `fm6000_hw.c` | ✅ **live-validated** | access model proven; our `fm6000reg` is the standalone check |
| Pre-enum PCIe/SerDes bring-up (make it enumerate) | (new) `fm6000-pcie-init.sh` | ✅ **live** | fold into the board bring-up / `fm6000_boot.c` preboot |
| `fm6000_boot_switch` ordering (preboot → BIST → sbus → caches → microcode → SPICO) | `fm6000_boot.c` | 🔨 | order recovered; **live-trace** BIST march data + SBus `0x0B0500` framing |
| Microcode/table load (parser/FFU CSR replay) | `fm6000_ucode.c` | ✅ **live** | The vendor image loads & M1 stays alive: L2 pipeline (lines 1–30321) loads fast; the L3AR region loads clean. **The tail's MOD block (`0x150000`, 3,820 lines) wedges** → load the **noMOD-tail (5,326 lines)**; CPU punt is ingress/RX so egress MOD is not needed. NOTE (corrected 2026-07-27): the microcode does **not** set the catch-all GLORT or the L2F_256 DMASK — those are EOS software config, not microcode. |
| SerDes SPICO upload (SBus IMEM) | `fm6000_ucode.c` | ⏸ | blob located; **not needed for first link** (PCIe SerDes came up without it) |
| Packet DMA BD-rings — **TX transmits** (PCIE block byte `0x5000`; 32B BDs, F64 tag) | `fpdma.c` | ✅ **TX live** | Full `fpdma_init` replicated: `dma_cfg(0x5060)=0x37`, **`0x505c=0x30f`**, split enable `TX_START(0x1)`→settle→`RX_START(0x2)`. **TX BD transmits** (`0x09→0x0c` DONE, reclaim, IRQ) kicked with **`PCI_TX_POST=0x5`** (Table 7-2). **F64 tag goes in the BD F64 field (offset 0x0C), not the payload** — the DMA inserts it (§7.11.1.4); `fpdma_tx_f64` + `fpdma_probe`. NOTE: repeated `fpdma_init` stalls the engine (desc stuck `0x09`) → one inject per fresh boot. |
| DMA/MSI kernel module (BAR0 + low-4GiB coherent pool + MSI) | `kmod/fm6000dma.c` | ✅ **live** | insmods clean, binds FM6000, 4 MiB coherent pool @ `0x7f800000` (<4 GiB) + MSI irq, `/dev/fm6000dma`. 7150 RS780 has no IOMMU → kmod required (not VFIO). |
| **RX return = L2 forwarding bring-up** (GLORT → L2F DMASK) | `fm6000_l2.c`, `fm6000_crm.c`, `fm6000-punt-inject.sh` | 🔨 **config solved; return-path open** | Pathway: `GLORT_CAM`(entry 0 = HW-forced catch-all)→`GLORT_RAM.DMaskBaseIdx=1`→`L2F_TABLE_256[0][1]`={CPU bit0, Et1 bit40}→L2F 13-stage. **Config wedge SOLVED (2026-07-27):** the GLORT/L2F tables are parity-invalid after BIST (microcode never writes them) → **CRM Memory-Set** (`fm6000_crm`) inits GLORT_CAM/RAM + all L2F 4K/256 + LBS with HW parity; then programming them no longer wedges. Golden EOS L2F 13-stage replicated as COMPLETE 4-word atomic entries (`golden_l2f_full.raw`; wide tables §8.3 commit on MSW). **TX injects (desc 0x0c); RX return still 0.** Remaining (arista phase47): source-port pruning (CPU ingress pruned from dest), VID membership (frame VID=0 vs golden VLAN1), unproven RX-capture, CPU-port egress scheduling. **Next diagnostic — DROP_CODE:** not a plain register; each L2F stage sets it (`L2F_PROFILE_TABLE.DropCode[22:19]`+`DropCodeSelect[23]`) when it zeroes DMASK_A; the 8-bit final value keys L2AR (`L2AR_CAM_KEYS.DROP_CODE[343:336]`) + STATS AR (`STATS_AR_CAM_KEYS.RX_KEY_DROP_CODE[52:45]`) → read via the **STATS block** (`STATS_AR` word 0x18000, `STATS_BANK` word 0x200000) to pinpoint the dropping stage instead of guessing. |
| `struct asic_ops` binding (`init/port_set/tx/rx_poll/intr_fd/shutdown`) | `fm6000_edged.c` | 🔨 | the seam the daemon drives |

### Forwarding primitives (built on the above, table/register-driven — clean-room)
| Part | Status | Notes |
|---|---|---|
| L2: MAC/MA table (learn/age), VLAN, FID, flood/mcast, GloRT→DMASK | ⬜ | datasheet §5.17; the `core/datapath/l2.c`+`vlan.c` backend |
| L3: LPM routes, next-hop, ARP/ND, router MAC | ⬜ | L3AR; `core/datapath/l3.c` backend |
| ACL/FFU classification | ⬜ | TCAM + action RAM (documented, no microcode) |
| CPU trap/copy (LLDP/ARP/STP punt) | ⬜ | rides `fpdma` rx |
| Counters / QoS / scheduler / mirror | ⬜ | later |

---

## Proof-of-life milestones (the order we chase)
1. **BAR0 register access** ✅
2. **CPU inject → pipeline → CPU receive** 🔨 *(TX ✅; forwarding config ✅ no-wedge; RX return open)*
   **TX ✅ — CPU inject → DMA → switch fabric WORKS.** `fpdma` engine golden-exact; injected BD **transmits**
   (desc `0x09→0x0c` DONE, `tx_reclaim=1`) kicked with **`PCI_TX_POST=0x5`**. F64 tag in the BD F64 field.
   **Forwarding config ✅ — no longer wedges (2026-07-27 breakthrough).** The GLORT/L2F tables are
   parity-invalid after BIST → **CRM Memory-Set** (`fm6000_crm`) inits them with HW parity; then GLORT/DMASK/L2F
   programming + the golden L2F 13-stage replay succeed cleanly (the chip is fully programmable).
   **RX ⬜ — frame transmits but does not return.** With the whole path configured (GLORT catch-all → DMASK
   {CPU,Et1} → golden 13-stage membership/STP/LBS/profiles), the byte still doesn't come back. Narrowed (arista
   phase47) to: source-port pruning (CPU ingress removed from dest), VID membership, RX-capture path, CPU-port
   scheduling. Next: read **DROP_CODE** via the STATS block to identify the exact dropping stage.
3. **One front-panel port links (10G)** ⬜ — EPL + SerDes + MAC.
4. **Two-port L2 forward** ⬜ — VLAN + MAC flood/learn.
5. **An L3 route** ⬜.
6. **All 52 ports + the `core/datapath` agents driving `asic_ops`** ⬜.

## Remaining FM6000 gaps (current — the box has been up + SCD working for a while)
Structure was already recovered; this session the **CPU-datapath recipe is now RE-recovered too** (from
`fpdma.ko`: BD 32B format, DMA-ring regs, `PCI_COMMAND` values, F64 tag) — no longer a live-trace unknown.
Genuinely open items, all live-verifiable now, none blocking the M-B milestone:
- SBus-controller CSR framing at `0x0B0500` (`fm6000_sbus_write`) — for front-panel SerDes/SPICO only; the
  *PCIe* SBus (`0xF000–0xF003`) is already proven live.
- BIST MARCH/fusebox data values (only if a cold BIST path is exercised).
- SPICO CRC-verify interrupt encoding (deferred with SPICO).
- 7150 SFP/sensor/PSU per-cage i2c addresses — an M1 platform-HAL detail (`i2cdetect` with `scd-hwmon` loaded).

## ❓ Decisions
- **Microcode: BYO vs authored.** `fm6000_ucode.c` loads a `<addr> <val>` image. Path 1: operator supplies
  their OWN licensed `fm6000Microcode.raw` (extracted from an EOS they own) → full pipeline immediately, zero
  vendor IP in-tree. Path 2 (2nd milestone): hand-author a minimal FlexPipe personality from the documented
  encodings. Plan: validate M2 infra with BYO, then author our own minimal personality.
- **Later cleanup:** retrofit `bcm56846` behind the same `asic_ops` seam to unify the two daemons.

## Clean-room / open-source discipline
- Procedures reimplemented from behavioral RE; **payloads are never vendored** — microcode/SPICO/`.si5338`/`.srec`
  are `.gitignore`'d and operator-supplied from a licensed EOS (`FM6000_FW_*` load-by-path in `fm6000_ucode.h`).
- Register offsets are recovered/cited facts, not the copied vendor header.
- Code on public GitHub; the arista RE writeups stay on the private GitLab.
