#!/usr/bin/env bash
# Build edged for the AS5610 from the unified tree (core/datapath + asic/bcm56846 +
# platform/accton-as5610-52x), in a PowerPC cross-build container.
#
# Compiles every source to objects (this is the migration's compile proof). If the
# OpenMDK SDK static libs are present at output/sdk/, it also links a real edged.
# OpenMDK is the shared checkout at <repo-top>/OpenMDK (sibling of edgenos/).
#
# Usage: build/build-edged.sh            (run from the edgenos/ repo root)
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)              # edgenos/
TOP=$(cd "$HERE/.." && pwd)                         # repo top (has OpenMDK/)
IMG=${IMG:-debian:bullseye}

docker run --rm --network host -v "$TOP:/src" -w /src/edgenos --entrypoint /bin/bash "$IMG" -c '
  set -e
  echo "== ensuring PowerPC toolchain =="
  if ! command -v powerpc-linux-gnu-gcc >/dev/null; then
    apt-get update -qq >/dev/null && apt-get install -y -qq gcc-powerpc-linux-gnu make >/dev/null
  fi
  powerpc-linux-gnu-gcc --version | head -1

  CC=powerpc-linux-gnu-gcc
  OPENMDK=/src/OpenMDK
  INC="-I$OPENMDK/cdk/include -I$OPENMDK/bmd/include -I$OPENMDK/phy/include -I$OPENMDK/libbde/include \
       -Icore/datapath -Iasic/bcm56846 -Iplatform/accton-as5610-52x"
  DEF="-DCDK_INCLUDE_CUSTOM_CONFIG -DUSE_SYSTEM_LIBC -DBMD_CONFIG_INCLUDE_DMA=1 \
       -DBMD_SYS_USLEEP=_usleep -DPHY_SYS_USLEEP=_usleep \
       -DSYS_BE_PIO=1 -DSYS_BE_PACKET=1 -DSYS_BE_OTHER=1 \
       -DBMD_SYS_DMA_ALLOC_COHERENT=_bde_dma_alloc -DBMD_SYS_DMA_FREE_COHERENT=_bde_dma_free"
  DEF="$DEF -DBMD_SYS_DMA_CACHE_FLUSH(addr,len)= -DBMD_SYS_DMA_CACHE_INVAL(addr,len)="

  SRCS="core/datapath/edged.c core/datapath/packet_io.c core/datapath/netlink.c \
        core/datapath/l2.c core/datapath/l3.c core/datapath/vlan.c core/datapath/datapath.c \
        asic/bcm56846/bde_interface.c asic/bcm56846/cumulus_replicate.c \
        platform/accton-as5610-52x/portmap.c platform/accton-as5610-52x/led.c"

  echo "== compiling unified edged sources (PowerPC) =="
  mkdir -p /tmp/obj; rc=0; n=0
  for f in $SRCS; do
    if $CC -Wall -O2 $DEF $INC -c "$f" -o "/tmp/obj/$(basename "${f%.c}").o" 2>/tmp/e; then
      echo "  OK   $f"; n=$((n+1))
    else
      echo "  FAIL $f"; sed "s/^/      /" /tmp/e; rc=1
    fi
  done
  echo "compiled $n/11 objects"

  if [ "$rc" = 0 ] && ls /src/edgenos/output/sdk/cdk/libcdkmain.a >/dev/null 2>&1; then
    echo "== SDK libs present — linking edged =="
    make -C platform/accton-as5610-52x EDGENOS_ROOT=/src/edgenos \
         SDK_BLDDIR=/src/edgenos/output/sdk 2>&1 | tail -3
  else
    echo "== link skipped (build OpenMDK SDK libs into output/sdk to produce a binary) =="
  fi
  exit $rc
' 2>&1 | grep -vE "mesg:|ttyname|debconf:"
