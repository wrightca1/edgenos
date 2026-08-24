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

# Both inputs are read from inside a scratch dir we cd into, so a relative path
# would resolve against the wrong directory and fail with a confusing message
# ("no such file" naming a file that plainly exists). Absolutise them here.
KERNEL="$(cd "$(dirname "$KERNEL")" && pwd)/$(basename "$KERNEL")"
if [ -n "$BASE_INITRD" ]; then
    [ -f "$BASE_INITRD" ] || { echo "error: no initrd at $BASE_INITRD" >&2; exit 1; }
    BASE_INITRD="$(cd "$(dirname "$BASE_INITRD")" && pwd)/$(basename "$BASE_INITRD")"
fi

P="$HERE/payload"
A="$EROOT/asic/fm6000"
echo "=== EdgeNOS 7150 release $VERSION ($GITSHA) ==="

# ---- 1. build every tool from tracked source -------------------------------
echo "--- building tools from source ---"
built=0
# Names of the tools this run actually compiled. Staging uses THIS list and not
# a glob of the payload directory, because the payload directory keeps binaries
# from previous builds: when fm6000_l2arpre.c was deleted from the tree, its
# stale binary still shipped in the image and still ran. An image must not
# contain a tool whose source is gone.
BUILT=""
# standalone, single-file
for t in fm6000_coldreplay fm6000_initsbus fm6000_memfill fm6000_fullreplay \
         fm6000_spico fm6000_mrl fm6000_ucode_dbg fm6000_i2c_bringup \
         fm6000_safinit fm6000_cminit fm6000_sweepinit fm6000_l2finit fm6000_eplinit fm6000_ffuinit fm6000_l2linit fm6000_eplseq fm6000_l2arseq fm6000_mgmt2pre fm6000_tbl3init fm6000_crmdrop \
         fm6000_mgmt2init fm6000_sweeperinit fm6000_cmminit fm6000_monitorinit fm6000_statsarinit fm6000_eaclinit fm6000_laginit fm6000_glortinit \
         fm6000_l2arinit fm6000_parserinit fm6000_modinit fm6000_l3arinit fm6000_hashinit fm6000_mapperinit \
         fm6000_route fm6000_fibd fm6000_rport fm6000_bst fm6000_fibgen fm6000_lanelink fm6000_sbusdump \
         fm6000_wdog fm6000_sbus fm6000_ffubstinit fm6000_l3arslice1 fm6000_l3arslice4 fm6000_l3arslice3 fm6000_l3arslice2 fm6000_cmwm fm6000_l3artables fm6000_mapper fm6000_smalltables fm6000_cmrest fm6000_parserfields fm6000_esched fm6000_erl fm6000_modports fm6000_safmatrix fm6000_sbusseq fm6000_slice fm6000_eschedrr fm6000_parserseed; do
    [ -f "$A/$t.c" ] || continue
    cc -O2 -I"$A" -o "$P/$t" "$A/$t.c" 2>/dev/null && { built=$((built+1)); BUILT="$BUILT $t"; } \
        || echo "    WARN: $t failed to build"
done
# multi-object (need the DMA/hw helpers)
DEPS="$A/fpdma.c $A/fpdma_kmod.c $A/fm6000_hw.c"
for t in fm6000_txinline fm6000_l3 fm6000_portd fm6000_rxdump; do
    [ -f "$A/$t.c" ] || continue
    cc -O2 -I"$A" -o "$P/$t" "$A/$t.c" $DEPS 2>/dev/null && { built=$((built+1)); BUILT="$BUILT $t"; } \
        || echo "    WARN: $t failed to build"
done
# payload-local helpers
for t in fm6000reg fm6000load pcicfg scddump si5338 scdreg resettool kexec; do
    [ -f "$P/$t.c" ] && { cc -O2 -o "$P/$t" "$P/$t.c" 2>/dev/null && { built=$((built+1)); BUILT="$BUILT $t"; }; }
done
echo "    built $built tools"

# ---- 2. assemble the initramfs --------------------------------------------
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
if [ -n "$BASE_INITRD" ] && [ -f "$BASE_INITRD" ]; then
    # Overlay onto a known-good initramfs so the kernel modules match the kernel.
    echo "--- overlaying onto $BASE_INITRD ---"
    # gzip -cd < FILE, not zcat FILE: zcat appends .gz to a name that lacks a
    # known suffix, so a base initrd named "initrd-i386" is reported missing.
    ( cd "$WORK" && gzip -cd < "$BASE_INITRD" | cpio -idm --quiet 2>/dev/null )
else
    echo "error: set BASE_INITRD=<initrd-i386 matching KERNEL> (module vermagic must match)" >&2
    exit 1
fi

# ---- PURGE third-party payloads inherited from the base initramfs -----------
# The base initrd is a development image and carries operator-supplied blobs:
# the FM6000 microcode and the Si5338 clock register maps (Silicon Labs
# copyright, extracted from EOS). A RELEASE must not redistribute those, and
# they are already forbidden by .gitignore -- but they arrive here through the
# overlay, not through git, so they have to be removed explicitly.
purged=0
for junk in "$WORK/usr/share/firmware"/*.si5338 \
            "$WORK/usr/share/firmware"/fm6000Microcode.raw \
            "$WORK/usr/share/firmware"/*.raw \
            "$WORK"/mnt/flash/fwd*.txt "$WORK"/mnt/flash/ucode_*.raw; do
    [ -e "$junk" ] && { rm -f "$junk"; purged=$((purged+1)); echo "    purged $(basename "$junk")"; }
done
[ "$purged" = "0" ] && echo "    no third-party payloads in the base image"

mkdir -p "$WORK/usr/bin" "$WORK/usr/lib/edgenos/platform"
n=0
for t in $BUILT; do
    b="$P/$t"
    [ -f "$b" ] && [ -x "$b" ] && { cp "$b" "$WORK/usr/bin/"; n=$((n+1)); }
done
# Anything sitting in payload/ that we did not just build is stale: name it, so
# a deleted source cannot quietly keep shipping.
for b in "$P"/fm6000_*; do
    [ -f "$b" ] || continue
    case " $BUILT " in *" $(basename "$b") "*) ;; *)
        echo "    stale binary NOT staged: $(basename "$b") (no source in tree)" ;;
    esac
done
# ...and prune the SAME thing out of the base initramfs we overlaid onto. Not
# staging a stale binary is not enough on its own: the base image is a previous
# release, so it already carries the tool, and the overlay leaves it in place.
# fm6000_l2arpre survived exactly this way after its source was deleted.
for b in "$WORK"/usr/bin/fm6000_*; do
    [ -f "$b" ] || continue
    _n="$(basename "$b")"
    case " $BUILT " in *" $_n "*) continue ;; esac
    [ -f "$A/$_n.c" ] && continue     # source exists but did not build: already warned
    rm -f "$b"
    echo "    pruned from base image: $_n (no source in tree)"
done
cp "$P"/*.sh "$WORK/usr/lib/edgenos/platform/" 2>/dev/null || true

# ---- control plane: zebra + ospfd (Quagga), if built --------------------------
# Third-party GPL, built per platform/arista-7150s-52/deploy/README.md. Optional:
# without them the image still boots and forwards, it just has no routing protocol.
CP="${CONTROL_PLANE:-}"
if [ -n "$CP" ] && [ -d "$CP" ]; then
    for b in zebra ospfd; do
        [ -f "$CP/$b" ] && { cp "$CP/$b" "$WORK/usr/bin/"; chmod +x "$WORK/usr/bin/$b"; }
    done
    for l in libzebra.so.1 libospf.so.0 libm.so.6; do
        [ -f "$CP/$l" ] && cp "$CP/$l" "$WORK/usr/lib/"
    done
    mkdir -p "$WORK/etc/quagga" "$WORK/var/run/quagga"
    cp "$EROOT/platform/arista-7150s-52/deploy/zebra.conf" "$WORK/etc/quagga/" 2>/dev/null || true
    cp "$EROOT/platform/arista-7150s-52/deploy/ospfd.conf" "$WORK/etc/quagga/" 2>/dev/null || true
    echo "    control plane: zebra + ospfd + configs"
else
    echo "    control plane: SKIPPED (set CONTROL_PLANE=<dir with zebra/ospfd/libs>)"
fi
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
