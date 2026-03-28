# EdgeNOS Build Environment
# Cross-compiles for PowerPC e500v2 (Freescale P2020) and produces ONIE image
#
# Usage:
#   docker build -t edgenos-builder .
#   docker run --rm -v $(pwd)/output:/build/output edgenos-builder
#   docker run --rm -it -v $(pwd)/output:/build/output edgenos-builder bash

FROM ubuntu:22.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG KVER=5.10.224
ARG BRVER=2023.02.9
ARG JOBS=0

LABEL maintainer="EdgeNOS Contributors"
LABEL description="Cross-compilation environment for EdgeNOS (AS5610-52X)"

# ── Install build dependencies ────────────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
    # Cross-compiler
    gcc-powerpc-linux-gnu \
    g++-powerpc-linux-gnu \
    binutils-powerpc-linux-gnu \
    # Kernel build
    bc \
    bison \
    flex \
    libssl-dev \
    libelf-dev \
    # Device tree
    device-tree-compiler \
    # U-Boot tools (FIT image)
    u-boot-tools \
    # Filesystem tools
    squashfs-tools \
    cpio \
    # General build tools
    build-essential \
    make \
    ccache \
    wget \
    curl \
    rsync \
    perl \
    python3 \
    python-is-python3 \
    # Buildroot dependencies
    file \
    unzip \
    patch \
    gzip \
    bzip2 \
    xz-utils \
    libncurses-dev \
    # QEMU for testing PPC binaries
    qemu-user-static \
    # Partition tools (for installer testing)
    fdisk \
    parted \
    dosfstools \
    e2fsprogs \
    # Git (for buildroot patches)
    git \
    # Misc
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# ── Set up build user (avoid building as root) ────────────────
RUN useradd -m -s /bin/bash builder
ENV HOME=/home/builder

# ── Set cross-compile environment ─────────────────────────────
ENV CROSS_COMPILE=powerpc-linux-gnu-
ENV ARCH=powerpc
ENV PATH="/usr/lib/ccache:/usr/bin:${PATH}"
ENV CCACHE_DIR=/build/.ccache
ENV CCACHE_MAXSIZE=2G

# ── Copy project source ──────────────────────────────────────
WORKDIR /build
COPY --chown=builder:builder . /build/

# ── Verify cross-compiler ────────────────────────────────────
RUN powerpc-linux-gnu-gcc --version | head -1 && \
    echo 'int main(){return 0;}' | powerpc-linux-gnu-gcc -x c - -o /tmp/test-ppc -static && \
    file /tmp/test-ppc | grep -q "PowerPC" && \
    echo "Cross-compiler OK" && \
    rm /tmp/test-ppc

# ── Download kernel source ───────────────────────────────────
RUN mkdir -p /build/build && \
    echo "==> Downloading Linux ${KVER}..." && \
    wget -q --show-progress -O /build/build/linux-${KVER}.tar.xz \
        "https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-${KVER}.tar.xz" && \
    echo "==> Extracting..." && \
    tar -xf /build/build/linux-${KVER}.tar.xz -C /build/build/ && \
    rm /build/build/linux-${KVER}.tar.xz

# ── Apply kernel patches and install DTS ──────────────────────
RUN set -e; \
    for p in /build/kernel/patches/*.patch; do \
        [ -f "$p" ] || continue; \
        echo "Applying $(basename $p)"; \
        (cd /build/build/linux-${KVER} && patch -p1 < "$p"); \
    done; \
    if [ -f /build/kernel/dts/as5610-52x.dts ]; then \
        cp /build/kernel/dts/as5610-52x.dts \
           /build/build/linux-${KVER}/arch/powerpc/boot/dts/as5610-52x.dts; \
        echo 'dtb-$(CONFIG_PPC_85xx) += as5610-52x.dtb' >> \
           /build/build/linux-${KVER}/arch/powerpc/boot/dts/Makefile; \
    fi

# ── Download Buildroot ───────────────────────────────────────
RUN echo "==> Downloading Buildroot ${BRVER}..." && \
    wget -q --show-progress -O /build/build/buildroot-${BRVER}.tar.xz \
        "https://buildroot.org/downloads/buildroot-${BRVER}.tar.xz" && \
    echo "==> Extracting..." && \
    tar -xf /build/build/buildroot-${BRVER}.tar.xz -C /build/build/ && \
    rm /build/build/buildroot-${BRVER}.tar.xz

# ── Compute job count if not set ──────────────────────────────
RUN if [ "${JOBS}" = "0" ]; then JOBS=$(nproc); fi && \
    echo "JOBS=${JOBS}" > /build/.build-env

# ── Build entry point ────────────────────────────────────────
COPY <<'ENTRYPOINT_SCRIPT' /build/docker-build.sh
#!/bin/bash
set -e
source /build/.build-env
JOBS=${JOBS:-$(nproc)}
KVER=5.10.224
BRVER=2023.02.9
KSRC=/build/build/linux-${KVER}
BRSRC=/build/build/buildroot-${BRVER}
OUTDIR=/build/output

echo "============================================"
echo "  EdgeNOS Build (${JOBS} jobs)"
echo "  Target: powerpc-linux-gnu (e500v2)"
echo "============================================"

build_kernel() {
    echo ""
    echo "==> [1/6] Building kernel..."
    cp /build/config/kernel/as5610_defconfig "${KSRC}/.config"
    make -C "${KSRC}" ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- olddefconfig
    make -C "${KSRC}" ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- -j${JOBS} \
        uImage dtbs modules

    mkdir -p "${OUTDIR}/kernel"
    cp "${KSRC}/arch/powerpc/boot/uImage" "${OUTDIR}/kernel/"
    cp "${KSRC}/arch/powerpc/boot/dts/as5610-52x.dtb" "${OUTDIR}/kernel/" 2>/dev/null || true

    # Install kernel modules to staging
    mkdir -p "${OUTDIR}/kernel/modules"
    make -C "${KSRC}" ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- \
        INSTALL_MOD_PATH="${OUTDIR}/kernel/modules" modules_install

    echo "  uImage: ${OUTDIR}/kernel/uImage"
    echo "  DTB:    ${OUTDIR}/kernel/as5610-52x.dtb"
}

build_platform_modules() {
    echo ""
    echo "==> [2/6] Building platform kernel modules..."
    for mod in platform/cpld platform/retimer; do
        echo "  Building ${mod}..."
        make -C "${KSRC}" M=/build/${mod} \
            ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- modules || \
            echo "  WARNING: ${mod} build failed (may need kernel headers)"
    done

    echo "  Building BDE modules..."
    make -C "${KSRC}" M=/build/asic/bde \
        ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- modules || \
        echo "  WARNING: BDE build failed"
}

build_sdk() {
    echo ""
    echo "==> [3/6] Building OpenMDK + mdk-init..."
    local SDK_BLDDIR="${OUTDIR}/sdk"
    mkdir -p "${SDK_BLDDIR}"

    # Build OpenMDK libraries + mdk-init using the mdk-init Makefile.
    # This follows the official linux-user example pattern:
    # - Runs instpkgs.pl to generate chip-specific headers
    # - Cross-compiles CDK, BMD, PHY, LIBBDE as static libraries
    # - Links mdk-init tool
    make -C /build/asic/mdk-init \
        CROSS_COMPILE=powerpc-linux-gnu- \
        MDK=/build/asic/openmdk \
        BLDDIR="${SDK_BLDDIR}" \
        -j${JOBS} || {
        echo "  WARNING: mdk-init link failed (DMA stubs) — skipping test tool"
        echo "  SDK libraries were built successfully:"
    }

    echo "  SDK libraries:"
    find "${SDK_BLDDIR}" -name "*.a" -exec ls -lh {} \;
    if [ -f "${SDK_BLDDIR}/mdk-init" ]; then
        echo "  mdk-init:"
        ls -lh "${SDK_BLDDIR}/mdk-init"
        file "${SDK_BLDDIR}/mdk-init"
    else
        echo "  mdk-init: not built (non-fatal)"
    fi
}

build_switchd() {
    echo ""
    echo "==> [4/6] Building switchd..."
    local SDK_BLDDIR="${OUTDIR}/sdk"

    make -C /build/asic/switchd \
        CROSS_COMPILE=powerpc-linux-gnu- \
        TOPDIR=/build \
        OPENMDK=/build/asic/openmdk \
        SDK_BLDDIR="${SDK_BLDDIR}" \
        -j${JOBS}

    echo "  switchd:"
    ls -lh /build/asic/switchd/switchd
    file /build/asic/switchd/switchd
}

build_rootfs() {
    echo ""
    echo "==> [5/6] Building rootfs..."

    # Configure buildroot
    cp /build/config/rootfs/buildroot_defconfig "${BRSRC}/configs/edgenos_defconfig"
    make -C "${BRSRC}" edgenos_defconfig

    # Build base rootfs
    make -C "${BRSRC}" -j${JOBS}

    mkdir -p "${OUTDIR}/rootfs"
    cp "${BRSRC}/output/images/rootfs.tar" "${OUTDIR}/rootfs/" 2>/dev/null || true

    # Assemble final rootfs
    echo "  Assembling final rootfs..."
    local STAGING="${OUTDIR}/rootfs/staging"
    rm -rf "${STAGING}"
    mkdir -p "${STAGING}"

    # Extract base rootfs
    if [ -f "${OUTDIR}/rootfs/rootfs.tar" ]; then
        tar -xf "${OUTDIR}/rootfs/rootfs.tar" -C "${STAGING}"
    fi

    # Install kernel modules
    if [ -d "${OUTDIR}/kernel/modules" ]; then
        cp -a "${OUTDIR}/kernel/modules/lib/modules" "${STAGING}/lib/" 2>/dev/null || true
    fi

    # Install platform kernel modules
    mkdir -p "${STAGING}/lib/modules/extra"
    find /build/platform -name "*.ko" -exec cp {} "${STAGING}/lib/modules/extra/" \; 2>/dev/null || true
    find /build/asic/bde -name "*.ko" -exec cp {} "${STAGING}/lib/modules/extra/" \; 2>/dev/null || true

    # Install switchd
    if [ -f /build/asic/switchd/switchd ]; then
        install -D -m 755 /build/asic/switchd/switchd "${STAGING}/usr/sbin/switchd"
    fi

    # Install mdk-init (optional test tool, may not be built)
    if [ -f "${OUTDIR}/sdk/mdk-init" ]; then
        install -D -m 755 "${OUTDIR}/sdk/mdk-init" "${STAGING}/usr/sbin/mdk-init"
        echo "  mdk-init installed"
    fi

    # Install ASIC config
    mkdir -p "${STAGING}/etc/switchd"
    cp /build/config/bcm/* "${STAGING}/etc/switchd/" 2>/dev/null || true

    # Apply rootfs overlay
    if [ -d /build/config/rootfs/overlay ]; then
        cp -a /build/config/rootfs/overlay/* "${STAGING}/"
    fi

    # Set root password to 'as5610'
    HASH='$6$52x8izoNf.9aB3Vd$azJoPieNNwYutepMslp9J.32/wB0pGCdd5lxeiz9J8jhoBdqwllvIvNIvyGYnCWfYuVZ4LBP9970NCzaymfsI/'
    if [ -f "${STAGING}/etc/shadow" ]; then
        sed -i "s|^root:[^:]*:|root:${HASH}:|" "${STAGING}/etc/shadow"
        echo "  Root password set"
    fi

    # Allow root SSH login
    if [ -f "${STAGING}/etc/ssh/sshd_config" ]; then
        sed -i 's/^#PermitRootLogin.*/PermitRootLogin yes/' "${STAGING}/etc/ssh/sshd_config"
        grep -q "^PermitRootLogin" "${STAGING}/etc/ssh/sshd_config" || \
            echo "PermitRootLogin yes" >> "${STAGING}/etc/ssh/sshd_config"
    fi

    # Create squashfs
    mkdir -p "${OUTDIR}/images"
    mksquashfs "${STAGING}" "${OUTDIR}/images/rootfs.sqsh" \
        -comp xz -noappend -all-root

    echo "  rootfs: ${OUTDIR}/images/rootfs.sqsh"
    du -sh "${OUTDIR}/images/rootfs.sqsh"
}

build_image() {
    echo ""
    echo "==> [6/6] Building ONIE installer..."

    local UIMAGE="${OUTDIR}/kernel/uImage"
    local DTB="${OUTDIR}/kernel/as5610-52x.dtb"
    local ROOTFS="${OUTDIR}/images/rootfs.sqsh"

    for f in "${UIMAGE}" "${DTB}" "${ROOTFS}"; do
        if [ ! -f "$f" ]; then
            echo "  ERROR: Missing $f"
            return 1
        fi
    done

    # Extract raw gzip kernel from uImage (strip legacy U-Boot header)
    # FIT needs raw kernel payload, not a uImage wrapper inside a FIT
    echo "  Extracting raw kernel from uImage..."
    local FITDIR="${OUTDIR}/images/fitbuild"
    mkdir -p "${FITDIR}"
    dumpimage -T kernel -p 0 -o "${FITDIR}/kernel.gz" "${UIMAGE}" || {
        echo "  WARN: dumpimage failed, using dd fallback (skip 64-byte uImage header)"
        dd if="${UIMAGE}" of="${FITDIR}/kernel.gz" bs=64 skip=1 2>/dev/null
    }
    cp "${DTB}" "${FITDIR}/as5610_52x.dtb"

    # Build real initramfs with nos-init (handles squashfs + overlayfs mount)
    echo "  Building initramfs..."
    local IRDIR=$(mktemp -d)
    powerpc-linux-gnu-gcc -static -nostdlib -nostartfiles -nodefaultlibs \
        -Os -Wall -o "${IRDIR}/init" /build/initramfs/nos-init.c 2>/dev/null
    if [ -f "${IRDIR}/init" ]; then
        echo "  nos-init compiled OK"
    else
        echo "  nos-init compile failed, using shell initramfs"
        mkdir -p "${IRDIR}/bin" "${IRDIR}/dev" "${IRDIR}/proc" "${IRDIR}/sys" \
                 "${IRDIR}/lower" "${IRDIR}/rw" "${IRDIR}/newroot"
        cp "${BRSRC}/output/target/bin/busybox" "${IRDIR}/bin/" 2>/dev/null || \
            cp /usr/powerpc-linux-gnu/bin/busybox "${IRDIR}/bin/" 2>/dev/null || true
        cat > "${IRDIR}/init" << 'SHINIT'
#!/bin/busybox sh
export PATH=/bin
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev
/bin/busybox mkdir -p /lower /rw /newroot
echo "nos-init: waiting for USB storage..."
i=0
while [ ! -b /dev/sda6 ] && [ $i -lt 30 ]; do
    /bin/busybox sleep 1
    i=$((i+1))
done
if [ ! -b /dev/sda6 ]; then
    echo "nos-init: ERROR /dev/sda6 not found"
    exec /bin/busybox sh
fi
echo "nos-init: mounting squashfs..."
/bin/busybox mount -t squashfs -o ro /dev/sda6 /lower || {
    echo "nos-init: squashfs mount failed"; exec /bin/busybox sh; }
echo "nos-init: mounting overlay..."
/bin/busybox mount -t ext2 /dev/sda3 /rw 2>/dev/null || {
    echo "nos-init: formatting sda3..."
    /lower/sbin/mke2fs -L NOS-RW /dev/sda3 2>/dev/null
    /bin/busybox mount -t ext2 /dev/sda3 /rw || {
        echo "nos-init: overlay mount failed, booting read-only"
        /bin/busybox mount --move /proc /lower/proc 2>/dev/null
        /bin/busybox mount --move /sys /lower/sys 2>/dev/null
        /bin/busybox mount --move /dev /lower/dev 2>/dev/null
        exec /bin/busybox switch_root /lower /sbin/init
    }
}
/bin/busybox mkdir -p /rw/upper /rw/work
/bin/busybox mount -t overlay overlay \
    -o lowerdir=/lower,upperdir=/rw/upper,workdir=/rw/work /newroot || {
    echo "nos-init: overlay failed, booting read-only"
    /bin/busybox mount --move /proc /lower/proc 2>/dev/null
    /bin/busybox mount --move /sys /lower/sys 2>/dev/null
    /bin/busybox mount --move /dev /lower/dev 2>/dev/null
    exec /bin/busybox switch_root /lower /sbin/init
}
echo "nos-init: switching root..."
/bin/busybox mount --move /proc /newroot/proc
/bin/busybox mount --move /sys /newroot/sys
/bin/busybox mount --move /dev /newroot/dev
exec /bin/busybox switch_root /newroot /sbin/init
SHINIT
        chmod +x "${IRDIR}/init"
    fi
    (cd "${IRDIR}" && find . | cpio -o -H newc 2>/dev/null | gzip -9 > "${FITDIR}/initramfs.cpio.gz")
    rm -rf "${IRDIR}"
    echo "  initramfs: $(du -h "${FITDIR}/initramfs.cpio.gz" | awk '{print $1}')"

    # Generate ITS matching open-nos-as5610 reference format
    cat > "${FITDIR}/nos.its" << 'ITSEOF'
/dts-v1/;

/ {
    description = "EdgeNOS for AS5610-52X";
    #address-cells = <1>;

    images {
        kernel {
            description = "PowerPC Kernel";
            data = /incbin/("kernel.gz");
            type = "kernel";
            arch = "powerpc";
            os = "linux";
            compression = "gzip";
            load = <0x00000000>;
            entry = <0x00000000>;
            hash { algo = "crc32"; };
        };
        accton_as5610_52x_dtb {
            description = "AS5610-52X device tree";
            data = /incbin/("as5610_52x.dtb");
            type = "flat_dt";
            arch = "powerpc";
            compression = "none";
            load = <0x00f00000>;
            hash { algo = "crc32"; };
        };
        initramfs {
            description = "initramfs stub";
            data = /incbin/("initramfs.cpio.gz");
            type = "ramdisk";
            arch = "powerpc";
            os = "linux";
            compression = "none";
            load = <0x01000000>;
            hash { algo = "crc32"; };
        };
    };

    configurations {
        default = "accton_as5610_52x";
        accton_as5610_52x {
            description = "EdgeNOS AS5610-52X";
            kernel = "kernel";
            fdt = "accton_as5610_52x_dtb";
            ramdisk = "initramfs";
        };
    };
};
ITSEOF

    (cd "${FITDIR}" && mkimage -f nos.its "${OUTDIR}/images/uImage-powerpc.itb") || true
    if [ ! -f "${OUTDIR}/images/uImage-powerpc.itb" ]; then
        echo "  ERROR: FIT image creation failed"
        return 1
    fi
    echo "  FIT image: ${OUTDIR}/images/uImage-powerpc.itb"
    ls -lh "${OUTDIR}/images/uImage-powerpc.itb"
    rm -rf "${FITDIR}"

    # Create self-extracting ONIE installer (data.tar payload)
    local TMPDIR=$(mktemp -d)
    cp "${OUTDIR}/images/uImage-powerpc.itb" "${TMPDIR}/"
    cp "${OUTDIR}/images/rootfs.sqsh" "${TMPDIR}/"
    (cd "${TMPDIR}" && tar cf payload.tar uImage-powerpc.itb rootfs.sqsh)

    cp /build/installer/install.sh "${OUTDIR}/images/edgenos-as5610-52x.bin"
    cat "${TMPDIR}/payload.tar" >> "${OUTDIR}/images/edgenos-as5610-52x.bin"
    chmod +x "${OUTDIR}/images/edgenos-as5610-52x.bin"
    rm -rf "${TMPDIR}"

    echo ""
    echo "============================================"
    echo "  BUILD COMPLETE"
    echo "============================================"
    echo ""
    echo "  ONIE installer: ${OUTDIR}/images/edgenos-as5610-52x.bin"
    ls -lh "${OUTDIR}/images/edgenos-as5610-52x.bin"
    echo ""
    echo "  Install via: onie-nos-install <path-to-bin>"
    echo "============================================"
}

# ── Main ──────────────────────────────────────────────────────
CMD="${1:-all}"

case "$CMD" in
    kernel)     build_kernel ;;
    modules)    build_platform_modules ;;
    sdk)        build_sdk ;;
    switchd)    build_switchd ;;
    rootfs)     build_rootfs ;;
    image)      build_image ;;
    all)
        build_kernel
        build_platform_modules
        build_sdk
        build_switchd
        build_rootfs
        build_image
        ;;
    bash|sh)
        exec /bin/bash
        ;;
    *)
        echo "Usage: $0 {all|kernel|modules|sdk|switchd|rootfs|image|bash}"
        exit 1
        ;;
esac
ENTRYPOINT_SCRIPT

RUN chmod +x /build/docker-build.sh

ENTRYPOINT ["/build/docker-build.sh"]
CMD ["all"]
