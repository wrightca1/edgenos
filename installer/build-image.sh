#!/bin/bash
# build-image.sh - Assemble ONIE-compatible .bin installer
#
# Combines install.sh header + tar archive (FIT + rootfs)

set -e

TOPDIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="$TOPDIR/output/images"

FIT="$OUTDIR/uImage-powerpc.itb"
ROOTFS="$OUTDIR/rootfs.sqsh"
INSTALLER="$TOPDIR/installer/install.sh"
OUTPUT="$OUTDIR/edgenos-as5610-52x.bin"

for f in "$FIT" "$ROOTFS" "$INSTALLER"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: Required file not found: $f"
        exit 1
    fi
done

echo "==> Building ONIE installer image..."

# Create temporary archive with FIT image and rootfs
TMPDIR=$(mktemp -d)
cp "$FIT" "$TMPDIR/uImage-powerpc.itb"
cp "$ROOTFS" "$TMPDIR/rootfs.sqsh"

# Create tar archive
(cd "$TMPDIR" && tar cf payload.tar uImage-powerpc.itb rootfs.sqsh)

# Combine: installer script + tar payload
cp "$INSTALLER" "$OUTPUT"
cat "$TMPDIR/payload.tar" >> "$OUTPUT"

chmod +x "$OUTPUT"
rm -rf "$TMPDIR"

echo "==> ONIE installer: $OUTPUT"
ls -lh "$OUTPUT"
echo ""
echo "Install via: onie-nos-install $OUTPUT"
