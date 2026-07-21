#!/bin/sh
# build-m1-kernel.sh - produce the M1/M2 kernel (bzImage + modules) from the M0
# minimal config + the kernel-m1.config fragment.
#
#   KDIR=/path/to/linux-6.12 ./build-m1-kernel.sh [-j N]
#
# Builds IN-TREE (this kernel was built in-tree, so out-of-tree O= is blocked
# without mrproper). The M0 .config is backed up to .config.m0 and can be
# restored; the M0 bzImage is already staged in edgenos-m0.swi, so overwriting
# the tree build here is fine.
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
KDIR="${KDIR:-/home/smiley/own_kernel/linux-6.12}"
FRAG="$HERE/kernel-m1.config"
JOBS="${1:-$(nproc)}"; JOBS="${JOBS#-j}"

[ -d "$KDIR" ] || { echo "no kernel tree: $KDIR (set KDIR=)" >&2; exit 1; }
[ -f "$FRAG" ] || { echo "no fragment: $FRAG" >&2; exit 1; }

# 1. Preserve the M0 config, then merge the M1 fragment onto it + resolve deps.
[ -f "$KDIR/.config.m0" ] || cp "$KDIR/.config" "$KDIR/.config.m0"
echo "M0 config backed up -> $KDIR/.config.m0 (restore with: cp .config.m0 .config)"

"$KDIR/scripts/kconfig/merge_config.sh" -m -O "$KDIR" "$KDIR/.config" "$FRAG"
make -C "$KDIR" olddefconfig

# 2. Report which requested symbols actually took (fail loudly if any dropped).
echo "=== M1 kernel config result ==="
miss=0
for sym in $(grep -oE '^CONFIG_[A-Z0-9_]+' "$FRAG"); do
    val=$(grep -E "^${sym}=" "$KDIR/.config" || true)
    if [ -n "$val" ]; then echo "  OK  $val"; else echo "  !!  $sym did NOT resolve"; miss=1; fi
done
[ "$miss" -eq 0 ] || { echo "ERROR: symbols above didn't resolve (missing deps)"; exit 1; }

# 3. Build kernel + modules in-tree.
make -C "$KDIR" -j"$JOBS" bzImage modules

echo
echo "kernel : $KDIR/arch/x86/boot/bzImage   (-> stage as linux-i386 in the M1 SWI)"
echo "symvers: $KDIR/Module.symvers          (feed to build-scd-modules.sh)"
echo "in-tree modules: 'make -C $KDIR INSTALL_MOD_PATH=<rootfs> modules_install' for at24/hwmon/tg3/tun/vfio"
