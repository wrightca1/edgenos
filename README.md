# EdgeNOS - Network Operating System for Edgecore AS5610-52X

Open-source NOS for the Edgecore AS5610-52X bare metal switch.

- **CPU**: Freescale P2020 (PowerPC e500v2, dual-core 1.2 GHz)
- **ASIC**: Broadcom BCM56846 (Trident+) -- 48x SFP+ 10GbE + 4x QSFP+ 40GbE
- **SDK**: OpenMDK (open-source CDK/BMD/PHY, no proprietary Broadcom SDK)
- **Kernel**: Linux 5.10.224 LTS
- **Root**: SquashFS + OverlayFS (read-only base + writable overlay)

---

## Requirements

- **Docker** 20.10+ (with BuildKit support)
- **Host OS**: Linux x86_64 (tested on Fedora 36)
- **Disk space**: ~10 GB for Docker image + build output
- **RAM**: 4 GB minimum, 8 GB recommended
- **Network**: Internet access for kernel and Buildroot downloads (first build only)
- **Switch**: Edgecore AS5610-52X with ONIE bootloader
- **Serial cable**: RJ45-to-DB9 or USB console cable (115200 baud)

## Building

### Docker Build (Recommended)

The entire build runs inside Docker -- no cross-compiler needed on the host.

```bash
# Clone the repo
git clone https://github.com/wrightca1/edgenos.git
cd edgenos

# Build the Docker container (downloads kernel + buildroot, ~5 min first time)
docker build --network host -t edgenos-builder .

# Run the full build (kernel + SDK + switchd + rootfs + ONIE installer)
mkdir -p output
docker run --rm --network host \
    -v $(pwd)/output:/build/output \
    edgenos-builder all

# Output: output/images/edgenos-as5610-52x.bin
```

### Build Targets

Build individual components for faster iteration:

```bash
# Interactive shell inside build environment
docker run --rm -it --network host \
    -v $(pwd)/output:/build/output \
    edgenos-builder bash

# Inside the container:
./docker-build.sh kernel      # Build Linux kernel + DTB (~5 min)
./docker-build.sh modules     # Build platform kernel modules
./docker-build.sh sdk          # Build OpenMDK libraries (CDK/BMD/PHY)
./docker-build.sh switchd     # Build switch daemon
./docker-build.sh rootfs      # Build root filesystem via Buildroot (~20 min)
./docker-build.sh image       # Create ONIE installer
./docker-build.sh all          # Full build
```

### Iterative Development

Mount source directories for live editing without rebuilding the container:

```bash
docker run --rm -it --network host \
    -v $(pwd)/output:/build/output \
    -v $(pwd)/asic/switchd:/build/asic/switchd \
    -v $(pwd)/platform:/build/platform \
    -v $(pwd)/config:/build/config \
    edgenos-builder bash

# Edit code on host, rebuild inside container:
./docker-build.sh switchd     # Rebuild just switchd (~10 sec)
./docker-build.sh image       # Repackage installer (~30 sec)
```

### Docker Compose

```bash
docker compose build                    # Build container
docker compose run --rm build           # Full build
docker compose run --rm build kernel    # Kernel only
docker compose run --rm build bash      # Interactive shell
```

### What the Docker Container Includes

| Component | Version | Purpose |
|-----------|---------|---------|
| Ubuntu 22.04 | base | Build environment |
| gcc-powerpc-linux-gnu | 11.4.0 | PPC cross-compiler |
| Linux kernel source | 5.10.224 | Kernel + modules |
| Buildroot | 2023.02.9 | Root filesystem |
| mkimage | - | FIT image creation |
| dtc | - | Device tree compiler |
| mksquashfs | - | SquashFS creation |
| qemu-user-static | - | PPC binary testing |

---

## Installing on the Switch

### Step 1: Boot into ONIE Install Mode

Connect serial console (115200 8N1). Power on the switch and interrupt U-Boot:

```
Hit any key to stop autoboot
=> run onie_bootcmd
```

Or from an existing OS: `onie-nos-install`

### Step 2: Serve the Image

On a machine reachable from the switch management port:

```bash
cd output/images
python3 -m http.server 8080
```

### Step 3: Install from ONIE

In ONIE shell (via serial console):

```bash
onie-nos-install http://<your-ip>:8080/edgenos-as5610-52x.bin
```

### Step 4: Configure U-Boot (Required After Install)

ONIE resets `nos_bootcmd`. After install, before reboot:

```bash
# Stop ONIE auto-discovery
killall -9 discover; sleep 1

# Set boot variables (write to temp file, not stdin - ONIE fw_setenv quirk)
cat > /tmp/env.txt <<'E'
fdt_high 0xffffffff
initrd_high 0xffffffff
nos_bootcmd usb start; usbiddev; setenv bootargs console=ttyS0,115200 root=/dev/sda6 rootfstype=squashfs ro rootwait; usbboot 0x02000000 ${usbdev}:5 && bootm 0x02000000#accton_as5610_52x
boot_count 0
E
# CRITICAL: DELETE onie_boot_reason (empty value = delete)
# If set to ANY value (even "nos"), U-Boot boots ONIE instead of NOS!
echo "onie_boot_reason" >> /tmp/env.txt

fw_setenv -f -s /tmp/env.txt

# Reboot into EdgeNOS
reboot -f
```

**Important U-Boot notes for AS5610:**
- `usbboot` (not `usb read`) is the proven command for this U-Boot build
- `onie_boot_reason` must be **deleted** (empty), not set to "nos"
- `usbiddev` detects the USB device number automatically
- `bootm ... #accton_as5610_52x` selects the FIT configuration by name

### Step 5: Login

- **Serial**: ttyS0 at 115200 baud
- **SSH**: `ssh root@<dhcp-ip>`
- **Password**: `as5610`

---

## Architecture

```
U-Boot 2013.01 (in NOR flash)
  nos_bootcmd -> usb read FIT from sda5 -> bootm
        |
Linux 5.10.224 (PowerPC e500v2, SMP)
  gianfar (eth0) | mpc-i2c (x2) | fsl-ehci (USB) | PCIe
        |
SquashFS rootfs (sda6) + ext2 overlay (sda3) + persist (sda1)
        |
systemd -> platform-init.sh -> switchd -> networking
                |                   |
                |                   +-- OpenMDK CDK/BMD/PHY
                |                   +-- bmd_reset -> bmd_init (WC firmware v0x0101)
                |                   +-- bmd_port_mode_set (10G/40G)
                |                   +-- bmd_tx/rx (DMA packet I/O)
                |                   +-- TUN interfaces (swp1-52)
                |
                +-- Load 12 kernel modules
                +-- GPIO init (QSFP reset, SFP TX enable)
                +-- 32x DS100DF410 retimer programming
                +-- CPLD LED + fan PWM
```

### ASIC Packet Path

```
TX: kernel -> TUN read -> bmd_tx() -> DMA -> BCM56846 -> wire
RX: wire -> BCM56846 -> DMA -> bmd_rx_poll() -> TUN write -> kernel
```

### Hardware Components (133 devices)

| Category | Count | Chips |
|----------|-------|-------|
| CPU | 1 | P2020 (I2C x2, UART, USB, PCIe, GbE) |
| Switch ASIC | 1 | BCM56846 (14 Warpcore SerDes, 2 LED processors) |
| CPLD | 1 | Accton custom (LEDs, PSU, fan, watchdog) |
| I2C Mux | 10 | PCA9548 x7 + PCA9546 x3 |
| GPIO Expander | 9 | PCA9506 x5 + PCA9538 x4 |
| Retimer | 32 | DS100DF410 (signal conditioning) |
| Temp Sensor | 2 | MAX6697 + MAX1617 |
| EEPROM | 59 | Board + 48 SFP + 4 QSFP |
| RTC | 1 | RTC8564 |

---

## Project Structure

```
Makefile                        Top-level build orchestration
Dockerfile                      Docker cross-compile environment
docker-compose.yml              Docker Compose config
config/
  kernel/as5610_defconfig       Kernel config (P2020/e500v2)
  bcm/                          ASIC config (config.bcm, rc.soc, LED hex)
  rootfs/
    buildroot_defconfig          Buildroot config
    overlay/                    Files merged into rootfs
      etc/systemd/system/       Service files (switchd, platform-init, thermal)
      etc/modules-load.d/       Kernel module load order (12 modules)
      usr/sbin/                 Init scripts (platform-init.sh, switchd-init, etc.)
kernel/
  dts/as5610-52x.dts            Device tree (full I2C mux topology)
installer/
  install.sh                    ONIE self-extracting installer
asic/
  openmdk/                      OpenMDK (CDK/BMD/PHY) -- git submodule
  bde/                          BDE kernel modules (PCI, DMA, IRQ, mmap)
    linux-kernel-bde.c          Kernel BDE (ioread32/iowrite32 with PPC barriers)
    linux-user-bde.c            Userspace BDE bridge
  switchd/                      Switch daemon
    switchd.c                   Main daemon (init, main loop)
    bde_interface.c             BDE/CDK/BMD integration (register access, DMA)
    portmap.c                   Port config + link polling (bmd_port_mode_set)
    packet_io.c                 Packet I/O (TUN + bmd_tx/rx with DMA coherent)
    l2.c                        L2 MAC table (bmd_port_mac_addr_add/remove)
    l3.c                        L3 routing (software forwarding, HW needs OpenNSL)
    vlan.c                      VLAN management (bmd_vlan_create/port_add)
    netlink.c                   Netlink listener (route/neigh/link events)
  mdk-init/                     OpenMDK standalone init tool
platform/
  cpld/                         CPLD kernel module (LEDs, PSU, fan, watchdog)
  retimer/                      DS100DF410 retimer kernel module
  onlp/                         Platform library (SFP, sensors)
```

---

## Implementation Status

### Working (code complete, needs deploy+test)

| Component | API | Source |
|-----------|-----|--------|
| ASIC reset + SerDes firmware | `bmd_reset()` + `bmd_init()` | OpenMDK (WC firmware v0x0101) |
| Port bringup (10G/40G) | `bmd_port_mode_set()` | OpenMDK |
| TX driver tuning | `PHY_CONFIG_SET(TxIDrv=2, TxPreIDrv=3)` | OpenMDK + Cumulus RE values |
| Link polling (30ms) | `bmd_port_mode_update()` | OpenMDK |
| Packet TX (kernel->ASIC) | `bmd_tx()` with DMA coherent | OpenMDK |
| Packet RX (ASIC->kernel) | `bmd_rx_start/poll()` 16-buffer ring | OpenMDK |
| L2 MAC forwarding | `bmd_port_mac_addr_add()` | OpenMDK |
| VLAN management | `bmd_vlan_create/port_add()` | OpenMDK |
| BDE kernel module | PCI + DMA + IRQ + mmap | Custom |
| Platform init | 12 modules + GPIO + retimer | Scripts |
| Thermal management | Fan PWM via CPLD | Scripts |
| CMIC LED programs | led0.hex + led1.hex | From Cumulus capture |

### Not Yet Implemented

| Component | Reason |
|-----------|--------|
| L3 hardware offload | Requires OpenNSL SDK (L3 is software-forwarded via kernel) |
| ACL/FP table programming | Requires OpenNSL |
| ECMP hardware hash | Works at L2; L3 ECMP needs OpenNSL |
| LLDP/STP/BGP | Install from packages (lldpd, mstpd, frr) |

---

## Reverse Engineering Data

This NOS was built using reverse-engineered data from Cumulus Linux 2.5 running
on the same hardware. The RE project is at:
[github.com/wrightca1/edgecore-5610-reverse-engineering](https://github.com/wrightca1/edgecore-5610-reverse-engineering)

Key RE findings used (parameter values only, no proprietary code):
- TX driver values: idriver=2, predriver=3 (from live TX_DRIVERr capture)
- MDIO PHY address map: 6 addresses across 3 buses (from GDB MIIM capture)
- Retimer EQ coefficients: channels=12, cdr_rst=28/16, tap_dem=23
- LED programs: 256-byte CMIC bytecode (from /etc/bcm.d/)
- I2C bus topology: 70 buses, 133 devices (complete sysfs enumeration)
- S-Channel protocol: 1789 operations captured via GDB breakpoint
- CMICm register architecture: direct window vs PIO indirect access

---

## License

- Platform drivers: GPL-2.0-or-later
- OpenMDK: Broadcom Switch APIs License
- EdgeNOS components: GPL-2.0-or-later
