# EdgeNOS Boot Guide for Edgecore AS5610-52X

Complete documentation of how EdgeNOS boots on the AS5610-52X, including
every issue discovered during development and the solutions.

## Hardware

- **CPU**: Freescale P2020 (dual-core PowerPC e500v2, 1.2 GHz)
- **RAM**: 2 GB DDR3
- **ASIC**: Broadcom BCM56846 (Trident+) on PCIe
- **Management Ethernet**: eTSEC (gianfar driver) with BCM54610C PHY at RGMII-ID
- **Storage**: 4 GB internal USB flash (`/dev/sda`)
- **Serial Console**: ttyS0 at 115200 baud (ns16550 at MMIO 0xff704500)
- **I2C**: 70 buses (2 controllers + PCA9546/PCA9548 mux tree)
- **U-Boot**: 2013.01.01 (ONIE build, 2017-06-06)

## Partition Layout

Single-slot MBR layout (ONIE busybox kernel only exposes 3 logical partitions):

```
/dev/sda1  (primary)   8192-270273     128 MB   ext2    Persistent config
/dev/sda2  (extended)  270274-895839              Container for logicals
  /dev/sda5  (logical) 270336-303041    16 MB   raw     Kernel FIT image
  /dev/sda6  (logical) 303104-895839   ~290 MB  raw     Rootfs squashfs
/dev/sda3  (primary)   895840-end      ~2.6 GB  ext2    RW overlay (overlayfs upper)
```

**Sector offsets for U-Boot `usb read`**:
- sda5 starts at sector 270336 = **0x42000**
- sda5 size = 32706 sectors = **0x7FC2**

## Boot Flow

```
U-Boot 2013.01
  ├── check_boot_reason: if onie_boot_reason=install/rescue/uninstall → ONIE
  ├── nos_bootcmd:
  │     usb start
  │     setenv bootargs noinitrd console=ttyS0,115200 earlycon root=/dev/sda6 ...
  │     usb read 0x02000000 0x42000 0x7fc2    (load FIT from sda5 raw sectors)
  │     bootm 0x02000000#accton_as5610_52x
  └── onie_bootcmd (fallback → boots ONIE)

Kernel 5.10.224
  ├── Decompresses gzip kernel
  ├── Loads DTB from FIT at 0x00f00000
  ├── Unpacks initramfs (nos-init)
  └── Runs /init

nos-init (initramfs)
  ├── Mount proc, sys, devtmpfs
  ├── Wait for /dev/sda6 (retry mount loop, USB takes ~1s to enumerate)
  ├── Mount squashfs from sda6 on /lower (read-only)
  ├── Mount ext2 from sda3 on /rw
  ├── Mount overlayfs: lower=/lower, upper=/rw/upper, work=/rw/work → /newroot
  ├── switch_root to /newroot
  └── exec /sbin/init (systemd)

systemd (Debian jessie)
  ├── Serial getty on ttyS0
  ├── SSH server (OpenSSH)
  ├── DHCP on eth0
  ├── nos-boot-success.service (resets U-Boot boot_count)
  └── Login: root / as5610
```

## Critical Kernel Config Options

These are all **required** for the AS5610-52X. Missing any one causes a boot failure:

```
# Platform (without MPC85xx_RDB: "No suitable machine description found")
CONFIG_PPC_85xx=y
CONFIG_MPC85xx_RDB=y
CONFIG_E500=y
CONFIG_RELOCATABLE=y

# FPU emulation (without this: "illegal instruction" from Debian glibc)
CONFIG_SPE=y
CONFIG_MATH_EMULATION=y
CONFIG_MATH_EMULATION_FULL=y

# Physical addressing (MUST be =n, =y causes silent hang)
CONFIG_PHYS_64BIT=n

# Serial console (without SERIAL_EARLYCON: no kernel output on serial)
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
CONFIG_SERIAL_EARLYCON=y
CONFIG_EARLY_PRINTK=y
CONFIG_PPC_UDBG_16550=y

# Network (full dependency chain, missing any link = no eth0)
CONFIG_NETDEVICES=y          ← menuconfig gate
CONFIG_ETHERNET=y            ← depends on NETDEVICES
CONFIG_NET_VENDOR_FREESCALE=y ← depends on ETHERNET
CONFIG_GIANFAR=y             ← the actual driver
CONFIG_FSL_PQ_MDIO=y         ← MDIO bus for PHY
CONFIG_BROADCOM_PHY=y        ← BCM54610C management PHY

# USB storage (for root on /dev/sda6)
CONFIG_SCSI=y                ← required for BLK_DEV_SD
CONFIG_BLK_DEV_SD=y
CONFIG_USB_STORAGE=y
CONFIG_USB_EHCI_HCD=y
CONFIG_USB_EHCI_FSL=y
CONFIG_MSDOS_PARTITION=y     ← required for MBR partition table

# Filesystem
CONFIG_SQUASHFS=y
CONFIG_SQUASHFS_XZ=y
CONFIG_OVERLAY_FS=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y

# No built-in cmdline (U-Boot bootargs controls everything)
CONFIG_CMDLINE_BOOL=n
```

## Critical U-Boot Environment Variables

```
# MUST be set to prevent FDT relocation hang
fdt_high=0xffffffff
initrd_high=0xffffffff

# MUST be set to prevent ONIE install-mode loop
onie_boot_reason=nos        (not blank, not 'install')

# Boot command using raw sector read (not ext2load, not usbboot)
nos_bootcmd=usb start; setenv bootargs noinitrd console=ttyS0,115200 earlycon root=/dev/sda6 rootfstype=squashfs ro rootwait; usb read 0x02000000 0x42000 0x7fc2; bootm 0x02000000#accton_as5610_52x

# Boot count safety (NOS resets to 0 on successful boot)
boot_count=0
```

## FIT Image Format

The FIT image **must** have:

1. **Kernel**: gzip compressed (NOT uncompressed uImage-in-FIT)
   - Extract raw payload: `dumpimage -T kernel -p 0 -o kernel.gz uImage`
   - `compression = "gzip"`, `load = <0x00000000>`, `entry = <0x00000000>`

2. **DTB**: with explicit load address below 16MB
   - `load = <0x00f00000>` (without this: kernel hangs after "Loading Device Tree")
   - Compatible string: `"accton,as5610_52x"` (underscores, not hyphens)

3. **Initramfs**: real init binary (not a stub)
   - `load = <0x01000000>`
   - U-Boot 2013.01 requires a ramdisk node or fails

4. **Configuration**: name must be `accton_as5610_52x`
   - U-Boot bootm uses `#${cl.platform}` which resolves to this

## Initramfs (nos-init.c)

Must be compiled with:
```
powerpc-linux-gnu-gcc -nostdlib -nostartfiles -static -O1 \
    -mcpu=8548 -msoft-float -fno-builtin -fno-pic -fno-pie \
    -o init nos-init.c -lgcc
```

**Why these flags matter**:
- `-nostdlib -nostartfiles`: Debian's powerpc glibc has FPU instructions (stfd)
  that crash on e500v2. Must use raw syscalls only.
- `-mcpu=8548`: e500v2 ISA (same as 8548)
- `-msoft-float`: no hardware FPU on e500v2
- `-fno-builtin`: prevents GCC from replacing string ops with libc calls
- `-lgcc`: provides `_restgpr_*` register save/restore helpers

**Wait for USB**: The init must retry-loop the squashfs mount (not just device
existence check) because the USB disk appears ~1s after init starts, and
`stat()` syscall struct mismatch on PPC32 makes device detection unreliable.

## ONIE Installer Issues

See `installer/ONIE_ISSUES.md` for the complete list. Key issues:

1. Platform string uses underscores: `powerpc-accton_as5610_52x-r0`
2. `/sbin` not in ONIE PATH - must export it
3. `fw_setenv` prompts for confirmation - use `-f -s` script mode
4. ONIE resets `nos_bootcmd=true` via `install_remain_sticky_arch()` -
   set `onie_boot_reason=nos` to prevent this
5. ONIE's `exec_installer` calls `reboot` without full path, fails,
   triggers error handler that resets nos_bootcmd
6. Must format sda3 (overlay partition) during install or root is read-only
7. Installer may fail to format sda3 if ONIE's kernel hasn't re-read the partition
   table after fdisk. The initramfs auto-formats sda3 on first boot using
   `/lower/sbin/mke2fs` from the squashfs rootfs.

## SSH Access

- **Username**: root
- **Password**: as5610
- **Key auth**: Also supported (add key to `/root/.ssh/authorized_keys`)

The root password and `PermitRootLogin yes` are baked into the squashfs during
the rootfs build. `sshpass` or key auth is needed from hosts that don't support
interactive password prompts:

```bash
sshpass -p 'as5610' ssh root@<switch-ip>
# or
ssh-copy-id root@<switch-ip>
```

## Build

```bash
# Full build (kernel + initramfs + Debian rootfs + FIT + installer)
docker run --rm --privileged --network=host \
    -v $(pwd)/output:/build/output \
    -v $(pwd):/src:ro \
    debian:bookworm /src/scripts/build-all.sh

# Output: output/images/edgenos-as5610-52x.bin (ONIE installer)

# Install on switch (from ONIE):
#   onie-nos-install http://<server>:8080/edgenos-as5610-52x.bin
# Then SSH in and set U-Boot env manually (ONIE resets nos_bootcmd):
#   killall -9 discover; sleep 1
#   cat <<'E' | fw_setenv -f -s -
#   fdt_high 0xffffffff
#   initrd_high 0xffffffff
#   nos_bootcmd usb start; setenv bootargs noinitrd console=ttyS0,115200 earlycon root=/dev/sda6 rootfstype=squashfs ro rootwait; usb read 0x02000000 0x42000 0x7fc2; bootm 0x02000000#accton_as5610_52x
#   boot_count 0
#   onie_boot_reason nos
#   E
#   reboot -f
```

## Quick Recovery

If the switch won't boot EdgeNOS:

1. Power cycle, hit any key at "Hit any key to stop autoboot"
2. At `LOADER=>` prompt:
   ```
   setenv fdt_high 0xffffffff
   setenv initrd_high 0xffffffff
   setenv bootargs 'noinitrd console=ttyS0,115200 earlycon root=/dev/sda6 rootfstype=squashfs ro rootwait'
   saveenv
   usb start
   usb read 0x02000000 0x42000 0x7fc2
   bootm 0x02000000#accton_as5610_52x
   ```
3. To return to ONIE: `setenv onie_boot_reason install` then `saveenv` then `boot`
