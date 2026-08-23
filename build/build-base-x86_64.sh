#!/bin/bash
# build-base-x86_64.sh — build the EdgeNOS x86_64 base system with Buildroot and
# capture it as the base .epk for the x86_64 virtual platform.
#
# What comes out (output/base-x86_64/):
#   bzImage          6.1 LTS kernel (generic x86-64: Intel + AMD, QEMU/KVM, EVE-NG, containerlab)
#   initrd.img       initramfs implementing the squashfs-overlay root
#   rootfs.sqsh      the proven base rootfs (Buildroot + systemd + glibc)
#   grub.img/boot.img/efi-part/   GRUB2 for legacy BIOS and UEFI
# and output/packages/base_<ver>_x86_64-<asic>.epk  (via `edgenos pkg base`).
#
# Native build on any x86_64 Linux host with the usual Buildroot deps (gcc, make,
# flex, bison, libncurses-dev, libssl-dev, libelf-dev, cpio, rsync, bc, unzip, wget,
# python3). No docker, no cross toolchain, no vendor SDK. ~20-40 min the first time
# on a many-core box (Buildroot builds its own toolchain); incremental after that.
#
# Env:  BR_DIR    Buildroot checkout (default ../buildroot, cloned + pinned if absent)
#       BR_TAG    Buildroot tag (default 2026.02.3 LTS)
#       O         Buildroot output dir (default output/br-x86_64)
#       PLATFORM  switch-DB key the base is captured for (default x86_64-kvm_x86_64-r0)
#       JOBS      parallelism (default nproc)
#       BR_TRIM   =1: after a successful build delete the big intermediate source trees
#                 (toolchain, kernel, glibc...) keeping only Buildroot's .stamp_* files, so
#                 the output dir stays incremental-rebuild-capable but ~1 GB instead of ~14 GB
#                 (for CI caches / small disks; a changed package still rebuilds)
set -euo pipefail
TOP=$(cd "$(dirname "$0")/.." && pwd)
BR_DIR=${BR_DIR:-$TOP/../buildroot}
BR_TAG=${BR_TAG:-2026.02.3}
O=${O:-$TOP/output/br-x86_64}
PLATFORM=${PLATFORM:-x86_64-kvm_x86_64-r0}
JOBS=${JOBS:-$(nproc)}
EXT=$TOP/arch/x86_64/buildroot
OUT=$TOP/output/base-x86_64

# Ubuntu 25.10+/26.04 ship uutils (Rust) coreutils; Buildroot needs GNU install.
# Shim it for this build only (no system-wide alternative changes).
if install --version 2>/dev/null | grep -qi uutils && command -v gnuinstall >/dev/null; then
    SHIM=$TOP/output/host-shim; mkdir -p "$SHIM"; ln -sf "$(command -v gnuinstall)" "$SHIM/install"
    export PATH="$SHIM:$PATH"; echo "==> using GNU install shim ($SHIM)"
fi

if [ ! -d "$BR_DIR/.git" ]; then
    echo "==> cloning Buildroot $BR_TAG into $BR_DIR"
    git clone -q --branch "$BR_TAG" --depth 1 https://gitlab.com/buildroot.org/buildroot.git "$BR_DIR"
else
    cur=$(git -C "$BR_DIR" describe --tags --exact-match 2>/dev/null || echo none)
    if [ "$cur" != "$BR_TAG" ]; then
        echo "==> Buildroot at $cur, switching to $BR_TAG"
        git -C "$BR_DIR" fetch -q --tags && git -C "$BR_DIR" checkout -q "$BR_TAG"
    fi
fi

mkdir -p "$O"
echo "==> configure (edgenos_x86_64_defconfig, BR2_EXTERNAL=$EXT)"
make -C "$BR_DIR" O="$O" BR2_EXTERNAL="$EXT" edgenos_x86_64_defconfig >/dev/null
echo "==> build (-j$JOBS) — log: $O/build.log"
make -C "$BR_DIR" O="$O" -j"$JOBS" 2>&1 | tee "$O/build.log" | grep -E '^>>> |edgenos:|Error|error:' || true
test -f "$O/images/rootfs.squashfs" || { echo "build failed — see $O/build.log"; exit 1; }

echo "==> collecting base artefacts -> $OUT"
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$O/images/bzImage" "$O/images/initrd.img" "$OUT/"
cp "$O/images/rootfs.squashfs" "$OUT/rootfs.sqsh"
cp "$O/images/grub.img" "$O/images/boot.img" "$OUT/" 2>/dev/null || true
[ -d "$O/images/efi-part" ] && cp -r "$O/images/efi-part" "$OUT/"
cp "$O/.config" "$OUT/buildroot.config"
( cd "$O/images" && sha256sum bzImage initrd.img rootfs.squashfs ) > "$OUT/SHA256SUMS"
ls -l "$OUT"

if [ "${BR_TRIM:-0}" = 1 ]; then
    echo "==> BR_TRIM: dropping intermediate source trees (keeping .stamp_* files)"
    for d in "$O"/build/*/; do
        case "$(basename "$d")" in
            buildroot-config|buildroot-fs) continue ;;
        esac
        find "$d" -mindepth 1 -maxdepth 1 ! -name '.stamp_*' -exec rm -rf {} + 2>/dev/null || true
    done
    du -sh "$O" | sed 's/^/    /'
fi

echo "==> capturing base .epk for $PLATFORM"
"$TOP/bin/edgenos" pkg base --from "$OUT/rootfs.sqsh" --platform "$PLATFORM"
echo "==> done"
