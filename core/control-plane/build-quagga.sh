#!/bin/bash
# build-quagga.sh — cross-compile static Quagga (zebra + ospfd) for the PPC
# AS5610-52X and drop the binaries at output/{zebra,ospfd}-ppc, which
# assemble-rootfs-from-base.sh installs into the image as the OSPF control plane.
#
# WHY this exists: output/ is gitignored, so the prebuilt zebra-ppc/ospfd-ppc are
# NOT in version control. Without this script a fresh checkout can't rebuild the
# image with OSPF. Run this once after a clean checkout (or whenever output/ is
# wiped) before scripts/assemble-rootfs-from-base.sh.
#
# Quagga 1.2.4 (not FRR): Cumulus 2.5 shipped Quagga, and modern FRR's
# libyang/cmake make a static cross-build painful. Runs inside edgenos-builder.
#
# edged mirrors the kernel FIB to the chip (RTM_NEWROUTE -> l3_route_add_paths,
# incl. ECMP), so ANY daemon that installs to the kernel gets HW-programmed for
# free — OSPF needs only this daemon + the control-traffic CPU punt (already in
# edged: FP 224/8 trap + CPU_CONTROL_1 TTL1 traps + MC copy-replication regs).
set -e

QUAGGA_VER=1.2.4
QUAGGA_URL="https://download.savannah.gnu.org/releases/quagga/quagga-${QUAGGA_VER}.tar.gz"
IMG=edgenos-builder
export DOCKER_HOST=${DOCKER_HOST:-unix:///run/user/1000/docker.sock}
TOP=$(cd "$(dirname "$0")/.." && pwd)

mkdir -p "$TOP/output"

docker run --rm --network=host -v "$TOP:/build/src" --entrypoint /bin/bash "$IMG" -c '
set -e
VER='"$QUAGGA_VER"'
URL="'"$QUAGGA_URL"'"

# configure needs GNU awk (not mawk); container is ephemeral so install each run.
apt-get update -qq && apt-get install -y -qq gawk wget >/dev/null

cd /tmp
if [ -f /build/src/output/quagga-${VER}.tar.gz ]; then
    cp /build/src/output/quagga-${VER}.tar.gz .
else
    wget -q "$URL" -O quagga-${VER}.tar.gz
    cp quagga-${VER}.tar.gz /build/src/output/   # cache the tarball for offline rebuilds
fi
tar xf quagga-${VER}.tar.gz
cd quagga-${VER}

# No static libcrypt for the cross target, and crypt() is locally declared inside
# these functions so it cannot be defined-in-place. Both call sites are VTY
# password hashing (unused here) — stub them out.
sed -i "s|strcmp (crypt(buf, passwd), passwd)|strcmp (buf, passwd)|" lib/vty.c
sed -i "s|return crypt (passwd, salt);|(void)salt; return (char *)passwd;|" lib/command.c

# -fcommon is ESSENTIAL: GCC10+ -fno-common -> "multiple definition of __packed"
# in prefix.h. ac_cv_lib_readline_main=no avoids the readline link probe.
./configure \
    --host=powerpc-linux-gnu --build=x86_64-pc-linux-gnu CC=powerpc-linux-gnu-gcc \
    --disable-vtysh --disable-doc \
    --disable-bgpd --disable-ripd --disable-ripngd --disable-ospf6d \
    --disable-isisd --disable-pimd --disable-nhrpd \
    --enable-user=root --enable-group=root --enable-vty-group=root \
    --enable-static --disable-shared \
    LDFLAGS="-static" CFLAGS="-O2 -fcommon" ac_cv_lib_readline_main=no

make -j"$(nproc)"

powerpc-linux-gnu-strip zebra/zebra -o /build/src/output/zebra-ppc
powerpc-linux-gnu-strip ospfd/ospfd -o /build/src/output/ospfd-ppc
echo "==> built:"
ls -l /build/src/output/zebra-ppc /build/src/output/ospfd-ppc
'

echo "==> Quagga binaries ready at output/{zebra,ospfd}-ppc"
echo "    Next: scripts/assemble-rootfs-from-base.sh to bake them into rootfs.sqsh"
