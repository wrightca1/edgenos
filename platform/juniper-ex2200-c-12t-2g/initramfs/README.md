# Bring-up initramfs

A static busybox userspace for the EX2200-C. Deliberately minimal: this is a
bring-up and diagnosis image, not a product rootfs.

## Why static

`CONFIG_STATIC=y`. There is no libc in the image and no dynamic loader to get
wrong on a board still being characterised — one file, `/bin/busybox`, and the
kernel either runs it or does not.

## Architecture

Must be **ARMv5TE** for the Feroceon 88FR131. Verify before booting, because a
v7 build fails as an undefined-instruction abort that looks like a kernel bug:

```
$ arm-linux-gnueabi-readelf -A busybox | grep Tag_CPU_arch
  Tag_CPU_arch: v5T
$ arm-linux-gnueabi-objdump -d busybox | grep -cE '\s(movw|movt|sdiv|udiv|dmb)\s'
0
```

## Build

```sh
curl -LO https://busybox.net/downloads/busybox-1.36.1.tar.bz2
tar xf busybox-1.36.1.tar.bz2 && cd busybox-1.36.1
export ARCH=arm CROSS_COMPILE=arm-linux-gnueabi-
make defconfig
sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config      # needs kernel headers we do not carry
make -j4 CONFIG_STATIC=y
```

Then substitute `$(BUILD)` in `initramfs.list.in` for your build root and point
`CONFIG_INITRAMFS_SOURCE` at the result. `gen_init_cpio` ships in the kernel
tree at `usr/gen_init_cpio`.

## Why a list file rather than a directory

A directory-based initramfs **cannot carry device nodes**. Without
`/dev/console` the kernel reports

```
Warning: unable to open an initial console.
```

and `init` runs with no stdout — which is indistinguishable from init never
starting. That cost a debugging cycle (findings/47), hence the explicit `nod`
entries here even though `/init` mounts devtmpfs immediately afterwards.

## The shell

`/init` ends with `exec setsid cttyhack /bin/sh`. `setsid` plus `cttyhack`
gives the shell a real controlling terminal, so job control and Ctrl-C work
over the serial console; a bare `exec /bin/sh` leaves it without one.
