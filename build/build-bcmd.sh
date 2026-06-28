#!/usr/bin/env bash
# Build bcmd for the AS4610 from the unified tree (asic/bcm56340/bcmd.c), in the
# ARM/iProc cross-build container.
#
# Like edged's SDK, the OpenBCM SDK is a shared vendor dependency (not vendored in
# edgenos/). bcmd is an incremental build: bcmd.c is appended to the SDK's
# socdiag.c and its REPL diverted (diag_shell -> bcmd_run); socdiag.o is recompiled
# and bcm.user relinked against the pre-built SDK libs -> bcmd. socdiag.c is
# restored on exit, so the SDK tree is left pristine.
#
# Env overrides:
#   SDK     OpenBCM sdk-6.5.16 with pre-built libs (default: the sdk-ref checkout)
#   KERNDIR matching kernel source (for KNET)
#   IMG     ARM builder image
#
# Usage: build/build-bcmd.sh   (from the edgenos/ repo root)
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)              # edgenos/
TOP=$(cd "$HERE/.." && pwd)                         # repo top
BCMD_C="$HERE/asic/bcm56340/bcmd.c"

SDK="${SDK:-$TOP/edgecore-4610-54t/live-investigation/sdk-ref/OpenBCM/sdk-6.5.16}"
KERNDIR="${KERNDIR:-$TOP/OpenNetworkLinux/packages/base/armhf/kernels/kernel-4.14-lts-armhf-iproc-all/builds/stretch/linux-4.14.151}"
IMG="${IMG:-edgenos/builder9:1.8-rootless}"
OUT="$HERE/output/bcmd"

SOCDIAG="$SDK/systems/linux/user/common/socdiag.c"
BCMUSER="$SDK/build/linux/user/iproc-4_4/bcm.user"
[ -f "$BCMD_C" ]  || { echo "bcmd.c missing at $BCMD_C" >&2; exit 1; }
[ -f "$SOCDIAG" ] || { echo "socdiag.c not found at $SOCDIAG" >&2; exit 1; }
ls "$SDK"/build/unix-user/iproc-4_4/libbcm.a >/dev/null 2>&1 \
  || { echo "OpenBCM SDK libs not built (need a prior bcm.user build) at $SDK" >&2; exit 1; }

# patch socdiag.c with a restore trap
BAK="$SOCDIAG.edgenosbak"
restore() { [ -f "$BAK" ] && mv -f "$BAK" "$SOCDIAG" && echo "[bcmd] socdiag.c restored"; }
trap restore EXIT
cp -f "$SOCDIAG" "$BAK"
grep -q "diag_shell();" "$SOCDIAG" || { echo "diag_shell(); not in socdiag.c" >&2; exit 1; }
sed -i 's/diag_shell();/bcmd_run();/' "$SOCDIAG"
{ echo ""; echo "/* ==== appended by edgenos build-bcmd.sh: AS4610 datapath ==== */"; cat "$BCMD_C"; } >> "$SOCDIAG"
echo "[bcmd] socdiag.c patched (diag_shell -> bcmd_run, asic/bcm56340/bcmd.c appended)"

rm -f "$SDK"/build/*/user/iproc-4_4/socdiag.o "$BCMUSER" "$BCMUSER.dbg" 2>/dev/null || true

echo "== building bcmd ($IMG) =="
docker run --rm -u root:0 \
  -v "$SDK":/sdk -v "$KERNDIR":/kern:ro -w /sdk/systems/linux/user/iproc-4_4 \
  "$IMG" bash -lc '
    set -e
    KINC=$(arm-linux-gnueabihf-gcc -print-file-name=include)
    ADD_TO_CFLAGS="-Wno-error -Wno-cpp -DINCLUDE_KNET -I/sdk/systems/linux/kernel/modules/include" \
    make SDK=/sdk CROSS_COMPILE=arm-linux-gnueabihf- KERNDIR=/kern \
         TOOLCHAIN_BASE_DIR=/usr KFLAG_INCLD="$KINC" LINUX_MAKE_USER=1 \
         BUILD_KNET=1 MAKE=make -j"$(nproc)" bcm 2>&1
  ' 2>&1 | tail -20

[ -f "$BCMUSER" ] || { echo "[bcmd] build did not produce bcm.user" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
cp -f "$BCMUSER" "$OUT"
docker run --rm -u root:0 -v "$TOP":"$TOP" "$IMG" arm-linux-gnueabihf-strip "$OUT" 2>/dev/null || true
echo "== result =="; file "$OUT"; echo "unified bcmd -> $OUT"
