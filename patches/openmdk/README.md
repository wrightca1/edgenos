# OpenMDK source modifications — EdgeNOS AS5610-52X

`asic/openmdk/` is a nested clone of `Broadcom/OpenMDK` 2.10.9 and is gitignored
by the parent EdgeNOS repo, so our divergence from stock can't live in the
parent's git as normal tracked files. This directory holds **canonical copies of
every OpenMDK source file EdgeNOS modifies**, plus a `.patch` (git diff vs stock
`db9c678`) alongside each for review.

These are the SDK half of the working datapath. The `asic/edged/` changes
(tracked normally in the parent repo) are the companion application half.

> **Reproducibility:** run `scripts/apply-openmdk-patches.sh` after a fresh
> OpenMDK clone — it copies every file below into the openmdk tree's PKG/
> masters. Without it, a clean build produces a **non-working** binary (no 40G,
> wrong port map, no RX→CPU punt).

## What's here

### PHY — Warpcore SerDes (40G QSFP bring-up)
| File | Maps to | Change |
|---|---|---|
| `bcmi_warpcore_xgxs_drv.c` | `phy/PKG/chip/bcmi_warpcore_xgxs/` | **The 40G fix.** `fw_mode = 0` (was `0x1111`, which froze SerDes RX adaptation → only 2/4 lanes locked); TX_DRIVER = `0x2440` (Cumulus value); CL82 dual-block PCS config (AM markers + deskew) written via raw `cdk_xgs_miim_write` + AER for the lane-2 context the SDK macros can't reach. |
| `cumulus_wc_ucode.c` | `phy/PKG/chip/bcmi_warpcore_xgxs/` | **New file.** Cumulus Warpcore microcode image referenced by the driver. |

### BMD — bcm56840_a0 datapath
| File | Maps to | Change |
|---|---|---|
| `bcm56840_a0_bmd_attach.c` | `bmd/PKG/chip/bcm56840_a0/` | `bcm56840_a0_p2l()` rewritten to the captured Cumulus physical→logical port map (stock's contiguous fallback put swp2 at logical 58 instead of port 66). |
| `bcm56840_a0_bmd_rx.c` | `bmd/PKG/chip/bcm56840_a0/` | RX rewritten to the XGS packed-`CMIC_DMA` path (`0x100`). The CMICm (xgsd) per-channel DMA regs at `0x31xxx` never armed on this chip; the XGS path is what made RX→CPU punt fire. |
| `bcm56840_a0_bmd_tx.c` | `bmd/PKG/chip/bcm56840_a0/` | XGS TX-DMA path (directed egress). |
| `bcm56840_a0_bmd_port_mode_set.c` | `bmd/PKG/chip/bcm56840_a0/` | Disable→Enable cycle so a forced 40G/10G `bmd_port_mode_set` doesn't fail with `CDK_E_PARAM`; used by edged's link poll + 40G auto-retry. |
| `bcm56840_a0_bmd_port_stp_set.c` | `bmd/PKG/chip/bcm56840_a0/` | Per-port STP state programming (FORWARDING). |
| `bcm56840_a0_bmd_switching_init.c` | `bmd/PKG/chip/bcm56840_a0/` | `if (P2L(unit,port)<0) continue;` so init doesn't abort on the `0x7f` unused lanes in the new port map. |
| `bcm56840_a0_bmd_stat_get.c` | `bmd/PKG/chip/bcm56840_a0/` | `RDBGC3/4/5/6`+`RIPC4` drop-localization stat readers (telemetry). |

### BMD — arch / shared / headers
| File | Maps to | Change |
|---|---|---|
| `xgsd_dma.c` | `bmd/PKG/arch/xgsd/` | RX channel init sets `CONTINUOUS_DMA=1` / `DROP_RX_PKT_ON_CHAIN_END=0` (block instead of drop when the ring is full). |
| `bmd_phy_staged_init.c` | `bmd/shared/` | Staged PHY init ordering for the Warpcore bring-up. |
| `bmd.h` | `bmd/include/bmd/` | Declarations for the added stat readers / helpers. |

### CDK
| File | Maps to | Change |
|---|---|---|
| `xgs_miim.c` | `cdk/PKG/arch/xgs/` | Warpcore block-select fix so MIIM reaches registers beyond block `0x8000` (MISC1r etc.) instead of a phantom device. |

## Not modified (intentionally)
- **`bcm56840_a0_bmd_init.c` — stock.** An earlier experiment compiled rc.soc
  tuning (`IFP_METER_PARITY`, `RDBGC*_SELECT`) into `bmd_init`; that was reverted.
  The chip-init tuning is applied at runtime via `/etc/edged/{config.bcm,rc.soc}`,
  not compiled in. The working-tree file is byte-identical to stock `db9c678`.

## Reproducing the build
```sh
# from EdgeNOS repo root, with the nested OpenMDK clone in place at asic/openmdk
cd asic/openmdk && git checkout db9c678 && cd -   # stock 2.10.9
scripts/apply-openmdk-patches.sh                  # restore EdgeNOS modifications
./build.sh image
```
