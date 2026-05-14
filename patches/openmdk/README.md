# OpenMDK BMD patches — Cumulus-alignment

These four files are hand-modified BMD sources that diverge from
stock Broadcom OpenMDK 2.10.9. They are the BMD half of the changes
that gave EdgeNOS its first end-to-end ping to a Nexus neighbor on
2026-05-14 (the matching `asic/edged/` changes are committed alongside).

`asic/openmdk/` itself is a nested clone of `Broadcom/OpenMDK` and is
gitignored by the parent edgenos repo, so we keep canonical copies of
just our modified files here.

## Files

| File | Maps to | Change |
|---|---|---|
| `bcm56840_a0_bmd_rx.c`   | `asic/openmdk/bmd/pkgsrc/chip/bcm56840_a0/bcm56840_a0_bmd_rx.c`   | Replace single-DCB RX polling with a 64-DCB contiguous ring. Every DCB has `RELOAD=1`; `DESC_HALT_ADDR` is programmed past the last DCB so the chip wraps cleanly. Was the root cause of "ICMP echo replies never reach CPU." |
| `bcm56840_a0_bmd_init.c` | `asic/openmdk/bmd/pkgsrc/chip/bcm56840_a0/bcm56840_a0_bmd_init.c` | Mirror Cumulus's `/etc/bcm.d/rc.soc` tuning flags: `IFP_METER_PARITY=0` (Trident errata workaround), `RDBGC0/3/4/5/6_SELECT` and `TDBGC6_SELECT` so the disaggregated RX/TX drop counters populate. |
| `bcm56840_a0_bmd_tx.c`   | `asic/openmdk/bmd/pkgsrc/chip/bcm56840_a0/bcm56840_a0_bmd_tx.c`   | (Pre-existing tracked snapshot — earlier TX-DMA work.) |
| `xgsd_dma.c`             | `asic/openmdk/bmd/pkgsrc/arch/xgsd/xgsd_dma.c`                    | RX channel init sets `CONTINUOUS_DMA=1` and `DROP_RX_PKT_ON_CHAIN_END=0` so the chip blocks instead of dropping when the ring is full. |

## Companion `.patch` files

The `.patch` files alongside are `git diff` output (vs HEAD of
the nested OpenMDK clone) for the same set, useful for reviewing what
diverged from stock without doing a full file compare.

## Reproducing the build

After cloning the parent edgenos repo and the nested OpenMDK clone:

```sh
# from edgenos repo root
cd asic/openmdk
git checkout 73274f3   # OpenMDK 2.10.9 + the WARPCORE firmware fix
cd -
cp patches/openmdk/bcm56840_a0_bmd_rx.c   asic/openmdk/bmd/pkgsrc/chip/bcm56840_a0/
cp patches/openmdk/bcm56840_a0_bmd_init.c asic/openmdk/bmd/pkgsrc/chip/bcm56840_a0/
cp patches/openmdk/bcm56840_a0_bmd_tx.c   asic/openmdk/bmd/pkgsrc/chip/bcm56840_a0/
cp patches/openmdk/xgsd_dma.c             asic/openmdk/bmd/pkgsrc/arch/xgsd/
./build.sh image
```

The resulting `output/images/edgenos-as5610-52x.bin` produces the
`/usr/sbin/edged` with sha256 `49ca6da40590391a0d8fd3c4236f1ed145b93c4d974a5a94ecaeeb44f63109e7`
that pings the Nexus.
