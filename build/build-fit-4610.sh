#!/usr/bin/env bash
# Build the AS4610 (6.1) loader FIT from the CANONICAL edgenos device tree.
# Single source of truth: compiles edgenos/platform/accton-as4610-54/dts/*-rtcdis.dts
# (no longer the edgecore-4610-54t fork copy). Kernel Image + loader initrd are
# referenced build artifacts (like the SDKs), not vendored.
#
# Env: KIMAGE (kernel Image), INITRD (loader initrd cpio.gz), IMG (builder).
# Output: edgenos/output/images/arm-accton-as4610-54-r0.itb
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)            # edgenos/
EDGE=$(cd "$HERE/.." && pwd)
IMG="${IMG:-edgenos/builder9:1.8-rootless}"
DTS="$HERE/platform/accton-as4610-54/dts/arm-accton-as4610-rtcdis.dts"
KIMAGE="${KIMAGE:-$EDGE/edgecore-4610-54t/output/kport61/linux-6.1.175/arch/arm/boot/Image}"
INITRD="${INITRD:-$EDGE/OpenNetworkLinux/REPO/stretch/extracts/onl-loader-initrd_armhf/usr/share/onl/packages/armhf/onl-loader-initrd/onl-loader-initrd-armhf.cpio.gz}"
OUT="$HERE/output/images/arm-accton-as4610-54-r0.itb"
WORK="$HERE/output/fit-4610"
for f in "$DTS" "$KIMAGE" "$INITRD"; do [ -e "$f" ] || { echo "ERROR missing $f" >&2; exit 1; }; done
rm -rf "$WORK"; mkdir -p "$WORK" "$(dirname "$OUT")"

cat > "$WORK/fit.its" <<ITS
/dts-v1/;
/ { description = "EdgeNOS AS4610 6.1 loader FIT";
  images {
    kernel-61 { description="linux-6.1.175-iproc-helix4"; data=/incbin/("kernel.gz");
      type="kernel"; arch="arm"; os="linux"; compression="gzip"; load=<0x61008000>; entry=<0x61008000>; hash@1{algo="crc32";}; };
    fdt-61 { description="arm-accton-as4610-rtcdis.dtb"; data=/incbin/("board.dtb"); type="flat_dt"; arch="arm"; compression="none"; };
    initrd-61 { description="onl-loader-initrd"; data=/incbin/("initrd.cpio.gz"); type="ramdisk"; arch="arm"; os="linux"; compression="gzip"; load=<0x0>; entry=<0x0>; hash@1{algo="crc32";}; };
  };
  configurations { default="arm-accton-as4610-54-r0";
    arm-accton-as4610-54-r0 { description="as4610-54 6.1"; kernel="kernel-61"; ramdisk="initrd-61"; fdt="fdt-61"; };
    arm-accton-as4610-30-r0 { description="as4610-30 6.1"; kernel="kernel-61"; ramdisk="initrd-61"; fdt="fdt-61"; };
  };
};
ITS
gzip -nc "$KIMAGE" > "$WORK/kernel.gz"
cp "$INITRD" "$WORK/initrd.cpio.gz"

docker run --rm -u root:0 -e SOURCE_DATE_EPOCH=1717480800 -v "$EDGE":"$EDGE" -w "$WORK" "$IMG" bash -lc "
  set -e
  dtc -I dts -O dtb '$DTS' -o board.dtb 2>/dev/null
  mkimage -f fit.its out.itb >/dev/null
" 2>&1 | grep -viE "mesg:|ttyname|unit_address_vs_reg" || true
cp "$WORK/out.itb" "$OUT"; rm -rf "$WORK"
( cd "$(dirname "$OUT")" && md5sum "$(basename "$OUT")" )
echo "==> AS4610 FIT (canonical): $OUT"
