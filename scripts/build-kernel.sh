#!/bin/bash
# build-kernel.sh - Download and build Linux kernel for AS5610-52X
set -e

TOPDIR="$(cd "$(dirname "$0")/.." && pwd)"
KVER="5.10.224"
KSRC="$TOPDIR/build/linux-$KVER"
CROSS="powerpc-linux-gnu-"
ARCH="powerpc"
OUTDIR="$TOPDIR/output"
JOBS=$(nproc)

download() {
    if [ -d "$KSRC" ]; then
        echo "Kernel source already present at $KSRC"
        return
    fi

    mkdir -p "$TOPDIR/build"
    local MAJOR=$(echo "$KVER" | cut -d. -f1)
    local URL="https://cdn.kernel.org/pub/linux/kernel/v${MAJOR}.x/linux-${KVER}.tar.xz"
    local TARBALL="$TOPDIR/build/linux-${KVER}.tar.xz"

    if [ ! -f "$TARBALL" ]; then
        echo "==> Downloading Linux $KVER..."
        wget -q --show-progress -O "$TARBALL" "$URL"
    fi

    echo "==> Extracting kernel source..."
    tar -xf "$TARBALL" -C "$TOPDIR/build/"

    # Apply patches
    if [ -d "$TOPDIR/kernel/patches" ]; then
        echo "==> Applying kernel patches..."
        for p in "$TOPDIR/kernel/patches"/*.patch; do
            [ -f "$p" ] || continue
            echo "  Applying $(basename "$p")..."
            (cd "$KSRC" && patch -p1 < "$p")
        done
    fi

    # Copy our device tree into the kernel tree
    if [ -f "$TOPDIR/kernel/dts/as5610-52x.dts" ]; then
        echo "==> Installing AS5610-52X device tree..."
        cp "$TOPDIR/kernel/dts/as5610-52x.dts" \
           "$KSRC/arch/powerpc/boot/dts/as5610-52x.dts"
        # Add to Makefile if not present
        local DTS_MAKE="$KSRC/arch/powerpc/boot/dts/Makefile"
        if ! grep -q "as5610-52x" "$DTS_MAKE" 2>/dev/null; then
            echo 'dtb-$(CONFIG_PPC_85xx) += as5610-52x.dtb' >> "$DTS_MAKE"
        fi
    fi
}

build() {
    if [ ! -d "$KSRC" ]; then
        echo "ERROR: Kernel source not found. Run '$0 download' first."
        exit 1
    fi

    # Copy defconfig if .config doesn't exist
    if [ ! -f "$KSRC/.config" ]; then
        cp "$TOPDIR/config/kernel/as5610_defconfig" "$KSRC/.config"
        make -C "$KSRC" ARCH=$ARCH CROSS_COMPILE=$CROSS olddefconfig
    fi

    echo "==> Building kernel (${JOBS} jobs)..."
    make -C "$KSRC" ARCH=$ARCH CROSS_COMPILE=$CROSS -j$JOBS \
        uImage dtbs modules

    # Collect outputs
    mkdir -p "$OUTDIR/kernel"
    cp "$KSRC/arch/powerpc/boot/uImage" "$OUTDIR/kernel/"
    cp "$KSRC/arch/powerpc/boot/dts/as5610-52x.dtb" "$OUTDIR/kernel/" 2>/dev/null || true
    cp "$KSRC/vmlinux" "$OUTDIR/kernel/"

    # Install modules to staging dir
    mkdir -p "$OUTDIR/kernel/modules"
    make -C "$KSRC" ARCH=$ARCH CROSS_COMPILE=$CROSS \
        INSTALL_MOD_PATH="$OUTDIR/kernel/modules" modules_install

    echo "==> Kernel build complete"
    echo "  uImage: $OUTDIR/kernel/uImage"
    echo "  DTB:    $OUTDIR/kernel/as5610-52x.dtb"
}

case "${1:-}" in
    download) download ;;
    build)    build ;;
    *)
        echo "Usage: $0 {download|build}"
        exit 1
        ;;
esac
