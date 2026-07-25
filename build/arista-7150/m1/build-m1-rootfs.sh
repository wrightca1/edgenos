#!/bin/sh
# build-m1-rootfs.sh - assemble the M1 platform rootfs.
#
# Default (minimal, "start minimal"): a shell/sysfs initramfs (busybox + kernel
# modules + SCD .ko + board scripts + init-m1) as a cpio.gz — reuses the M0
# packager unchanged (kernel + this initramfs). No python; test LEDs/SFP/sensors
# via sysfs.
#
# --squashfs (later stage): build a squashfs rootfs instead (add python3 +
# i2c-tools yourself for platform.py/DOM), for `build-aboot-swi.sh --rootfs`.
#
#   KDIR=... SONIC=... EDGENOS_ROOT=... ./build-m1-rootfs.sh [--squashfs] [--out FILE]
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
KDIR="${KDIR:-/home/smiley/own_kernel/linux-6.12}"
SONIC="${SONIC:-/home/smiley/edgecore/sonic}"
EROOT="${EDGENOS_ROOT:-$(cd "$HERE/../../.." && pwd)}"
M0="$HERE/../m0"
MODE="initramfs"; OUT=""

while [ $# -gt 0 ]; do
    case "$1" in
        --squashfs) MODE="squashfs"; shift;;
        --out)      OUT="$2"; shift 2;;
        *) echo "unknown arg: $1" >&2; exit 1;;
    esac
done
[ -n "$OUT" ] || OUT="$HERE/edgenos-m1-$( [ "$MODE" = squashfs ] && echo rootfs.sqsh || echo initramfs.cpio.gz )"

KREL="$(cat "$KDIR/include/config/kernel.release" 2>/dev/null || echo unknown)"
R="$(mktemp -d)"; trap 'rm -rf "$R"' EXIT
echo "=== staging M1 rootfs ($MODE, kernel $KREL) in $R ==="

# 1. busybox userland (reuse the M0 static busybox + applet symlinks).
mkdir -p "$R/bin" "$R/sbin" "$R/proc" "$R/sys" "$R/dev" "$R/etc" "$R/tmp"
cp "$M0/initramfs_root/bin/busybox" "$R/bin/busybox"
chmod +x "$R/bin/busybox"

# 2. kernel modules: install the in-tree set (at24/tg3/tun/vfio/i2c-dev/hwmon/...)
#    then the SCD .ko from the sonic fork; depmod so modprobe-by-name works.
if [ -d "$KDIR/lib/modules/$KREL" ] || [ -f "$KDIR/modules.order" ]; then
    make -C "$KDIR" INSTALL_MOD_PATH="$R" modules_install >/dev/null 2>&1 || \
        echo "warn: modules_install failed (build the M1 kernel first)" >&2
fi
SRC="$SONIC/src"
if [ -f "$SRC/scd.ko" ]; then
    mkdir -p "$R/lib/modules/$KREL/extra"
    for ko in scd.ko scd-hwmon.ko raven-fan-driver.ko; do
        [ -f "$SRC/$ko" ] && cp "$SRC/$ko" "$R/lib/modules/$KREL/extra/"
    done
else
    echo "warn: no scd.ko in $SRC — run build-scd-modules.sh first" >&2
fi
# M2 DMA kmod (clean-room FM6000 DMA/MSI backing)
FM6000DMA_KO="$EROOT/asic/fm6000/kmod/fm6000dma.ko"
[ -f "$FM6000DMA_KO" ] && { mkdir -p "$R/lib/modules/$KREL/extra"; cp "$FM6000DMA_KO" "$R/lib/modules/$KREL/extra/"; }
[ -d "$R/lib/modules/$KREL" ] && depmod -b "$R" "$KREL" 2>/dev/null || true

# 2b. payload: test binaries (kexec-reboot, reset tester, scd reg, fm6000 bring-up)
# + their glibc (busybox is static; these few tools are dynamic).
PAY="$HERE/payload"
if [ -d "$PAY" ]; then
    mkdir -p "$R/usr/bin" "$R/lib64" "$R/lib/x86_64-linux-gnu"
    for b in kexec resettool scdreg fm6000_bringup si5338 pcicfg scddump; do
        [ -f "$PAY/$b" ] && { cp "$PAY/$b" "$R/usr/bin/"; chmod +x "$R/usr/bin/$b"; }
    done
    # bundle the dynamic loader + libc for the (non-static) tools
    cp /lib64/ld-linux-x86-64.so.2 "$R/lib64/" 2>/dev/null || true
    for L in /lib64/libc.so.6 /lib/x86_64-linux-gnu/libc.so.6 /usr/lib64/libc.so.6; do
        [ -f "$L" ] && cp "$L" "$R/lib64/" && cp "$L" "$R/lib/x86_64-linux-gnu/" && break
    done
fi

# 3. platform assets: board scripts (+ HAL python for the later stage; harmless now).
mkdir -p "$R/usr/lib/edgenos/platform"
[ -d "$PAY" ] && cp "$PAY/"*.sh "$R/usr/lib/edgenos/platform/" 2>/dev/null || true   # to-eos.sh, fm6000-up.sh
cp "$EROOT/platform/arista-7150s-52/services/"*.sh "$R/usr/lib/edgenos/platform/" 2>/dev/null || true
cp "$EROOT/platform/arista-7150s-52/platform.py" "$R/usr/lib/edgenos/platform/" 2>/dev/null || true
cp "$EROOT/core/platform/base.py"                "$R/usr/lib/edgenos/platform/" 2>/dev/null || true
chmod +x "$R/usr/lib/edgenos/platform/"*.sh 2>/dev/null || true

# 3b. SSH (dropbear) + on-box firmware (regmap) + nc-shell. Staged in m1/ssh/
#     (gitignored; dropbear+libs from the Fedora rpm, regmap = board data).
SSHD="$HERE/ssh"
if [ -d "$SSHD" ]; then
    mkdir -p "$R/usr/sbin" "$R/usr/bin" "$R/lib64" "$R/etc/dropbear" "$R/root/.ssh" \
             "$R/usr/share/firmware"
    cp "$SSHD/dropbear"     "$R/usr/sbin/" 2>/dev/null && chmod +x "$R/usr/sbin/dropbear"
    cp "$SSHD/dropbearkey"  "$R/usr/bin/"  2>/dev/null && chmod +x "$R/usr/bin/dropbearkey"
    cp "$SSHD/lib/"*.so.* "$R/lib64/" 2>/dev/null
    cp "$SSHD/passwd" "$R/etc/passwd" 2>/dev/null
    cp "$SSHD/shadow" "$R/etc/shadow" 2>/dev/null; chmod 600 "$R/etc/shadow" 2>/dev/null
    cp "$SSHD/ncsh.sh" "$R/usr/bin/ncsh.sh" 2>/dev/null && chmod +x "$R/usr/bin/ncsh.sh"
    cp "$SSHD/"*.si5338 "$R/usr/share/firmware/" 2>/dev/null   # Si5338 regmap (on-box only)
    echo "root:x:0:0:root:/root:/bin/sh" >> "$R/etc/group" 2>/dev/null || true
    echo "bundled dropbear SSH + regmap + nc-shell"
fi

# 4. init.
cp "$HERE/init-m1" "$R/init"; chmod +x "$R/init"

# 5. pack.
if [ "$MODE" = squashfs ]; then
    command -v mksquashfs >/dev/null || { echo "need squashfs-tools (mksquashfs)" >&2; exit 1; }
    rm -f "$OUT"
    mksquashfs "$R" "$OUT" -comp zstd -noappend -all-root >/dev/null
    echo "NOTE: squashfs mode needs an initramfs that loop-mounts rootfs-i386.sqsh + switch_root"
else
    ( cd "$R" && find . | cpio -o -H newc 2>/dev/null | gzip -9 ) > "$OUT"
fi

echo "built $OUT ($(du -h "$OUT" | cut -f1))"
echo "package: build/build-aboot-swi.sh --kernel $KDIR/arch/x86/boot/bzImage \\"
if [ "$MODE" = squashfs ]; then
    echo "         --initramfs <loader-initramfs> --rootfs $OUT --out edgenos-m1.swi --release edgenos-m1"
else
    echo "         --initramfs $OUT --boot0 $M0/boot0 --out edgenos-m1.swi --version 0.1.0-m1 --release edgenos-m1"
fi
