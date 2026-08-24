# Building for the EX2200-C

Verified: mainline Linux 6.12 boots on the hardware with this configuration
and device tree. See `junos-ex2200-static-analysis/findings/46`.

## Toolchain

```sh
apt install gcc-arm-linux-gnueabi device-tree-compiler u-boot-tools \
            bison flex bc libssl-dev
```

**Soft-float `gnueabi`, not `gnueabihf`.** The AS4610's armhf toolchain is for
ARMv7 Cortex-A9 and will not do — this is ARMv5TE with no FPU. Tune as
`arm926ej-s`; GCC has no Feroceon tune name.

## Kernel

```sh
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- mvebu_v5_defconfig
# then the fragment in config/kernel-6.12.config, or set at minimum:
#   ARM_APPENDED_DTB, ARM_ATAG_DTB_COMPAT   - this U-Boot has no fdt/bootz
#   BLK_DEV_INITRD, DEVTMPFS_MOUNT
#   INITRAMFS_SOURCE=<dir>                  - one file to deliver
#   DEBUG_LL + DEBUG_MVEBU_UART0_ALTERNATE  - keep this, see below
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- -j3 \
     zImage marvell/kirkwood-ex2200-c-12t-2g.dtb
```

## Package

```sh
> **`make zImage` does not rebuild the DTB.** They are separate targets, and
> `bootargs` lives in the DTB — so editing the device tree, running only
> `make zImage`, and concatenating produces an image that silently boots with
> the *old* command line. Verify what actually shipped:
>
> ```sh
> dtc -I dtb -O dts arch/arm/boot/dts/marvell/kirkwood-ex2200-c-12t-2g.dtb | grep bootargs
> cat /proc/cmdline        # on the booted box - the decisive check
> ```
>
> This cost a boot cycle here: a change to remove `ip=` appeared to have no
> effect, and the running `/proc/cmdline` still showed it.

cat zImage kirkwood-ex2200-c-12t-2g.dtb > zImage-dtb    # appended, not passed
mkimage -A arm -O linux -T kernel -C none \
        -a 0x01000000 -e 0x01000000 -d zImage-dtb uImage
```

**The load address must not be `0x8000`.** U-Boot runs at ~`0x300000` on this
board because it boots the upgrade bank, so a multi-megabyte copy to `0x8000`
overwrites U-Boot mid-relocation and the board resets. `0x01000000` clears both
U-Boot and the TFTP staging address.

## Boot (no flash write)

At the U-Boot prompt — RAM only, **never `saveenv`**:

```
setenv netmask 255.255.255.248; setenv ipaddr <board>; setenv gatewayip <gw>
setenv serverip <tftp>; setenv bootargs console=ttyS0,9600 earlyprintk
tftpboot 0x2000000 uImage; bootm 0x2000000
```

`netmask` is **not** optional: without it U-Boot assumes a classful /8, treats an
off-subnet TFTP server as on-link, and times out with no useful error.

## Keep DEBUG_LL enabled

The zImage decompressor is silent without it, so any pre-console failure is
indistinguishable from a hang. It is what turned "no output at all" into a
register dump identifying the load-address collision above. Use the
`_ALTERNATE` variant — the plain one targets bootloaders that leave registers at
`0xd0000000` and crashes silently here.

## Console access

`junos-ex2200-static-analysis/bin/ex-uboot-run.py` reaches the U-Boot prompt
from any state and runs a command line, unattended.
