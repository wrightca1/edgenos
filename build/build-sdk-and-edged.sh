#!/usr/bin/env bash
# Full from-source 5610 build: OpenMDK SDK libs + linked edged, from the unified tree.
# The SDK build harness (mdk-init/openmdk) is still sourced from newnos/ pending its
# own migration; OpenMDK headers/libs are the shared checkout. Produces
# edgenos/output/edged and edgenos/output/sdk/*.a.
#
# Usage: build/build-sdk-and-edged.sh   (from the edgenos/ repo root)
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
TOP=$(cd "$HERE/.." && pwd)
IMG=${IMG:-debian:bullseye}

docker run --rm --network host -v "$TOP:/src" --entrypoint /bin/bash "$IMG" -c '
  set -e
  command -v powerpc-linux-gnu-gcc >/dev/null || { apt-get update -qq >/dev/null; \
    apt-get install -y -qq gcc-powerpc-linux-gnu make perl >/dev/null; }
  echo "== building OpenMDK SDK libs (mdk-init) =="
  cp -r /src/newnos/asic/openmdk /tmp/openmdk
  cp -r /src/newnos/asic/mdk-init /tmp/mdk-init
  find /tmp/openmdk -path "*/pkgsrc/*" -exec touch {} + 2>/dev/null || true
  rm -rf /tmp/openmdk/*/build; mkdir -p /tmp/sdk
  make -C /tmp/mdk-init CROSS_COMPILE=powerpc-linux-gnu- MDK=/tmp/openmdk BLDDIR=/tmp/sdk \
       -j"$(nproc)" >/tmp/mdk.log 2>&1 || echo "(mdk-init tool link warn — expected)"
  N=$(find /tmp/sdk -name "*.a" | wc -l); echo "SDK libs: $N (expect >=16)"
  [ "$N" -ge 16 ] || { tail -25 /tmp/mdk.log; exit 1; }
  for d in bmd phy cdk libbde; do mkdir -p /src/edgenos/output/sdk/$d; cp /tmp/sdk/$d/*.a /src/edgenos/output/sdk/$d/; done
  echo "== linking unified edged =="
  make -C /src/edgenos/platform/accton-as5610-52x EDGENOS_ROOT=/src/edgenos \
       SDK_BLDDIR=/src/edgenos/output/sdk 2>&1 | tail -3
  SZ=$(stat -c%s /src/edgenos/output/edged 2>/dev/null || echo 0)
  echo "unified edged size: $SZ"
  [ "$SZ" -gt 15000000 ] && echo "EDGED_BUILD_OK" || { echo "ERROR: edged too small/missing"; exit 1; }
' 2>&1 | grep -vE "mesg:|ttyname|debconf:"
