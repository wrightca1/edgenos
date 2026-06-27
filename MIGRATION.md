# Phase 4 — Board migration: Accton AS5610-52X (first board)

Migrating the AS5610 from its legacy fork (`newnos/`) onto the unified
`core/ · arch/ · asic/ · platform/` tree. **`newnos/` is now the frozen reference**
— it stays buildable until the unified build is validated in the container, then
it is retired.

## What moved where

The datapath daemon `edged` is split across the three seams by role:

| New home | Files | Role |
|----------|-------|------|
| `core/datapath/` | `edged.c` `datapath.c` `netlink.c` `l2.c` `l3.c` `vlan.c` `packet_io.c` (+ headers) | arch/ASIC-agnostic datapath framework |
| `asic/bcm56846/` | `bde_interface.c` `cumulus_replicate.c` `cumulus_swp1_replay.h` `cdk_custom_config.h` `phy_bus_miim_int.c`† | Trident+ chip glue (BDE/CDK) |
| `platform/accton-as5610-52x/` | `portmap.c` `led.c` (+ headers, `led*.hex`) | board port map + LED layout |

† `phy_bus_miim_int.c` is kept but not in the build (matches the proven Makefile).

Board support also migrated under `platform/accton-as5610-52x/`:

| Dir | Contents |
|-----|----------|
| `config/` | `config.bcm`, `rc.soc`, `datapath.conf`, `hw_desc`, `rc.forwarding`, `rc.ports_0`, `led0/1.hex` |
| `dts/` | `as5610-52x.dts` |
| `kernel/` | `as5610_defconfig` |
| `drivers/cpld/` | `accton_as5610_52x_cpld.c` (+ Makefile) |
| `drivers/retimer/` | `ds100df410.c`, `retimer_class.{c,h}` (+ Makefile) |
| `drivers/i2c/` | `i2c_init.sh` |
| `onlp/` | `platform_lib.{c,h}`, `sfpi.c` (+ Makefile) |
| `services/` | `platform-init`, `retimer-init`, `sfp-enable`, `fan-controller`, `swp-l3` units + scripts, `edged.service` |

The CPLD/retimer/ONLP dirs keep their own Makefiles and are **self-contained**
sub-builds (their headers resolve within their own dir), independent of the `edged` link.

## Build wiring

```
arch/powerpc/toolchain.mk      cross toolchain + static-link policy
asic/bcm56846/sdk.mk           OpenMDK include/flags/libs (mirrors proven flags)
platform/accton-as5610-52x/Makefile   composes core + asic + platform -> edged
platform/accton-as5610-52x/board.yml  the board manifest
```

OpenMDK is **shared** at the repo top level (`../OpenMDK`, sibling of `edgenos/`),
not vendored. The build links against OpenMDK static libs in `output/sdk/` (built by
the SDK step; gitignored).

### Building (cross-build container only)

The host has no `powerpc-linux-gnu-gcc`, so `edged` is built in the EdgeNOS
cross-build container:

```sh
# inside the container, with the toolchain + built OpenMDK libs present:
make -C platform/accton-as5610-52x EDGENOS_ROOT=$(pwd)
#   -> output/edged   (then: pkg build packaging/specs/edged.yml --platform … )
```

## Validation done here (no compiler available)

- ✅ every Makefile source resolves via `VPATH` across the three seams
- ✅ every local `#include "*.h"` in the `edged` sources resolves in a seam dir
- ✅ shared OpenMDK reachable at `../OpenMDK`
- ⏳ **compilation + link** — requires the container; not run on this host

## Remaining for this board (later Phase 4 sub-steps)

- control plane (zebra/ospfd) → `core/control-plane/`
- kernel tree + BDE/KNET modules + initramfs + rootfs build → arch/platform
- package the remaining components (bde/knet, quagga, platform-svc) so the image is
  fully package-composed (today they come "from base bits")
- once the unified `edged` is container-built and validated, point
  `packaging/specs/edged.yml` at `output/edged` and retire `newnos/`
