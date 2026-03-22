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
    local ROOTFS="$IMGDIR/rootfs.sqsh"

    for f in "$UIMAGE" "$DTB" "$ROOTFS"; do
        if [ ! -f "$f" ]; then
            echo "ERROR: Required file not found: $f"
            exit 1
        fi
    done

    # Generate ITS (Image Tree Source)
    cat > "$IMGDIR/nos.its" << EOF
/dts-v1/;

/ {
    description = "EdgeNOS for AS5610-52X";
    #address-cells = <1>;

    images {
        kernel@1 {
            description = "Linux kernel";
            data = /incbin/("$(realpath "$UIMAGE")");
            type = "kernel";
            arch = "ppc";
            os = "linux";
            compression = "none";
            load = <0x00000000>;
            entry = <0x00000000>;
        };

        fdt@1 {
            description = "AS5610-52X device tree";
            data = /incbin/("$(realpath "$DTB")");
            type = "flat_dt";
            arch = "ppc";
            compression = "none";
        };

        rootfs@1 {
            description = "Root filesystem (squashfs)";
            data = /incbin/("$(realpath "$ROOTFS")");
            type = "ramdisk";
            arch = "ppc";
            os = "linux";
            compression = "none";
        };
    };

    configurations {
        default = "accton_as5610_52x";

        accton_as5610_52x {
            description = "EdgeNOS AS5610-52X";
            kernel = "kernel@1";
            fdt = "fdt@1";
            ramdisk = "rootfs@1";
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
    # The installer script + FIT image are concatenated
    cp "$INSTALLER" "$OUTPUT"
    echo "__ARCHIVE__" >> "$OUTPUT"
    cat "$FIT" >> "$OUTPUT"

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
