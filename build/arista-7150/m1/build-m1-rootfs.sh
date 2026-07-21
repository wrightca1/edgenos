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
[ -d "$R/lib/modules/$KREL" ] && depmod -b "$R" "$KREL" 2>/dev/null || true

# 3. platform assets: board scripts (+ HAL python for the later stage; harmless now).
mkdir -p "$R/usr/lib/edgenos/platform"
cp "$EROOT/platform/arista-7150s-52/services/"*.sh "$R/usr/lib/edgenos/platform/" 2>/dev/null || true
cp "$EROOT/platform/arista-7150s-52/platform.py" "$R/usr/lib/edgenos/platform/" 2>/dev/null || true
cp "$EROOT/core/platform/base.py"                "$R/usr/lib/edgenos/platform/" 2>/dev/null || true
chmod +x "$R/usr/lib/edgenos/platform/"*.sh 2>/dev/null || true

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
