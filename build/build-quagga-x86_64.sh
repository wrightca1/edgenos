#!/bin/bash
# build-quagga-x86_64.sh — static Quagga 1.2.4 (zebra + ospfd + ospf6d + bgpd + vtysh)
# for x86_64, built with the Buildroot toolchain that build-base-x86_64.sh produced, so the
# binaries match the base's glibc/kernel headers exactly (and are static anyway).
#
# Needs GNU awk (gawk) on the host. Same recipe as core/control-plane/build-quagga.sh: Quagga 1.2.4
# not FRR, -fcommon, crypt() stubbed, static. Differences: native-ish (the Buildroot
# x86_64 cross toolchain, no docker), bgpd + vtysh enabled (a lab switch wants BGP and an
# interactive shell), multipath 64. Drops the binaries at output/quagga-x86_64/ for
# packaging/specs/qemu-kvm-x86_64/quagga.yml.
#
# Env: O (Buildroot output dir, default output/br-x86_64)  JOBS (default nproc)
set -euo pipefail
TOP=$(cd "$(dirname "$0")/.." && pwd)
O=${O:-$TOP/output/br-x86_64}
JOBS=${JOBS:-$(nproc)}
QUAGGA_VER=1.2.4
QUAGGA_URL="https://github.com/Quagga/quagga/releases/download/quagga-${QUAGGA_VER}/quagga-${QUAGGA_VER}.tar.gz"
HOSTDIR=$O/host
CROSS=$HOSTDIR/bin/x86_64-buildroot-linux-gnu-
SYSROOT=$HOSTDIR/x86_64-buildroot-linux-gnu/sysroot
OUT=$TOP/output/quagga-x86_64
WORK=$TOP/output/quagga-x86_64-build

[ -x "${CROSS}gcc" ] || { echo "error: Buildroot toolchain not found at ${CROSS}gcc — run build/build-base-x86_64.sh first"; exit 1; }
mkdir -p "$TOP/output" "$OUT" "$WORK"

tarball=$TOP/output/quagga-${QUAGGA_VER}.tar.gz
if [ ! -f "$tarball" ]; then
    echo "==> downloading quagga ${QUAGGA_VER}"
    wget -q "$QUAGGA_URL" -O "$tarball"
fi
rm -rf "$WORK/quagga-${QUAGGA_VER}"
tar xf "$tarball" -C "$WORK"
cd "$WORK/quagga-${QUAGGA_VER}"

# crypt(): stub both VTY password-hash call sites (same as the PPC build)
sed -i "s|strcmp (crypt(buf, passwd), passwd)|strcmp (buf, passwd)|" lib/vty.c
sed -i "s|return crypt (passwd, salt);|(void)salt; return (char *)passwd;|" lib/command.c

# gcc 14 makes implicit decls / pointer mismatches hard errors; 2018 code needs them back as warnings.
CFLAGS="-O2 -fcommon -Wno-implicit-function-declaration -Wno-incompatible-pointer-types -Wno-int-conversion -Wno-implicit-int -Wno-error"

echo "==> configure"
./configure \
    --host=x86_64-buildroot-linux-gnu --build=x86_64-pc-linux-gnu \
    CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" STRIP="${CROSS}strip" \
    --prefix=/usr --sysconfdir=/etc/quagga --localstatedir=/run/quagga \
    --enable-vtysh --disable-doc \
    --disable-ripd --disable-ripngd --disable-isisd --disable-pimd --disable-nhrpd \
    --enable-user=root --enable-group=root --enable-vty-group=root \
    --enable-multipath=64 \
    --enable-static --disable-shared \
    LDFLAGS="-static" CFLAGS="$CFLAGS" LIBS="-lncurses" \
    ac_cv_lib_readline_main=yes > "$WORK/configure.log" 2>&1 || { tail -40 "$WORK/configure.log"; exit 1; }

echo "==> make -j$JOBS (libtool -all-static: fully static daemons)"
# libtool turns a plain -static into "prefer .la over .so"; -all-static is what makes the
# final binaries static. Passed at make time so configure's link tests still work.
make -j"$JOBS" LDFLAGS="-all-static" > "$WORK/make.log" 2>&1 || { grep -E "error|Error" "$WORK/make.log" | tail -40; exit 1; }

for d in zebra ospfd ospf6d bgpd; do
    "${CROSS}strip" "$d/$d" -o "$OUT/$d"
done
"${CROSS}strip" vtysh/vtysh -o "$OUT/vtysh"
file "$OUT"/* | sed 's|.*/||'
echo "==> built: $(ls "$OUT" | tr '\n' ' ')  ->  $OUT"
