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

## The story so far (what changed this session)
EdgeNOS's FM6000 dataplane was **statically RE-complete but never live-validated** — the chip wouldn't even
enumerate, so none of `asic/fm6000/` had run. This session unblocked exactly that:
- the FM6000 now **enumerates** (auto on boot) and we have **live BAR0 CSR read/write** — validating the
  `fm6000_hw.c` access model (userspace `resource0` mmap, word-addressed `bar0[word]`) end to end;
- we discovered + proved the **pre-enum PCIe/SerDes bring-up** (`fmPlatformSetupPCIe`: JSS release + SBus init
  + `PCI_SERDES_CTRL_1`) that the chip needs to come up at all — a piece the skeleton didn't have;
- the box is powered and the chip is up, so the **`GAPS.md` live-probe queue is now closeable.**

So the remaining work is: **fold the live-validated sequences + a few captured runtime values into the existing
`asic/fm6000/` C, then build up the datapath.**

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
| Microcode/table load (parser/FFU CSR replay) | `fm6000_ucode.c` | ✅ **live — 90%** | **35,641 / 39,461 lines of the vendor image load & M1 stays alive** (2026-07-26): L2 pipeline MAPPER+PARSER+L2AR (lines 1–30321) loads fast; the L3AR/table-commit region (5,320 lines) loads clean **when paced line-by-line** (blasting wedges = HW table-commit backpressure → real fix is a commit done-poll). **Only MOD (`0x150000` egress-modify TCAM, 3,820 lines) still wedges even paced** after ~5 entries — a distinct egress/EPL-enable or MOD-commit done-poll dependency, deeper RE, **not on the byte-mover path**. |
| SerDes SPICO upload (SBus IMEM) | `fm6000_ucode.c` | ⏸ | blob located; **not needed for first link** (PCIe SerDes came up without it) |
| Packet DMA BD-rings — **TX transmits** (PCIE block byte `0x5000`; 32B BDs, F64 tag) | `fpdma.c` | ✅ **TX live** | Full `fpdma_init` replicated from vendor disasm + golden EOS capture: `dma_cfg(0x5060)=0x37`, **`0x505c=0x30f`**, split enable `TX_START(0x1)`→settle→`RX_START(0x2)`; engine reaches golden `STATUS=0x12`. **TX BD transmits** (`0x09→0x0c` DONE, reclaim, IRQ) once kicked with **`PCI_TX_POST=0x5`** (Table 7-2; `0x3`=`TX_STOP` was the bug). 32B BD, `status@0`=0x09 handoff/bit2=DONE, len@2, addr@4. Harness `fpdma_probe.c`. RX-ring receive pending L2 forwarding. |
| DMA/MSI kernel module (BAR0 + low-4GiB coherent pool + MSI) | `kmod/fm6000dma.c` | ✅ **live** | insmods clean, binds FM6000, 4 MiB coherent pool @ `0x7f800000` (<4 GiB) + MSI irq, `/dev/fm6000dma`. 7150 RS780 has no IOMMU → kmod required (not VFIO). |
| **RX return = L2 forwarding bring-up** (GLORT → L2F DMASK) | `fm6000_l2.c` + `fm6000_i2c_bringup.c` | 🔨 **root-caused; recipe complete** | Pathway pinned: `GLORT_CAM`(match DGLORT 0xFF00)→`GLORT_RAM.DMaskBaseIdx`→**`L2F_TABLE_256`** (76-bit DMASK, CPU port = bit0) → L2F membership. CPU port 0 / GLORT 0xFF00 (SDK). **Blocker root-caused:** table RAM has uninitialized ECC → CPU writes wedge the chip; the ONLY fix is the HW cold-BIST (`fm6000BistMemoryInit`) — full sequence + the 27-step `fm6000BootSwitch` order recovered & HW-validated (arista phase40/41/42). Tools: `fm6000_l2_probe` (loopback cfg), `fm6000_i2c_bringup` (observable pre-enum BIST+SerDes). **Last live gap:** i2c mgmt slave is dead after kexec-from-EOS → must **Aboot-boot M1** for a live slave, then run the tool → inject. |
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
2. **CPU inject → pipeline → CPU receive** 🔨 *(TX half DONE; RX half = L2 forwarding)*
   **TX ✅ — CPU inject → DMA → switch fabric WORKS (2026-07-26).** MSB released, L2-pipeline microcode loaded,
   `fpdma` rings + engine golden-exact (`dma_cfg=0x37`, `0x505c=0x30f`, split `TX_START`/`RX_START`), and the
   injected BD **transmits** (desc `0x09→0x0c` DONE, `tx_reclaim=1`, TX-done IRQ). The blocker was a wrong command
   code — the kick is **`PCI_TX_POST=0x5`**, not `0x3` (=`TX_STOP`); see datasheet Table 7-2. Harness `fpdma_probe.c`.
   **RX ⬜ — needs L2 forwarding.** No special-delivery bypass exists; the frame must traverse
   `GLORT_CAM→GLORT_TABLE→DMaskBaseIdx→DMASK_TABLE(76b)→L2F 13-stage(VLAN∧STP∧srcport)`. A CPU-loopback needs
   `GLORT_TABLE[0]`(catch-all)→DMASK(CPU-port bit) **and** the L2F membership masks to include the CPU port, plus
   the runtime CPU-port GLORT. All unconfigured on M1 → this is the L2-dataplane bring-up (its own campaign).
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
