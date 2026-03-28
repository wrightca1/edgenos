#!/bin/bash
# build-installer.sh - Build FIT image and ONIE installer
set -e

TOPDIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="$TOPDIR/output"
IMGDIR="$OUTDIR/images"
PLATFORM="powerpc-accton-as5610-52x-r0"

mkdir -p "$IMGDIR"

build_fit() {
    echo "==> Building FIT image..."

    local UIMAGE="$OUTDIR/kernel/uImage"
    local DTB="$OUTDIR/kernel/as5610-52x.dtb"
    local INITRAMFS="$TOPDIR/initramfs.cpio.gz"

    for f in "$UIMAGE" "$DTB"; do
        if [ ! -f "$f" ]; then
            echo "ERROR: Required file not found: $f"
            exit 1
        fi
    done

    # Build initramfs if not present
    if [ ! -f "$INITRAMFS" ]; then
        echo "  Building initramfs..."
        bash "$TOPDIR/initramfs-build.sh"
    fi
    if [ ! -f "$INITRAMFS" ]; then
        echo "ERROR: initramfs.cpio.gz not found and build failed"
        exit 1
    fi

    # Extract gzip kernel payload from uImage (strip 64-byte mkimage header)
    # The uImage contains: mkimage_header(64 bytes) + gzip(vmlinux)
    # FIT needs the raw gzip, not the uImage wrapper
    local KERNEL_GZ="$IMGDIR/kernel.gz"
    dd if="$UIMAGE" bs=64 skip=1 of="$KERNEL_GZ" 2>/dev/null
    echo "  kernel.gz: $(ls -lh "$KERNEL_GZ" | awk '{print $5}')"

    # Generate ITS matching Cumulus 2.5.0 FIT format exactly.
    # See BOOT.md "FIT Image Format" for why each field matters.
    #
    # CRITICAL RULES (all verified on hardware March 28, 2026):
    #   - kernel: load=0x00, entry=0x00, compression=gzip
    #   - DTB: NO load address (let U-Boot place it)
    #   - initramfs: REQUIRED, load=0x00, compression=none
    #   - node names: no @1 suffix (match Cumulus convention)
    #   - Do NOT set fdt_high or initrd_high in U-Boot env
    cat > "$IMGDIR/nos.its" << EOF
/dts-v1/;

/ {
    description = "PowerPC kernel, initramfs and FDT blobs";
    #address-cells = <0x01>;

    images {
        kernel {
            description = "PowerPC Kernel";
            data = /incbin/("$(realpath "$KERNEL_GZ")");
            type = "kernel";
            arch = "ppc";
            os = "linux";
            compression = "gzip";
            load = <0x00>;
            entry = <0x00>;
        };

        initramfs {
            description = "initramfs";
            data = /incbin/("$(realpath "$INITRAMFS")");
            type = "ramdisk";
            arch = "ppc";
            os = "linux";
            compression = "none";
            load = <0x00>;
        };

        accton_as5610_52x_dtb {
            description = "accton_as5610_52x.dtb";
            data = /incbin/("$(realpath "$DTB")");
            type = "flat_dt";
            arch = "ppc";
            os = "linux";
            compression = "none";
        };
    };

    configurations {
        default = "accton_as5610_52x";

        accton_as5610_52x {
            description = "accton_as5610_52x";
            kernel = "kernel";
            ramdisk = "initramfs";
            fdt = "accton_as5610_52x_dtb";
        };
    };
};
EOF

    mkimage -f "$IMGDIR/nos.its" "$IMGDIR/uImage-powerpc.itb"
    echo "==> FIT image: $IMGDIR/uImage-powerpc.itb"
    ls -lh "$IMGDIR/uImage-powerpc.itb"
}

build_image() {
    echo "==> Building ONIE installer..."

    local INSTALLER="$TOPDIR/installer/install.sh"
    local FIT="$IMGDIR/uImage-powerpc.itb"
    local OUTPUT="$IMGDIR/edgenos-as5610-52x.bin"

    if [ ! -f "$FIT" ]; then
        echo "ERROR: FIT image not found. Run '$0 fit' first."
        exit 1
    fi

    # Create self-extracting installer
    # The install.sh expects a tar archive after __ARCHIVE__ containing:
    #   uImage-powerpc.itb  (FIT: kernel + DTB)
    #   rootfs.sqsh         (squashfs root filesystem)
    local ROOTFS="$IMGDIR/rootfs.sqsh"
    if [ ! -f "$ROOTFS" ]; then
        echo "ERROR: rootfs.sqsh not found."
        exit 1
    fi

    # Build payload tar
    local PAYLOAD="$IMGDIR/payload.tar"
    tar -cf "$PAYLOAD" -C "$IMGDIR" uImage-powerpc.itb rootfs.sqsh
    echo "  payload.tar: $(ls -lh "$PAYLOAD" | awk '{print $5}')"

    # Assemble: install.sh already ends with __ARCHIVE__ marker
    # Just append the payload tar directly after it
    cp "$INSTALLER" "$OUTPUT"
    cat "$PAYLOAD" >> "$OUTPUT"

    chmod +x "$OUTPUT"
    echo "==> ONIE installer: $OUTPUT"
    ls -lh "$OUTPUT"
}

case "${1:-}" in
    fit)   build_fit ;;
    image) build_image ;;
    *)
        echo "Usage: $0 {fit|image}"
        exit 1
        ;;
esac
