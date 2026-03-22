# EdgeNOS - Network Operating System for Edgecore AS5610-52X

Open-source NOS for the Edgecore AS5610-52X bare metal switch.

- **CPU**: Freescale P2020 (PowerPC e500v2, dual-core 1.2 GHz)
- **ASIC**: Broadcom BCM56846 (Trident+) - 48x SFP+ 10GbE + 4x QSFP+ 40GbE
- **OS**: Debian jessie (powerpc) on Linux 5.10 LTS
- **Root**: SquashFS + OverlayFS (read-only base + writable overlay)

## Quick Start

### Build

```bash
# Requires Docker with --privileged (for debootstrap)
docker run --rm --privileged --network=host \
    -v $(pwd)/output:/build/output \
    -v $(pwd):/src:ro \
    debian:bookworm /src/scripts/build-all.sh
```

Output: `output/images/edgenos-as5610-52x.bin` (~138 MB ONIE installer)

### Install

1. Boot switch into ONIE installer mode
2. From a machine on the same network:
   ```bash
   cd output/images && python3 -m http.server 8080
   ```
3. On the switch (via serial or SSH to ONIE):
   ```
   onie-nos-install http://<your-ip>:8080/edgenos-as5610-52x.bin
   ```
4. After install completes, set U-Boot environment (ONIE resets `nos_bootcmd`):
   ```bash
   killall -9 discover; sleep 1
   cat <<'E' | fw_setenv -f -s -
   fdt_high 0xffffffff
   initrd_high 0xffffffff
   nos_bootcmd usb start; setenv bootargs noinitrd console=ttyS0,115200 earlycon root=/dev/sda6 rootfstype=squashfs ro rootwait; usb read 0x02000000 0x42000 0x7fc2; bootm 0x02000000#accton_as5610_52x
   boot_count 0
   onie_boot_reason nos
   E
   reboot -f
   ```
5. Switch boots into EdgeNOS. Login: **root / as5610**

### Access

- **Serial console**: ttyS0 at 115200 baud
- **SSH**: `ssh root@<dhcp-ip>` (password: `as5610`)

## Architecture

```
┌─────────────────────────────────────────────────┐
│ U-Boot 2013.01                                  │
│   nos_bootcmd → usb read → bootm FIT            │
├─────────────────────────────────────────────────┤
│ Linux 5.10.224-edgenos (PowerPC e500v2)          │
│   gianfar (eth0) │ i2c-mux │ USB │ PCIe         │
├─────────────────────────────────────────────────┤
│ Initramfs (nos-init.c, raw syscalls)             │
│   mount squashfs → mount overlay → switch_root   │
├─────────────────────────────────────────────────┤
│ Debian jessie (overlayfs: squashfs + ext2 rw)    │
│   systemd │ openssh │ iproute2 │ i2c-tools       │
├─────────────────────────────────────────────────┤
│ Hardware                                         │
│   BCM56846 (PCIe) │ 70 I2C buses │ CPLD │ PHYs  │
└─────────────────────────────────────────────────┘
```

## Partition Layout

```
/dev/sda1  128 MB   ext2    Persistent config (/mnt/persist)
/dev/sda2  (ext)    ---     Extended partition container
  sda5     16 MB    raw     Kernel FIT image
  sda6     ~290 MB  raw     Rootfs (squashfs)
/dev/sda3  ~2.6 GB  ext2    RW overlay (overlayfs upper layer)
```

## Project Structure

```
Makefile                        Top-level build orchestration
Dockerfile                      Docker-based cross-compile environment
scripts/
  build-all.sh                  Complete build (kernel+initramfs+rootfs+FIT+installer)
config/
  kernel/as5610_defconfig       Kernel config (P2020/e500v2)
  bcm/                          ASIC config (config.bcm, rc.soc, etc.)
  rootfs/overlay/               Files overlaid onto rootfs
kernel/
  dts/as5610-52x.dts            Device tree (full I2C topology)
initramfs/
  nos-init.c                    Init binary (raw syscalls, no libc)
installer/
  install.sh                    ONIE-compatible installer
  ONIE_ISSUES.md                Known ONIE compatibility issues
platform/
  cpld/                         CPLD kernel module
  retimer/                      DS100DF410 retimer driver
  onlp/                         Platform library (I2C, SFP, sensors)
  i2c/                          I2C device instantiation
asic/
  openmdk/                      OpenMDK (CDK/BMD/PHY) for BCM56846
  bde/                          BDE kernel modules
  switchd/                      Switch daemon (L2/L3/packet-io)
BOOT.md                         Complete boot documentation
```

## Key Technical Details

See [BOOT.md](BOOT.md) for the complete boot guide including:
- All critical kernel config options and why each is needed
- U-Boot environment variables
- FIT image format requirements
- Initramfs design (no-libc, e500v2 compatible)
- ONIE installer compatibility issues
- Recovery procedures

## Hardware Status

| Component | Status | Notes |
|-----------|--------|-------|
| Kernel boot | Working | Linux 5.10.224, dual-core SMP |
| Serial console | Working | ttyS0 at 115200 |
| Management ethernet | Working | gianfar, 1Gbps, DHCP |
| I2C buses | Working | All 70 buses probed |
| BCM56846 PCIe | Detected | 14e4:b846 on bus 0001:01:00.0 |
| Overlay filesystem | Working | squashfs + ext2 rw |
| SSH | Working | root/as5610 |
| systemd | Working | Full service management |
| CPLD | Not yet | Driver written, needs loading |
| SFP/QSFP | Not yet | ONLP library written |
| Retimers | Not yet | Driver written, needs loading |
| ASIC init | Not yet | switchd written, needs BDE + OpenMDK integration |
| L2 switching | Not yet | Requires ASIC init |
| L3 routing | Not yet | Requires OpenBCM SDK |

## License

Platform drivers and ONLP: EPL-1.0 (ported from ONL/Accton)
OpenMDK: Broadcom Switch APIs License
EdgeNOS components: GPL-2.0-or-later
