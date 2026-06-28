#!/usr/bin/env bash
# Wrap an EdgeNOS-4610 .swi into a self-extracting ONIE installer (ONL mkshar/sfx).
# Mirrors edgecore-4610-54t/nos/build-61-installer.sh, but takes the unified-tree SWI.
# Reuses the proven loader FIT + the stock installer.sh/sfx payload; only the FIT, SWI,
# boot-config, and the loader-initrd offset/size are swapped, then repackaged.
#
# Env: SWI (default: the unified 4610 image), FIT, STOCK_INST, IMG.
# Output: edgenos/output/images/onie-installer-<swi-basename>
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)              # edgenos/
EDGE=$(cd "$HERE/.." && pwd)                        # repo top
T="$EDGE/edgecore-4610-54t"; ONL="$EDGE/OpenNetworkLinux"
IMG="${IMG:-edgenos/builder9:1.8-rootless}"

SWI="${SWI:-$HERE/output/images/EdgeNOS-0.1.0-arm-accton-as4610-54-r0.swi}"
FIT="${FIT:-$T/output/kport61/arm-accton-as4610-54-r0-61.itb}"
STOCK_INST="${STOCK_INST:-$T/output/onie-installer-edgenos-419}"
OUT="$HERE/output/images/onie-installer-$(basename "${SWI%.swi}")"
WORK="$HERE/output/inst-build"

for f in "$SWI" "$FIT" "$STOCK_INST" "$ONL/tools/scripts/sfx.sh.in" "$ONL/tools/mkshar"; do
  [ -e "$f" ] || { echo "ERROR missing $f" >&2; exit 1; }
done

rm -rf "$WORK"; mkdir -p "$WORK/src" "$WORK/stage"
unzip -q "$STOCK_INST" -d "$WORK/src" || true
[ -f "$WORK/src/installer.sh" ] || { echo "ERROR: payload extract failed" >&2; exit 1; }
cp "$WORK/src/installer.sh" "$WORK/src/autoperms.sh" \
   "$WORK/src/preinstall.sh" "$WORK/src/postinstall.sh" "$WORK/stage/"
cp -r "$WORK/src/config" "$WORK/src/plugins" "$WORK/stage/"
cp "$FIT" "$WORK/stage/onl-loader-fit.itb"
cp "$SWI" "$WORK/stage/$(basename "$SWI")"
printf 'NETDEV=ma1\nBOOTMODE=SWI\nSWI=images:%s\n' "$(basename "$SWI")" > "$WORK/stage/boot-config"

# recompute loader-initrd offset/size for this FIT and patch installer.sh
VONL="$ONL/packages/base/all/vendor-config-onl"
read -r OFF LAST < <(docker run --rm -u root:0 -v "$EDGE":"$EDGE" "$IMG" bash -lc \
  "PYTHONPATH=$VONL/src/python python2 $VONL/src/bin/pyfit offset '$FIT' --initrd 2>/dev/null")
SIZE=$(( LAST - OFF + 1 ))
echo "FIT initrd: offset=$OFF size=$SIZE"
sed -i -e "s/^initrd_offset=.*/initrd_offset=\"$OFF\"/" \
       -e "s/^initrd_size=.*/initrd_size=\"$SIZE\"/" "$WORK/stage/installer.sh"

docker run --rm -u root:0 -v "$EDGE":"$EDGE" -w "$WORK/stage" "$IMG" bash -lc "
  set -e
  python2 '$ONL/tools/mkshar' --lazy --unzip-pad --fixup-perms autoperms.sh \
    out.shar '$ONL/tools/scripts/sfx.sh.in' installer.sh \
    onl-loader-fit.itb '$(basename "$SWI")' boot-config preinstall.sh postinstall.sh \
    config plugins
"
cp "$WORK/stage/out.shar" "$OUT"; chmod +x "$OUT"
( cd "$(dirname "$OUT")" && md5sum "$(basename "$OUT")" > "$(basename "$OUT").md5sum" )
rm -rf "$WORK"
echo "==> built $OUT"; ls -lh "$OUT"
