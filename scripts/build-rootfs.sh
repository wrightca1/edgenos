#!/bin/bash
# build-rootfs.sh - Build root filesystem using Buildroot
set -e

TOPDIR="$(cd "$(dirname "$0")/.." && pwd)"
BRVER="2023.02.9"
BRSRC="$TOPDIR/build/buildroot-$BRVER"
OUTDIR="$TOPDIR/output"
JOBS=$(nproc)

download() {
    if [ -d "$BRSRC" ]; then
        echo "Buildroot source already present at $BRSRC"
        return
    fi

    mkdir -p "$TOPDIR/build"
    local URL="https://buildroot.org/downloads/buildroot-${BRVER}.tar.xz"
    local TARBALL="$TOPDIR/build/buildroot-${BRVER}.tar.xz"

    if [ ! -f "$TARBALL" ]; then
        echo "==> Downloading Buildroot $BRVER..."
        wget -q --show-progress -O "$TARBALL" "$URL"
    fi

    echo "==> Extracting Buildroot..."
    tar -xf "$TARBALL" -C "$TOPDIR/build/"
}

build() {
    if [ ! -d "$BRSRC" ]; then
        echo "ERROR: Buildroot source not found. Run '$0 download' first."
        exit 1
    fi

    # Copy our defconfig
    cp "$TOPDIR/config/rootfs/buildroot_defconfig" "$BRSRC/configs/edgenos_defconfig"

    echo "==> Configuring Buildroot..."
    make -C "$BRSRC" edgenos_defconfig

    echo "==> Building rootfs (this takes a while)..."
    make -C "$BRSRC" -j$JOBS

    mkdir -p "$OUTDIR/rootfs"
    cp "$BRSRC/output/images/rootfs.squashfs" "$OUTDIR/rootfs/" 2>/dev/null || true
    cp "$BRSRC/output/images/rootfs.tar" "$OUTDIR/rootfs/" 2>/dev/null || true

    echo "==> Base rootfs build complete"
}

assemble() {
    echo "==> Assembling final rootfs..."

    local STAGING="$OUTDIR/rootfs/staging"
    rm -rf "$STAGING"
    mkdir -p "$STAGING"

    # Extract base rootfs
    if [ -f "$OUTDIR/rootfs/rootfs.tar" ]; then
        tar -xf "$OUTDIR/rootfs/rootfs.tar" -C "$STAGING"
    else
        echo "ERROR: Base rootfs not found. Run '$0 build' first."
        exit 1
    fi

    # Install kernel modules
    if [ -d "$OUTDIR/kernel/modules" ]; then
        echo "  Installing kernel modules..."
        cp -a "$OUTDIR/kernel/modules/lib/modules" "$STAGING/lib/"
    fi

    # Install platform kernel modules
    mkdir -p "$STAGING/lib/modules/extra"
    find "$TOPDIR/platform" -name "*.ko" -exec cp {} "$STAGING/lib/modules/extra/" \;
    find "$TOPDIR/asic/bde" -name "*.ko" -exec cp {} "$STAGING/lib/modules/extra/" \;

    # Install switchd
    if [ -f "$TOPDIR/asic/switchd/switchd" ]; then
        echo "  Installing switchd..."
        install -D -m 755 "$TOPDIR/asic/switchd/switchd" "$STAGING/usr/sbin/switchd"
    fi

    # Install OpenMDK shared libs
    find "$TOPDIR/output/sdk" -name "*.so*" -exec cp {} "$STAGING/usr/lib/" \; 2>/dev/null || true

    # Install ASIC config files
    mkdir -p "$STAGING/etc/switchd"
    cp "$TOPDIR/config/bcm/"* "$STAGING/etc/switchd/" 2>/dev/null || true

    # Apply rootfs overlay
    if [ -d "$TOPDIR/config/rootfs/overlay" ]; then
        echo "  Applying rootfs overlay..."
        cp -a "$TOPDIR/config/rootfs/overlay/"* "$STAGING/"
    fi

    # Create final squashfs
    echo "  Creating squashfs image..."
    mkdir -p "$OUTDIR/images"
    mksquashfs "$STAGING" "$OUTDIR/images/rootfs.sqsh" \
        -comp xz -noappend -all-root

    echo "==> Final rootfs assembled: $OUTDIR/images/rootfs.sqsh"
    du -sh "$OUTDIR/images/rootfs.sqsh"
}

case "${1:-}" in
    download) download ;;
    build)    build ;;
    assemble) assemble ;;
    *)
        echo "Usage: $0 {download|build|assemble}"
        exit 1
        ;;
esac
