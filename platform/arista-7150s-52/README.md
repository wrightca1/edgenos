# platform/arista-7150s-52 — Arista DCS-7150S-52 (FM6000)

First Arista board in EdgeNOS: x86_64 host, Intel/Fulcrum FM6000 dataplane, SCD
FPGA board control. Milestones per arista `edgenos/ROADMAP.md`.

## What's here
| File | Milestone | Role |
|---|---|---|
| `platform.py` | **M1** | SCD-based platform HAL (`EdgeNOSPlatformBase` subclass): driver load order, LEDs/reset via `scd-led`/`scd-reset`, SFP present/tx-disable via `scd-xcvr`, DOM via base `sfp_diagnostics()`, sensors/PSU/fans via mainline hwmon |
| `services/scd-setup.sh` | **M1** | Declares the SCD board blocks to `scd-hwmon` (`new_reset`/`new_led`/`new_smbus_master`/`new_sfp`) — confirmed reg blocks hard-coded, per-cage i2c addrs `TODO(probe)` |
| `services/sfp-enable.sh` | **M1** | Clears `txdisable` on every cage (no enable3px gate under our own driver) |
| `edged.c` | **M2** | Minimal datapath daemon: drives the FM6000 through `asic_ops` and bridges the CPU port to a TUN netdev |
| `Makefile` | **M2** | Native x86_64 build of `edged-7150` (core seam + `asic/fm6000` + this main) |

The M0 SWI packager is `build/build-aboot-swi.sh` (repo top); M0 staging is
`build/arista-7150/m0/`.

## The ASIC-ops seam (M2 integration)
`core/datapath/edged.c` and its `l2/l3/vlan/packet_io` are written straight
against Broadcom OpenMDK (BMD/CDK) — they are **not** ASIC-agnostic today. Rather
than fork that, the FM6000 plugs in through an **additive** seam,
`core/datapath/asic_ops.h` (a `struct asic_ops` fn-pointer table). `asic/fm6000/
fm6000_edged.c` implements it (`init/port_set/tx/rx_poll/intr_fd/shutdown`) on top
of the clean-room driver (`fpdma_vfio` → `fm6000_boot_switch` → `fpdma`), and this
board's `edged.c` is a small daemon that drives it + a TUN CPU port.

**Follow-up (not done):** retrofit `bcm56846` behind `asic_ops` and collapse the
two daemons into one core `edged` that binds a backend at startup. Until then the
7150 runs its own `edged-7150` and the Broadcom boards keep the legacy path.

## Build & run
```
make -C platform/arista-7150s-52            # -> output/edged-7150
# host prep for DMA: intel_iommu=on; bind the FM6000 to vfio-pci:
echo 8086 155b > /sys/bus/pci/drivers/vfio-pci/new_id
EDGENOS_FM6000_SLOT=0000:02:00.0 ./edged-7150 cpu0
```
`edged-7150` runs bring-up, then bridges punted frames to/from the `cpu0` TAP.
The four `TODO(live-trace)` values in the driver (BIST/SBus/DMA-enable/SPICO) and
the `TODO(probe)` SCD i2c addresses all close in one session on the powered box.
