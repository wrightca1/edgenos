#!/usr/bin/env bash
# Build the AS5610 (PowerPC) FIT from the CANONICAL edgenos device tree.
# Single source of truth: compiles edgenos/platform/accton-as5610-52x/dts/as5610-52x.dts
# (not the newnos fork copy). Kernel + initramfs are referenced build artifacts.
# dtc/mkimage are arch-agnostic, so this runs in the same (arm) builder container.
#
# Env: KERNEL_GZ, INITRAMFS, IMG. Output: edgenos/output/images/uImage-powerpc.itb
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
EDGE=$(cd "$HERE/.." && pwd)
IMG="${IMG:-edgenos/builder9:1.8-rootless}"
DTS="$HERE/platform/accton-as5610-52x/dts/as5610-52x.dts"
KERNEL_GZ="${KERNEL_GZ:-$EDGE/newnos/output/images/kernel.gz}"
INITRAMFS="${INITRAMFS:-$EDGE/newnos/output/images/initramfs.cpio.gz}"
OUT="$HERE/output/images/uImage-powerpc.itb"
WORK="$HERE/output/fit-5610"
for f in "$DTS" "$KERNEL_GZ" "$INITRAMFS"; do [ -e "$f" ] || { echo "ERROR missing $f" >&2; exit 1; }; done
rm -rf "$WORK"; mkdir -p "$WORK" "$(dirname "$OUT")"

cat > "$WORK/fit.its" <<'ITS'
/dts-v1/;
/ { description = "EdgeNOS AS5610-52X PowerPC FIT";
  #address-cells = <1>;
  images {
    kernel { description="PowerPC Kernel"; data=/incbin/("kernel.gz"); type="kernel"; arch="ppc"; os="linux"; compression="gzip"; load=<0x00>; entry=<0x00>; hash@1{algo="crc32";}; };
    initramfs { description="initramfs"; data=/incbin/("initramfs.cpio.gz"); type="ramdisk"; arch="ppc"; os="linux"; compression="gzip"; load=<0x00>; entry=<0x00>; hash@1{algo="crc32";}; };
    accton_as5610_52x_dtb { description="accton_as5610_52x.dtb"; data=/incbin/("board.dtb"); type="flat_dt"; arch="ppc"; compression="none"; hash@1{algo="crc32";}; };
  };
  configurations { default="accton_as5610_52x";
    accton_as5610_52x { description="accton_as5610_52x"; kernel="kernel"; ramdisk="initramfs"; fdt="accton_as5610_52x_dtb"; };
  };
};
ITS
cp "$KERNEL_GZ" "$WORK/kernel.gz"
cp "$INITRAMFS" "$WORK/initramfs.cpio.gz"

docker run --rm -u root:0 -e SOURCE_DATE_EPOCH=1717480800 -v "$EDGE":"$EDGE" -w "$WORK" "$IMG" bash -lc "
  set -e
  dtc -I dts -O dtb '$DTS' -o board.dtb 2>/dev/null
  mkimage -f fit.its out.itb >/dev/null
" 2>&1 | grep -viE "mesg:|ttyname|unit_address_vs_reg" || true
cp "$WORK/out.itb" "$OUT"; rm -rf "$WORK"
( cd "$(dirname "$OUT")" && md5sum "$(basename "$OUT")" )
echo "==> AS5610 FIT (canonical): $OUT"
