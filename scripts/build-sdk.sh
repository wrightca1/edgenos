#!/bin/bash
# build-sdk.sh - Build OpenMDK libraries (CDK, BMD, PHY) for PowerPC
set -e

TOPDIR="$(cd "$(dirname "$0")/.." && pwd)"
OPENMDK="$TOPDIR/asic/openmdk"
CROSS="powerpc-linux-gnu-"
OUTDIR="$TOPDIR/output/sdk"
JOBS=$(nproc)

if [ ! -d "$OPENMDK" ]; then
    echo "ERROR: OpenMDK not found at $OPENMDK"
    exit 1
fi

mkdir -p "$OUTDIR"/{lib,include}

echo "==> Building OpenMDK CDK..."
make -C "$OPENMDK/cdk" \
    CDK_CC="${CROSS}gcc" \
    CDK_AR="${CROSS}ar" \
    CDK_CFLAGS="-DBCM56846_A0 -DBCM56840_B0 -I$OPENMDK/cdk/include" \
    -j$JOBS

echo "==> Building OpenMDK BMD..."
make -C "$OPENMDK/bmd" \
    CDK="$OPENMDK/cdk" \
    BMD_CC="${CROSS}gcc" \
    BMD_AR="${CROSS}ar" \
    BMD_CFLAGS="-I$OPENMDK/cdk/include -I$OPENMDK/bmd/include" \
    -j$JOBS

echo "==> Building OpenMDK PHY..."
make -C "$OPENMDK/phy" \
    CDK="$OPENMDK/cdk" \
    PHY_CC="${CROSS}gcc" \
    PHY_AR="${CROSS}ar" \
    PHY_CFLAGS="-I$OPENMDK/cdk/include -I$OPENMDK/phy/include" \
    -j$JOBS

# Collect outputs
echo "==> Collecting SDK outputs..."
find "$OPENMDK" -name "*.a" -newer "$OPENMDK/Makefile" -exec cp {} "$OUTDIR/lib/" \;
cp -r "$OPENMDK/cdk/include/"* "$OUTDIR/include/" 2>/dev/null || true
cp -r "$OPENMDK/bmd/include/"* "$OUTDIR/include/" 2>/dev/null || true
cp -r "$OPENMDK/phy/include/"* "$OUTDIR/include/" 2>/dev/null || true

echo "==> OpenMDK build complete"
ls -la "$OUTDIR/lib/"
