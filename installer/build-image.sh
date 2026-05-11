#!/bin/bash
# build-image.sh - Assemble ONIE-compatible .bin installer
#
# Combines installer header + tar archive (FIT + rootfs).
#
# Usage:
#   build-image.sh                # single-slot (default)
#   build-image.sh --dual-slot    # Cumulus-style dual-slot layout

set -e

TOPDIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="$TOPDIR/output/images"

INSTALLER_HEADER="$TOPDIR/installer/install.sh"
OUTPUT="$OUTDIR/edgenos-as5610-52x.bin"

case "${1:-}" in
    --dual-slot|dual-slot)
        INSTALLER_HEADER="$TOPDIR/installer/install-dual-slot.sh"
        OUTPUT="$OUTDIR/edgenos-as5610-52x-dualslot.bin"
        echo "==> Building DUAL-SLOT ONIE installer..."
        ;;
    "")
        echo "==> Building single-slot ONIE installer..."
        ;;
    *)
        echo "Unknown option: $1"
        echo "Usage: $0 [--dual-slot]"
        exit 1
        ;;
esac

FIT="$OUTDIR/uImage-powerpc.itb"
ROOTFS="$OUTDIR/rootfs.sqsh"

for f in "$FIT" "$ROOTFS" "$INSTALLER_HEADER"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: Required file not found: $f"
        exit 1
    fi
done

TMPDIR=$(mktemp -d)
cp "$FIT"    "$TMPDIR/uImage-powerpc.itb"
cp "$ROOTFS" "$TMPDIR/rootfs.sqsh"

(cd "$TMPDIR" && tar cf payload.tar uImage-powerpc.itb rootfs.sqsh)

cp "$INSTALLER_HEADER" "$OUTPUT"
cat "$TMPDIR/payload.tar" >> "$OUTPUT"

chmod +x "$OUTPUT"
rm -rf "$TMPDIR"

echo "==> ONIE installer: $OUTPUT"
ls -lh "$OUTPUT"
echo ""
echo "Install via: onie-nos-install <url-to-installer.bin>"
