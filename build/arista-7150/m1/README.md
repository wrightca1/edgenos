# M1 build — kernel + SCD drivers for the platform bring-up

Everything needed to build the M1 image's kernel side, offline. M1 = the platform
plane: mgmt NIC + i2c/hwmon + the SCD board drivers + `platform.py`, booted into
RAM (same brick-proof rail as M0). See `edgenos/M0-BOOT-CHECKLIST.md` Phase H.

## Files
| File | Role |
|---|---|
| `kernel-m1.config` | Config fragment merged onto the M0 minimal `.config`. Validated: all 25 symbols + deps (PHYLIB/PTP/VFIO_IOMMU_TYPE1/REGMAP_I2C…) resolve. |
| `build-m1-kernel.sh` | Merge fragment → `olddefconfig` → build `bzImage` + modules (in-tree; backs up M0 `.config` → `.config.m0`). |
| `build-scd-modules.sh` | Build `scd.ko` + `scd-hwmon.ko` + `raven-fan-driver.ko` from the `wrightca1/sonic` fork against the M1 kernel. |

## Why a new kernel (M0 was too minimal)
The M0 kernel had **networking entirely off** (`CONFIG_NET` unset) plus no TUN, i2c-dev,
at24, hwmon i2c drivers, tg3, or VFIO. The fragment adds:
- **net base**: `NET/UNIX/INET/PACKET/NETDEVICES/ETHERNET` (mgmt NIC + SSH/FRR + AF_PACKET)
- **mgmt NIC**: `TIGON3` (BCM5719) → pulls `PHYLIB`/`PTP_1588_CLOCK`
- **i2c/optics/sensors**: `I2C_CHARDEV`, `EEPROM_AT24`, `PMBUS`+`SENSORS_UCD9000`, `SENSORS_MAX31790`, `LM90`/`LM75`
- **rootfs**: `SQUASHFS`(+XZ/ZSTD), `OVERLAY_FS` (python3 rootfs loop-mounted from tmpfs = RAM)
- **CPU punt**: `TUN`
- **M2 DMA**: `VFIO`/`VFIO_PCI`/`VFIO_NOIOMMU` (one kernel serves M1+M2). *(IOMMU caveat: this
  AMD RS780 box may lack AMD-Vi → noiommu or a DMA kmod; see checklist H0.)*

## Build order
```
KDIR=/home/smiley/own_kernel/linux-6.12
./build-m1-kernel.sh            # -> bzImage (stage as linux-i386) + Module.symvers
./build-scd-modules.sh <rootfs> # -> scd.ko/scd-hwmon.ko/raven-fan-driver.ko [+ install into rootfs]
```
Restore the M0 kernel config anytime: `cp $KDIR/.config.m0 $KDIR/.config`.

## Verified offline
- Fragment fully resolves (`olddefconfig`), M0 `.config` untouched by the check.
- **The Arista GPL `scd` driver compiles clean against Linux 6.12** — every `scd-*.c` +
  `raven-fan-driver.c` builds (`CC [M]` all green); no source API-drift. The final `.ko`
  link just needs the kernel's `Module.symvers` (produced by `build-m1-kernel.sh`).

## Still needs the box (M1 runtime, not build)
- The per-cage SCD i2c/xcvr addresses (`services/scd-setup.sh` `TODO(probe)`) — one `i2cdetect`.
- Confirm `tg3` binds the mgmt NIC and the hwmon chips enumerate on the SCD i2c buses.
