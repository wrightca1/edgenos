#!/bin/bash
# build-kmodules.sh — cross-build the out-of-tree platform kernel modules
# (cpld, bde, tmon, retimer) against a freshly-built 5.10.224 kernel tree, so
# the corrected driver SOURCE actually becomes a .ko. Drops the .ko's in
# output/modules/ for assemble-rootfs-from-base.sh to install into the image.
#
# WHY: assemble-rootfs-from-base.sh reuses the base rootfs's stale modules and
# never recompiles them, so source fixes to platform/cpld never reached the
# image (the as5610_52x_cpld fan_pwm NULL-deref fix in particular). Run this to
# regenerate the .ko's whenever a platform driver changes.
#
# Builds the kernel exactly as build-all.sh does (same KVER, defconfig, patches)
# so module vermagic + symbol CRCs match the kernel shipped in the image.
set -e

KVER="5.10.224"
TOP=$(cd "$(dirname "$0")/.." && pwd)
export DOCKER_HOST=${DOCKER_HOST:-unix:///run/user/1000/docker.sock}

mkdir -p "$TOP/output/modules"

docker run --rm --network=host \
  -v "$TOP:/src:ro" \
  -v "$TOP/output:/build/output" \
  --entrypoint /bin/bash debian:bookworm -c '
set -e
KVER="'"$KVER"'"
KSRC="/build/linux-${KVER}"
export DEBIAN_FRONTEND=noninteractive

echo "==> installing kernel build deps..."
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential gcc-powerpc-linux-gnu g++-powerpc-linux-gnu \
  binutils-powerpc-linux-gnu bc bison flex libssl-dev libelf-dev \
  u-boot-tools device-tree-compiler wget ca-certificates make file xz-utils >/dev/null

echo "==> fetching + configuring kernel ${KVER}..."
mkdir -p /build
if [ ! -d "$KSRC" ]; then
  wget -q -O /build/linux-${KVER}.tar.xz \
    "https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-${KVER}.tar.xz"
  tar -xf /build/linux-${KVER}.tar.xz -C /build/
  rm /build/linux-${KVER}.tar.xz
fi

# DT (build-all.sh adds the board dts); harmless if already present.
cp /src/kernel/dts/as5610-52x.dts "$KSRC/arch/powerpc/boot/dts/" 2>/dev/null || true
for p in /src/kernel/patches/*.patch; do
  [ -f "$p" ] || continue
  (cd "$KSRC" && patch -p1 -N < "$p") || true
done

cp /src/config/kernel/as5610_defconfig "$KSRC/.config"
make -C "$KSRC" ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- olddefconfig

echo "==> building kernel (vmlinux + modules -> Module.symvers)... [slow]"
make -C "$KSRC" ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- -j"$(nproc)" uImage modules

echo "==> building out-of-tree platform modules..."
MODSTAGE=/build/modules-src
rm -rf "$MODSTAGE"; mkdir -p "$MODSTAGE"
for m in cpld bde tmon retimer; do
  case $m in
    cpld)    cp -a /src/platform/cpld    "$MODSTAGE/cpld" ;;
    retimer) cp -a /src/platform/retimer "$MODSTAGE/retimer" ;;
    bde)     cp -a /src/asic/bde         "$MODSTAGE/bde" ;;
    tmon)    cp -a /src/asic/tmon        "$MODSTAGE/tmon" ;;
  esac
  echo "   -> $m"
  make -C "$KSRC" ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- \
       M="$MODSTAGE/$m" modules
done

echo "==> staging .ko + verifying CPLD vermagic..."
find "$MODSTAGE" -name "*.ko" -exec cp -v {} /build/output/modules/ \;
modinfo /build/output/modules/accton_as5610_52x_cpld.ko | grep -iE "vermagic|filename"
'
echo "==> modules in output/modules:"
ls -l "$TOP/output/modules/"
