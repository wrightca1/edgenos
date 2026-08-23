#!/bin/bash
# build-vm-image.sh — build the EdgeNOS x86_64 virtual switch end to end:
#
#   1. base     build/build-base-x86_64.sh      (Buildroot: kernel/rootfs/initrd/GRUB + base .epk)
#   2. quagga   build/build-quagga-x86_64.sh    (static zebra/ospfd/ospf6d/bgpd/vtysh)
#   3. packages platform-svc, quagga, edgenos-cli (.epk)
#   4. image    bin/edgenos build x86_64-kvm_x86_64-r0  -> ONIE installer (.bin) + disk (.qcow2)
#
# Steps 1-2 are skipped when their outputs exist (pass --rebuild-base / --rebuild-quagga).
# Reproducible: SOURCE_DATE_EPOCH defaults to the HEAD commit time.
set -euo pipefail
TOP=$(cd "$(dirname "$0")/.." && pwd)
cd "$TOP"
PLATFORM=${PLATFORM:-x86_64-kvm_x86_64-r0}
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$TOP" log -1 --format=%ct 2>/dev/null || echo 0)}
REBUILD_BASE=; REBUILD_QUAGGA=
for a in "$@"; do case "$a" in --rebuild-base) REBUILD_BASE=1;; --rebuild-quagga) REBUILD_QUAGGA=1;; esac; done

step() { echo; echo "################ $*"; }

step "1/4 base (Buildroot)"
if [ -n "$REBUILD_BASE" ] || [ ! -f output/base-x86_64/rootfs.sqsh ] || ! ls output/packages/base_*_x86_64-*.epk >/dev/null 2>&1; then
    build/build-base-x86_64.sh
else
    echo "base present: output/base-x86_64 (use --rebuild-base to rebuild)"
fi

step "2/4 quagga"
if [ -n "$REBUILD_QUAGGA" ] || [ ! -x output/quagga-x86_64/zebra ]; then
    build/build-quagga-x86_64.sh
else
    echo "quagga present: output/quagga-x86_64 (use --rebuild-quagga to rebuild)"
fi

step "3/4 component packages"
echo "--- edged-vswitch (asic_ops software datapath), static, Buildroot toolchain"
make -s -C platform/qemu-kvm-x86_64 clean >/dev/null
make -s -C platform/qemu-kvm-x86_64 CROSS_COMPILE="$TOP/output/br-x86_64/host/bin/x86_64-buildroot-linux-gnu-"
bin/edgenos pkg build packaging/specs/qemu-kvm-x86_64/edged-vswitch.yml --source-root . --platform "$PLATFORM"
bin/edgenos pkg build packaging/specs/qemu-kvm-x86_64/platform-svc.yml --source-root . --platform "$PLATFORM"
bin/edgenos pkg build packaging/specs/qemu-kvm-x86_64/quagga.yml       --source-root . --arch x86_64 --asic any
# edgenos-cli's spec uses the parent-dir convention (paths start with edgenos/): give it one.
mkdir -p output/srcroot && ln -sfn "$TOP" output/srcroot/edgenos
bin/edgenos pkg build packaging/specs/edgenos-cli.yml --source-root output/srcroot --arch any --asic any

step "4/4 image: ONIE installer + qcow2"
bin/edgenos build "$PLATFORM" --source-root .
echo
ls -l output/images/ | grep -E "EdgeNOS-.*(bin|qcow2)$" || true
