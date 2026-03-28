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
# ONLY set nos_bootcmd. Do NOT set fdt_high or initrd_high!
# Setting fdt_high=0xffffffff BREAKS both EdgeNOS AND ONIE boot.
# The default U-Boot behavior (no fdt_high/initrd_high) works correctly.

# Delete onie_boot_reason to prevent ONIE install-mode loop
# (fw_setenv with empty value deletes the variable)

# Boot command using partition-based usbboot (not raw sector read)
nos_bootcmd=usb start; usbiddev; setenv bootargs console=$consoledev,$baudrate root=/dev/sda6 rootfstype=squashfs ro rootwait earlycon; usbboot $loadaddr ${usbdev}:5 && bootm ${loadaddr}#accton_as5610_52x
```

### Variables that must NOT be set

| Variable | Why NOT |
|----------|---------|
| `fdt_high` | Setting to 0xffffffff disables FDT relocation. This breaks ONIE's boot and may break EdgeNOS depending on memory layout. Leave at U-Boot default. |
| `initrd_high` | Same — leave at default. U-Boot handles initrd placement correctly without override. |

### Setting from ONIE shell

```bash
export PATH=/sbin:/usr/sbin:$PATH
echo y | fw_setenv nos_bootcmd "usb start; usbiddev; setenv bootargs console=\$consoledev,\$baudrate root=/dev/sda6 rootfstype=squashfs ro rootwait earlycon; usbboot \$loadaddr \${usbdev}:5 && bootm \${loadaddr}#accton_as5610_52x"
# Delete onie_boot_reason to exit install mode:
echo "onie_boot_reason" > /tmp/env_del.txt
echo y | fw_setenv -f -s /tmp/env_del.txt
```

### Recovering from broken U-Boot env

If boot is completely broken (both EdgeNOS and ONIE hang):
```
# At LOADER=> prompt, interrupt autoboot and run:
env default -a
saveenv
reset
# This restores all U-Boot defaults and boots into ONIE
```

## FIT Image Format

The FIT image format was reverse-engineered from Cumulus 2.5.0's working
`uImage-powerpc.itb` (March 2026 session). The format MUST match exactly.

### Working FIT structure (verified March 28, 2026)

```dts
/ {
    description = "PowerPC kernel, initramfs and FDT blobs";
    #address-cells = <0x01>;

    images {
        kernel {                          /* node name: "kernel" (no @1) */
            type = "kernel";
            arch = "ppc";
            os = "linux";
            compression = "gzip";         /* gzip compressed vmlinux */
            load = <0x00>;                /* load address 0 */
            entry = <0x00>;               /* entry address 0 */
        };

        initramfs {                       /* REQUIRED — U-Boot 2013.01 needs this */
            type = "ramdisk";
            arch = "ppc";
            os = "linux";
            compression = "none";         /* cpio.gz treated as uncompressed blob */
            load = <0x00>;
        };

        accton_as5610_52x_dtb {           /* node name matches Cumulus convention */
            type = "flat_dt";
            arch = "ppc";
            os = "linux";
            compression = "none";
            /* NO load address — let U-Boot place it automatically */
        };
    };

    configurations {
        default = "accton_as5610_52x";

        accton_as5610_52x {
            kernel = "kernel";
            ramdisk = "initramfs";
            fdt = "accton_as5610_52x_dtb";
        };
    };
};
```

### Critical rules (learned the hard way)

1. **Kernel load/entry = 0x00** — CONFIG_RELOCATABLE=y handles address setup
2. **DTB has NO load property** — U-Boot places it in high memory automatically
3. **Initramfs is REQUIRED** — without it, U-Boot 2013.01 fails silently
4. **Node names: no `@1` suffix** — use `kernel`, `initramfs`, not `kernel@1`
5. **Kernel data = gzip of raw vmlinux** — extract from uImage: `dd if=uImage bs=64 skip=1 of=kernel.gz`
6. **Do NOT wrap uImage inside FIT** — that double-wraps and bootm can't unwrap it
7. **Config name = `accton_as5610_52x`** — matches U-Boot's `#accton_as5610_52x`

### What DOESN'T work (all tested and failed)

| Attempt | Result |
|---------|--------|
| `load = <0x00f00000>` on kernel | Kernel hangs after "Using Device Tree in place" |
| `load = <0x00f00000>` on DTB | Same hang |
| `fdt_high = 0xffffffff` | Breaks ONIE boot AND EdgeNOS boot |
| `initrd_high = 0xffffffff` | Same |
| Full uImage inside FIT (`type=kernel, compression=none`) | bootm tries to execute uImage header bytes |
| FIT without initramfs/ramdisk node | U-Boot hangs or fails silently |
| `kernel@1` node naming | Works but differs from Cumulus convention |

## Initramfs (nos-init)

Two versions exist. Use `initramfs-nos-init.c` (libc-based) for reliability:

### Working version: `initramfs-nos-init.c` (with standard libc)

```bash
powerpc-linux-gnu-gcc -static -Os -o init initramfs-nos-init.c
```

This uses standard `main()` with glibc. Despite earlier concerns about e500v2 FPU
compatibility, gcc-11's static powerpc glibc works fine on e500v2.

### Raw syscall version: `initramfs/nos-init.c` (NO libc)

```bash
powerpc-linux-gnu-gcc -static -nostartfiles -Os -o init initramfs/nos-init.c -lgcc
```

**WARNING**: This uses a custom `_start()` without proper PPC stack setup.
Compiling with `-nostartfiles` causes `_start` to execute without a valid stack
frame, leading to `User access of kernel address (ffff8ff8)` crash. The `-lgcc`
flag is needed for `_restgpr_30_x` PPC register save/restore routines.

**Recommendation**: Use `initramfs-nos-init.c` with standard libc. It's larger
(767K vs ~12K) but reliable.
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
