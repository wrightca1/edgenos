#!/bin/bash
# mkswi.sh -- build an EdgeNOS SWI for the Arista DCS-7050TX-64.
#
# A SWI is a plain zip with every member STORED, not deflated (verified against
# EOS-4.14.16M.swi: all five members method=0). Nothing signs or checksums it --
# EOS 4.14 predates SWI signing -- so a correctly shaped zip boots. See
# docs/ABOOT-AND-SWI-BUILD.md.
#
# Members, in the order stock EOS uses them:
#   version       SWI_VERSION / SWI_RELEASE / BLESSED
#   boot0         stage-0 shell, sourced by Aboot
#   initrd-i386   gzip cpio (newc)
#   linux-i386    bzImage
# rootfs-i386.sqsh is NOT produced: Swi/create.py states "stage 0 Aboot needs
# only initrd and linux", and our initrd is the whole OS.
#
# The kernel is our own 6.12 build (tools/mkkernel.sh). Set EDGENOS_KERNEL to
# override -- e.g. to the stock EOS 3.4.43 bzImage at
# $FLASH/unpack/swi-4.14.16M/linux-i386, which is known-good on this silicon and
# is a useful A/B if our kernel misbehaves. Neither is vendored into this repo.
#
# NOTE the member is still named "linux-i386" even though the image is x86_64 --
# that name is a label boot0 greps for, not a statement about the ISA. Stock EOS
# does exactly the same: its linux-i386 is an x86_64 bzImage with i386 userland.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
FLASH="${EDGENOS_FLASH:-/home/smiley/projects/arista/7050tx64-flash-20260816}"

KERNEL="${EDGENOS_KERNEL:-$REPO/build/kernel/arch/x86/boot/bzImage}"
BUSYBOX="${EDGENOS_BUSYBOX:-/home/smiley/edgenos-work/lls2/bin/busybox}"
TREE="${EDGENOS_TREE:-$REPO/image/initrd}"
BOOT0="${EDGENOS_BOOT0:-$REPO/image/boot0}"
OUT="${1:-$REPO/build/edgenos.swi}"

VER="${EDGENOS_VERSION:-0.1.0-m1}"
REL="${EDGENOS_RELEASE:-$(cd "$REPO" && git rev-parse --short HEAD 2>/dev/null || echo nogit)}"

die() { echo "mkswi: FATAL: $*" >&2; exit 1; }
say() { echo "mkswi: $*"; }

for f in "$KERNEL" "$BUSYBOX" "$BOOT0"; do
    [ -s "$f" ] || die "missing input: $f"
done
[ -d "$TREE" ] || die "missing initrd tree: $TREE"
command -v cpio >/dev/null || die "cpio not installed"
command -v zip  >/dev/null || die "zip not installed"

BUILD="$(dirname "$OUT")"
STAGE="$BUILD/stage"
ROOT="$BUILD/initrd-root"
rm -rf "$STAGE" "$ROOT"
mkdir -p "$STAGE" "$ROOT" || die "cannot create $BUILD"

# ---------------------------------------------------------------- initrd ---
say "staging initrd from $TREE"
cp -a "$TREE"/. "$ROOT"/ || die "copy tree"
mkdir -p "$ROOT"/{bin,dev,proc,sys,tmp,etc,mnt,mnt/flash}
install -m 0755 "$BUSYBOX" "$ROOT/bin/busybox" || die "install busybox"
[ -x "$ROOT/init" ] || chmod 0755 "$ROOT/init"

# platmon -- the read-only platform monitor (PSU over PMBus, fan CPLD, SCD PSU
# presence). Built here rather than committed, so the shipped binary always
# matches tools/platmon.c. Static: the initrd has no libc.
# FRR, plus the glibc it needs. Built separately by tools/build-frr.sh because
# it wants Docker and the network; this just folds the prepared tree in.
#
# ⚠ This is what ended the initrd's "no libc" rule. Every binary used to have to
# be fully static, which is why sdkpoc needs STATIC=1 and why a static Quagga
# was once substituted for the FRR that was actually asked for. The image now
# carries a real loader and glibc, so stock distro binaries run here.
FRRROOT="${EDGENOS_FRR:-$REPO/build/frr-root}"
if [ -d "$FRRROOT" ]; then
    say "adding FRR from $FRRROOT ($(du -sh "$FRRROOT" | cut -f1))"
    cp -a "$FRRROOT"/. "$ROOT"/
    rm -f "$ROOT/VERSION"
    mkdir -p "$ROOT/etc/frr" "$ROOT/var/run/frr" "$ROOT/var/log/frr"
    cp "$REPO/image/frr/frr.conf" "$ROOT/etc/frr/frr.conf"
    cp "$REPO/image/frr/daemons"  "$ROOT/etc/frr/daemons"
    # vtysh refuses to run without this, and it is one line.
    echo "service integrated-vtysh-config" > "$ROOT/etc/frr/vtysh.conf"
else
    say "** no FRR tree at $FRRROOT -- run tools/build-frr.sh first"
    say "   (image will build, but it will have no routing daemons)"
fi

# ---- licence compliance ---------------------------------------------------
# The image ships binaries we did not write: FRR, glibc and its closure (from
# build-frr.sh, which also drops their copyright files and a generated
# BILL-OF-MATERIALS under /usr/share/doc/edgenos), and Arista's GPL scd modules.
# Conveying those obliges us to convey their source. The licence texts travel in
# the image; this is the written offer that says how to get the source.
mkdir -p "$ROOT/usr/share/doc/edgenos"
cat > "$ROOT/usr/share/doc/edgenos/WRITTEN-OFFER" <<'OFFER'
EdgeNOS for the Arista DCS-7050TX-64 -- offer of source code
============================================================

This image contains software licensed under the GNU General Public License and
the GNU Lesser General Public License, including:

  * FRR (zebra, ospfd, vtysh)                       GPL-2.0
  * GNU C Library and the shared libraries it needs LGPL-2.1 / GPL-2.0
  * Arista scd.ko and scd-hwmon.ko kernel modules   GPL-2.0
  * The Linux kernel                                GPL-2.0

The exact versions shipped, the source package each came from, and their licence
texts are in this directory:

  BILL-OF-MATERIALS      every third-party binary, its version and source package
  copyright/             the upstream copyright and licence file for each package

COMPLETE CORRESPONDING SOURCE for all of the above is available. Run

  tools/fetch-gpl-sources.sh

in the EdgeNOS source tree, which downloads the exact source packages named in
the bill of materials from the same Debian release, together with Arista's GPL
kernel module source at the commit the shipped modules were built from.

Alternatively, request the source from the distributor of this image. This offer
is valid to anyone in possession of this image.

Nothing in this notice restricts your rights under those licences.
OFFER

# Arista's GPL modules ship as binaries in the initrd, so carry their licence and
# say plainly where the source is.
mkdir -p "$ROOT/usr/share/doc/edgenos/copyright"
cat > "$ROOT/usr/share/doc/edgenos/copyright/scd.copyright" <<'SCDC'
Files: lib/modules/scd.ko lib/modules/scd-hwmon.ko
Copyright: Arista Networks, Inc.
License: GPL-2.0
Source: https://github.com/aristanetworks/sonic  (src/scd.c, src/scd-hwmon.c)
 The modules shipped here were built from that tree. The exact commit is
 recorded by tools/fetch-gpl-sources.sh, which also fetches it.
SCDC
say "licence texts and written offer installed"

say "building platmon"
gcc -Wall -Wextra -O2 -static -o "$ROOT/bin/platmon" "$REPO/tools/platmon.c" \
    || die "build platmon"
chmod 0755 "$ROOT/bin/platmon"

# busybox --install -s needs the applets to exist as symlinks OR be resolved at
# runtime; init calls --install itself, but sh/mount/echo must work BEFORE that
# line runs, so seed the handful the first four lines of init depend on.
for a in sh mount echo cat sleep; do
    ln -sf busybox "$ROOT/bin/$a"
done

# Device nodes, matching the set the stock EOS initrd ships. /init redirects its
# own stdio to /dev/console before any devtmpfs mount can be relied on, so an
# initrd without a real dev/console boots completely silent and the box looks
# dead. mknod needs root; mkcpio.py writes the newc headers directly instead.
say "packing initrd (mkcpio + gzip)"
"$REPO/tools/mkcpio.py" "$ROOT" "$STAGE/initrd.cpio" \
    --dev dev/console:c:0600:5:1 \
    --dev dev/null:c:0666:1:3 \
    --dev dev/zero:c:0666:1:5 \
    --dev dev/urandom:c:0444:1:9 \
    --dev dev/ttyS0:c:0600:4:64 \
    --dev dev/ttyS1:c:0600:4:65 \
    --dev dev/mem:c:0600:1:1 \
    || die "mkcpio failed"
gzip -9 -c "$STAGE/initrd.cpio" > "$STAGE/initrd-i386" || die "gzip failed"
rm -f "$STAGE/initrd.cpio"

# Verify EVERY regular file round-trips. A pack on this pipeline once silently
# truncated when the staging filesystem filled up, and a spot check passed
# because the one file it tested happened to sit before the cut.
say "verifying initrd contents"
want=$(cd "$ROOT" && find . -type f | wc -l)
got=$(gzip -dc "$STAGE/initrd-i386" | cpio -t 2>/dev/null | wc -l)
say "checking device nodes survived the pack"
for d in console null zero urandom ttyS0 ttyS1 mem; do
    gzip -dc "$STAGE/initrd-i386" | cpio -itv 2>/dev/null \
        | grep -qE "^c.* dev/$d\$" || die "device node dev/$d missing from initrd"
done
bad=0
while IFS= read -r f; do
    rel="${f#./}"
    gzip -dc "$STAGE/initrd-i386" 2>/dev/null \
      | cpio --to-stdout -i "$rel" 2>/dev/null \
      | cmp -s - "$ROOT/$rel" || { echo "  ** MISMATCH: $rel"; bad=$((bad+1)); }
done < <(cd "$ROOT" && find . -type f)
[ "$bad" -eq 0 ] || die "initrd verify failed ($bad bad files)"
say "initrd OK: $want files, $got entries, $(stat -c%s "$STAGE/initrd-i386") bytes"

# ---------------------------------------------------------------- kernel ---
cp "$KERNEL" "$STAGE/linux-i386" || die "copy kernel"
say "kernel: $(basename "$KERNEL") $(stat -c%s "$STAGE/linux-i386") bytes"

# --------------------------------------------------------------- boot0 -----
cp "$BOOT0" "$STAGE/boot0" || die "copy boot0"

# -------------------------------------------------------------- version ----
# Keys and alphabetical order match stock. BLESSED is a release marker set by
# `swi bless`, NOT a signature -- see docs/ABOOT-AND-SWI-BUILD.md.
cat > "$STAGE/version" <<EOF
BLESSED=1
BUILD_DATE=$(date -u +%Y%m%dT%H%M%SZ)
BUILD_HOST=$(hostname)
SWI_RELEASE=$REL
SWI_VERSION=$VER
EOF

# ------------------------------------------------------------------ zip ----
rm -f "$OUT"
say "zipping (stored, no compression)"
( cd "$STAGE" && zip -0 -q -X "$OUT" version boot0 initrd-i386 linux-i386 ) \
    || die "zip failed"

# Verify the container: every member present, STORED, and byte-identical.
say "verifying SWI container"
python3 - "$OUT" "$STAGE" <<'PY' || die "SWI verify failed"
import sys, zipfile, hashlib, os
swi, stage = sys.argv[1], sys.argv[2]
want = ["version", "boot0", "initrd-i386", "linux-i386"]
z = zipfile.ZipFile(swi)
names = z.namelist()
if names != want:
    print("  ** member list/order wrong: %s" % names); sys.exit(1)
ok = True
for i in z.infolist():
    disk = open(os.path.join(stage, i.filename), "rb").read()
    inzip = z.read(i.filename)
    if i.compress_type != 0:
        print("  ** %s is not STORED (method=%d)" % (i.filename, i.compress_type)); ok = False
    if disk != inzip:
        print("  ** %s content differs" % i.filename); ok = False
    print("  %-14s method=%d %10d bytes  md5=%s"
          % (i.filename, i.compress_type, i.file_size,
             hashlib.md5(inzip).hexdigest()))
sys.exit(0 if ok else 1)
PY

echo
say "built $OUT"
say "  size    $(stat -c%s "$OUT") bytes"
say "  md5     $(md5sum "$OUT" | cut -d' ' -f1)"
say "  version $VER  release $REL"
echo
cat <<EOF
Next, and in this order -- see docs/IMAGE-BUILD.md:
  1. copy to the switch:  scp $OUT admin@10.1.1.242:/mnt/flash/edgenos.swi
  2. reload, Ctrl-C at the Aboot banner
  3. DRY RUN:  boot --testonly flash:/edgenos.swi
  4. only if that returns clean:  boot flash:/edgenos.swi

boot-config is never modified, so the next power cycle returns to EOS by itself.
EOF
