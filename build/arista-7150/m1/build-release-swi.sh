#!/bin/sh
# build-release-swi.sh - produce a releasable EdgeNOS .swi for the Arista 7150S-52.
#
# This is the repeatable version of the hand-steps used during bring-up. It builds
# every tool from tracked source, assembles the initramfs, and wraps it in an Aboot
# SWI stamped with a release version.
#
# WHAT THE IMAGE CONTAINS
#   - our kernel + initramfs (busybox, SCD driver, FM6000 DMA kmod, dropbear SSH)
#   - the full FM6000 tool set, each built here from source in this repo
#   - the cold bring-up sequence, wired to run automatically at boot
#
# WHAT IT DELIBERATELY DOES NOT CONTAIN (bring your own, from a licensed EOS)
#   - the FM6000 microcode          -> /mnt/flash/ucode_l2.raw, ucode_tail.raw
#   - the SerDes SPICO firmware     -> inline in the replay set
#   - the register replay set       -> /mnt/flash/fwd4.txt
# These are third-party works. Without them the image still boots to a shell with
# mgmt SSH; the dataplane simply stays down and says so. See docs/PROVENANCE.md.
#
# KERNEL: pass KERNEL=<bzImage>. The tree's default KDIR build has been seen to
# produce an unusable kernel (no mgmt NIC IRQ, no block devices) -- see
# notes m1-kernel-aug1-broken. Prefer extracting a known-good kernel:
#     unzip -o edgenos-m1-bist17.swi linux-i386
#
#   Usage: [KERNEL=...] [BASE_INITRD=...] [VERSION=...] ./build-release-swi.sh [-o OUT]
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
EROOT="$(cd "$HERE/../../.." && pwd)"
VERSION="${VERSION:-$(cat "$EROOT/VERSION" 2>/dev/null || echo 0.0.0)}"
GITSHA="$(cd "$EROOT" && git rev-parse --short HEAD 2>/dev/null || echo unknown)"
OUT="${OUT:-$HERE/edgenos-7150-$VERSION.swi}"
KERNEL="${KERNEL:-}"
BASE_INITRD="${BASE_INITRD:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        -o) OUT="$2"; shift 2;;
        *) echo "unknown arg: $1" >&2; exit 1;;
    esac
done

[ -n "$KERNEL" ] || { echo "error: set KERNEL=<bzImage> (see the header)" >&2; exit 1; }
[ -f "$KERNEL" ] || { echo "error: no kernel at $KERNEL" >&2; exit 1; }

P="$HERE/payload"
A="$EROOT/asic/fm6000"
echo "=== EdgeNOS 7150 release $VERSION ($GITSHA) ==="

# ---- 1. build every tool from tracked source -------------------------------
echo "--- building tools from source ---"
built=0
# standalone, single-file
for t in fm6000_coldreplay fm6000_initsbus fm6000_memfill fm6000_fullreplay \
         fm6000_spico fm6000_mrl fm6000_ucode_dbg fm6000_i2c_bringup fm6000_route; do
    [ -f "$A/$t.c" ] || continue
    cc -O2 -I"$A" -o "$P/$t" "$A/$t.c" 2>/dev/null && built=$((built+1)) \
        || echo "    WARN: $t failed to build"
done
# multi-object (need the DMA/hw helpers)
DEPS="$A/fpdma.c $A/fpdma_kmod.c $A/fm6000_hw.c"
for t in fm6000_txinline fm6000_l3 fm6000_portd; do
    [ -f "$A/$t.c" ] || continue
    cc -O2 -I"$A" -o "$P/$t" "$A/$t.c" $DEPS 2>/dev/null && built=$((built+1)) \
        || echo "    WARN: $t failed to build"
done
# payload-local helpers
for t in fm6000reg fm6000load pcicfg scddump si5338 scdreg resettool kexec; do
    [ -f "$P/$t.c" ] && { cc -O2 -o "$P/$t" "$P/$t.c" 2>/dev/null && built=$((built+1)); }
done
echo "    built $built tools"

# ---- 2. assemble the initramfs --------------------------------------------
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
if [ -n "$BASE_INITRD" ] && [ -f "$BASE_INITRD" ]; then
    # Overlay onto a known-good initramfs so the kernel modules match the kernel.
    echo "--- overlaying onto $BASE_INITRD ---"
    ( cd "$WORK" && zcat "$BASE_INITRD" | cpio -idm --quiet 2>/dev/null )
else
    echo "error: set BASE_INITRD=<initrd-i386 matching KERNEL> (module vermagic must match)" >&2
    exit 1
fi

mkdir -p "$WORK/usr/bin" "$WORK/usr/lib/edgenos/platform"
n=0
for b in "$P"/fm6000_* "$P"/scdreg "$P"/resettool "$P"/kexec "$P"/pcicfg "$P"/scddump "$P"/si5338; do
    [ -f "$b" ] && [ -x "$b" ] && { cp "$b" "$WORK/usr/bin/"; n=$((n+1)); }
done
cp "$P"/*.sh "$WORK/usr/lib/edgenos/platform/" 2>/dev/null || true
chmod +x "$WORK/usr/lib/edgenos/platform/"*.sh 2>/dev/null || true
cp "$HERE/init-m1" "$WORK/init"; chmod +x "$WORK/init"
echo "    staged $n tools + $(ls "$WORK/usr/lib/edgenos/platform" | wc -l) scripts"

# identity, so the running image can say what it is
mkdir -p "$WORK/etc/edgenos"
cat > "$WORK/etc/edgenos/version.json" <<EOF
{
  "version": "$VERSION",
  "git": "$GITSHA",
  "platform": "arista-7150s-52",
  "arch": "x86_64",
  "asic": "fm6000",
  "boot": "aboot",
  "channel": "early-release"
}
EOF

( cd "$WORK" && find . | cpio -o -H newc --quiet 2>/dev/null | gzip -9 ) > "$WORK.cpio.gz"
echo "    initramfs: $(stat -c %s "$WORK.cpio.gz") bytes"

# ---- 3. wrap in an Aboot SWI ----------------------------------------------
sh "$EROOT/build/build-aboot-swi.sh" \
    --kernel "$KERNEL" --initramfs "$WORK.cpio.gz" \
    --boot0 "$HERE/../m0/boot0" --out "$OUT" \
    --version "$VERSION" --release "edgenos-7150" >/dev/null
rm -f "$WORK.cpio.gz"

echo "=== $OUT ($(stat -c %s "$OUT") bytes) ==="
echo "Install:  copy to /mnt/flash, then"
echo "          echo SWI=flash:/$(basename "$OUT") > /mnt/flash/boot-config && reboot"
echo "Recovery: serial console -> Ctrl-C at boot -> Aboot# -> rewrite boot-config"
