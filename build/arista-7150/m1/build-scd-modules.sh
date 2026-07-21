#!/bin/sh
# build-scd-modules.sh - build the Arista SCD kernel modules (GPL, from our
# wrightca1/sonic fork) against the M1 kernel.
#
#   KDIR=/path/to/linux-6.12 SONIC=/path/to/sonic ./build-scd-modules.sh [rootfs_dir]
#
# Builds against the IN-TREE M1 kernel (build-m1-kernel.sh built it in-tree).
# Produces: scd.ko (core) + scd-hwmon.ko (led/xcvr/smbus/reset/gpio/fan/...) +
# raven-fan-driver.ko. If rootfs_dir is given, installs them into it.
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
KDIR="${KDIR:-/home/smiley/own_kernel/linux-6.12}"
SONIC="${SONIC:-/home/smiley/edgecore/sonic}"
SRC="$SONIC/src"
ROOTFS="${1:-}"

[ -d "$SRC" ] || { echo "no scd source: $SRC (set SONIC=)" >&2; exit 1; }
if ! grep -q '^CONFIG_NET=y' "$KDIR/.config" 2>/dev/null; then
    echo "warn: $KDIR looks like the M0 config (no CONFIG_NET) — run build-m1-kernel.sh first" >&2
fi
[ -f "$KDIR/Module.symvers" ] || echo "warn: no $KDIR/Module.symvers — build the M1 kernel first (MODVERSIONS is off, so non-fatal)" >&2

# Build only the 7150-relevant modules: scd core + scd-hwmon bundle + raven fan.
# ARISTA_SCD_DRIVER_CONFIG=m makes the Makefile build scd.o standalone.
echo "=== building scd modules against $KDIR ==="
make -C "$KDIR" M="$SRC" \
     ARISTA_SCD_DRIVER_CONFIG=m \
     modules

echo "=== built ==="
for ko in scd.ko scd-hwmon.ko raven-fan-driver.ko; do
    if [ -f "$SRC/$ko" ]; then
        echo "  $SRC/$ko"
    else
        echo "  MISSING: $ko" >&2
    fi
done

if [ -n "$ROOTFS" ]; then
    dst="$ROOTFS/lib/modules/extra"
    mkdir -p "$dst"
    for ko in scd.ko scd-hwmon.ko raven-fan-driver.ko; do
        [ -f "$SRC/$ko" ] && cp "$SRC/$ko" "$dst/"
    done
    echo "installed -> $dst (matches platform.py _modpath lib/modules/*/extra)"
fi

echo "clean up sonic tree with: make -C $KDIR M=$SRC clean"
