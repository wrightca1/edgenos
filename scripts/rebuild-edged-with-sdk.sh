#!/bin/bash
# rebuild-edged-with-sdk.sh — rebuild the OpenMDK SDK libs AND edged from the
# CURRENT source tree, producing a real ~18.9 MB edged at output/edged-rebuilt.
#
# WHY THIS EXISTS
# --------------
# edged statically links 16 SDK libs from output/sdk/{bmd,phy,cdk,libbde}/*.a.
# Those libs are built by `make -C asic/mdk-init` (NOT the broken build-sdk.sh).
# Rebuilding them in-place fails for two reasons that cost hours on 2026-06-03:
#   1. The repo's SDK build dirs (asic/openmdk/*/build) are often root-owned from
#      a prior root docker build → a rootless container can't write them.
#   2. asic/openmdk/*/tools/instpkgs.pl (generates the per-chip pkgsrc tree) is
#      NON-idempotent: it errors if pkgsrc/ is missing, AND errors creating chip
#      subdirs that already exist. So re-running it over a pre-generated tree dies.
#
# THE WORKING RECIPE (what this script does), all inside the builder container:
#   - copy asic/{openmdk,mdk-init,edged} to a writable /tmp (sidesteps root-owned)
#   - copy your edited PHY chip driver(s) into the *generated* pkgsrc copies
#     (pkgsrc/PKG/chip AND pkgsrc/chip), not just PKG/chip
#   - `touch` the whole pkgsrc tree so instpkgs sees "up to date" and SKIPS regen
#   - `make -C /tmp/mdk-init ... BLDDIR=/tmp/sdk`  → builds all 16 libs with PHY
#     changes (the "_bde_dma_alloc / mdk-init tool link failed" warning is BENIGN
#     — the libs build before it)
#   - copy the 16 libs into output/sdk, then `make -C asic/edged SDK_BLDDIR=output/sdk`
#
# USAGE:  scripts/rebuild-edged-with-sdk.sh
#   Edit your PHY source under asic/openmdk/phy/PKG/chip/<chip>/ first.
#   IMPORTANT: this script syncs PKG/chip → the pkgsrc copies for the warpcore
#   driver only. To rebuild a different chip, extend WARPCORE_REL below.
#
# Output: output/edged-rebuilt (verify it is ~18.9 MB, NOT ~2 MB — a ~2 MB binary
#         means the SDK libs were inconsistent/incomplete and it WILL crash).
set -euo pipefail

cd "$(dirname "$0")/.."
TOP=$(pwd)
export DOCKER_HOST=${DOCKER_HOST:-unix:///run/user/1000/docker.sock}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/1000}
IMG=edgenos-builder
# PHY chip driver(s) whose PKG/chip master should be synced into the pkgsrc copies.
WARPCORE_REL=phy/PKG/chip/bcmi_warpcore_xgxs/bcmi_warpcore_xgxs_drv.c

LOG=output/rebuild-edged-$(date +%Y%m%d-%H%M%S).log
echo "Rebuilding SDK + edged from current source — log: $LOG"

docker run --rm -v "$TOP:/build/src" --entrypoint /bin/bash "$IMG" -c '
  set -e
  cp -r /build/src/asic/openmdk /tmp/openmdk
  cp -r /build/src/asic/mdk-init /tmp/mdk-init
  cp -r /build/src/asic/edged   /tmp/edged
  # sync edited warpcore PKG/chip master into the generated pkgsrc copies
  M=/build/src/asic/openmdk/'"$WARPCORE_REL"'
  for w in /tmp/openmdk/phy/pkgsrc/PKG/chip/bcmi_warpcore_xgxs/bcmi_warpcore_xgxs_drv.c \
           /tmp/openmdk/phy/pkgsrc/chip/bcmi_warpcore_xgxs/bcmi_warpcore_xgxs_drv.c; do
    [ -f "$w" ] && cp "$M" "$w"
  done
  # make pkgsrc look up-to-date so instpkgs SKIPS its non-idempotent regen
  find /tmp/openmdk -path "*/pkgsrc/*" -exec touch {} + 2>/dev/null || true
  rm -rf /tmp/openmdk/*/build; mkdir -p /tmp/sdk
  # build the 16 SDK libs (mdk-init test-tool link failure at the end is benign)
  make -C /tmp/mdk-init CROSS_COMPILE=powerpc-linux-gnu- MDK=/tmp/openmdk BLDDIR=/tmp/sdk -j"$(nproc)" \
       >/tmp/mdk.log 2>&1 || echo "(mdk-init tool link warn — expected; libs built)"
  N=$(find /tmp/sdk -name "*.a" | wc -l); echo "SDK libs built: $N (expect 16)"
  [ "$N" -ge 16 ] || { echo "ERROR: incomplete SDK build"; tail -30 /tmp/mdk.log; exit 1; }
  for d in bmd phy cdk libbde; do mkdir -p /build/src/output/sdk/$d; cp /tmp/sdk/$d/*.a /build/src/output/sdk/$d/; done
  cd /tmp/edged && rm -f edged *.o
  make CROSS_COMPILE=powerpc-linux-gnu- SDK_BLDDIR=/tmp/sdk 2>&1 | tail -2
  SZ=$(stat -c%s edged)
  echo "edged size: $SZ"
  [ "$SZ" -gt 15000000 ] || { echo "ERROR: edged too small ($SZ) — SDK inconsistent"; exit 1; }
  cp edged /build/src/output/edged-rebuilt
  echo "OK -> output/edged-rebuilt"
' 2>&1 | tee "$LOG"

echo "Done. Verify: ls -l output/edged-rebuilt (must be ~18.9 MB)"
