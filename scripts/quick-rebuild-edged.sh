#!/bin/bash
# Minimal incremental rebuild: just bcm56840_a0_bmd_rx.o + relink libbmdpkgsrc.a + relink edged
# Runs inside a Debian Bullseye container with PPC cross-compiler.
set -e

cd "$(dirname "$0")/.."
SRC=$(pwd)

# Already-built SDK lives in output/sdk; we keep all other .o files as-is.
test -f output/sdk/bmd/libbmdpkgsrc.a || { echo "ERROR: SDK not built (run build-all.sh first)"; exit 1; }
test -f asic/edged/edged || { echo "ERROR: edged binary missing (run build-all.sh first)"; exit 1; }

docker run --rm --network=host \
  -v "$SRC":/src:rw \
  -w /src \
  debian:bullseye bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential gcc-powerpc-linux-gnu binutils-powerpc-linux-gnu \
  libc6-dev-powerpc-cross make >/dev/null

CC=powerpc-linux-gnu-gcc
AR=powerpc-linux-gnu-ar
LIB=output/sdk/bmd/libbmdpkgsrc.a
SRCS="bcm56840_a0_bmd_rx bcm56840_a0_bmd_tx bcm56840_a0_bmd_attach bcm56840_a0_bmd_switching_init"

# Same flags used in the original build (extracted from a working .d/.o pair)
CFLAGS="-Wall -O2 -g"
CPPFLAGS="-DBCM56846_A0 -DBCM56840_B0 -DBCM56840_A0 \
  -DCDK_CONFIG_INCLUDE_BCM56846_A0=1 \
  -DCDK_CONFIG_INCLUDE_BCM56840_A0=1 \
  -DCDK_CONFIG_INCLUDE_BCM56840_B0=1 \
  -DBMD_CONFIG_INCLUDE_DMA=1 \
  -DCDK_INCLUDE_CUSTOM_CONFIG \
  -DSYS_BE_PIO=1 -DSYS_BE_PACKET=1 -DSYS_BE_OTHER=1 \
  -DBMD_SYS_USLEEP=_usleep -DPHY_SYS_USLEEP=_usleep \
  -DBMD_SYS_DMA_ALLOC_COHERENT=_bde_dma_alloc \
  -DBMD_SYS_DMA_FREE_COHERENT=_bde_dma_free \
  -DBMD_SYS_DMA_CACHE_FLUSH(addr,len)= \
  -DBMD_SYS_DMA_CACHE_INVAL(addr,len)= \
  -I/src/asic/edged \
  -I/src/asic/openmdk/cdk/include \
  -I/src/asic/openmdk/bmd/include \
  -I/src/asic/openmdk/phy/include \
  -I/src/asic/openmdk/bmd/pkgsrc"

for name in $SRCS; do
  srcf=asic/openmdk/bmd/pkgsrc/chip/bcm56840_a0/${name}.c
  objf=output/sdk/bmd/obj/pkgsrc/${name}.o
  echo "==> Recompiling $srcf"
  $CC $CFLAGS $CPPFLAGS -c -o $objf $srcf
  echo "==> Updating $LIB with $(basename $objf)"
  $AR rcs $LIB $objf
done

echo "==> Relinking edged"
# Re-run the edged Makefile (will detect lib changed and relink)
touch $LIB
rm -f asic/edged/edged
make -C asic/edged \
  CROSS_COMPILE=powerpc-linux-gnu- \
  OPENMDK=/src/asic/openmdk \
  TOPDIR=/src \
  SDK_BLDDIR=/src/output/sdk 2>&1 | tail -20

ls -lh asic/edged/edged
'
echo "==> Done. Binary at: $SRC/asic/edged/edged"
