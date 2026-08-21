#!/usr/bin/env bash
# Full from-source 5610 build: OpenMDK SDK libs + linked edged, from the unified tree.
# The SDK build harness (mdk-init/openmdk) is still sourced from newnos/ pending its
# own migration; OpenMDK headers/libs are the shared checkout. Produces
# edgenos/output/edged and edgenos/output/sdk/*.a.
#
# Requires the shared OpenMDK checkout at ../OpenMDK (sibling of edgenos/):
#   git clone https://github.com/Broadcom-Network-Switching-Software/OpenMDK
# and the SDK build harness at ../newnos/asic/mdk-init.
#
# JOBS caps build parallelism (default 2) -- the 58-chip SDK compile will
# otherwise saturate the build host.
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
  # Build IN the shared OpenMDK checkout, not a /tmp copy. `make instpkgs`
  # generates cdk_config_chips.h (and the per-chip sources it lists) into
  # $CDK/include inside the tree; a /tmp copy throws those away with the
  # container, and the edged compile -- which includes ../OpenMDK/cdk/include
  # -- then dies on a missing cdk_config_chips.h.
  # BLDDIR is persistent too, so a rebuild is incremental instead of a
  # from-scratch 58-chip compile.
  BLD=/src/edgenos/output/sdk-build
  mkdir -p "$BLD"
  make -C /src/newnos/asic/mdk-init CROSS_COMPILE=powerpc-linux-gnu- \
       MDK=/src/OpenMDK BLDDIR="$BLD" \
       -j"${JOBS:-2}" >/tmp/mdk.log 2>&1 || echo "(mdk-init tool link warn — expected)"
  N=$(find "$BLD" -name "*.a" | wc -l); echo "SDK libs: $N (expect >=16)"
  [ "$N" -ge 16 ] || { tail -25 /tmp/mdk.log; exit 1; }
  for d in bmd phy cdk libbde; do mkdir -p /src/edgenos/output/sdk/$d; cp "$BLD"/$d/*.a /src/edgenos/output/sdk/$d/; done
  echo "== linking unified edged =="
  make -C /src/edgenos/platform/accton-as5610-52x EDGENOS_ROOT=/src/edgenos \
       SDK_BLDDIR=/src/edgenos/output/sdk -j"${JOBS:-2}" 2>&1 | tail -3
  SZ=$(stat -c%s /src/edgenos/output/edged 2>/dev/null || echo 0)
  echo "unified edged size: $SZ"
  [ "$SZ" -gt 15000000 ] && echo "EDGED_BUILD_OK" || { echo "ERROR: edged too small/missing"; exit 1; }
' 2>&1 | grep -vE "mesg:|ttyname|debconf:"
