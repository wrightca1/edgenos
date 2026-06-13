#!/bin/bash
# package-image.sh — build the ONIE installer .bin from already-built artifacts
# (output/kernel/uImage + dtb, output/images/rootfs.sqsh). Mirrors the Dockerfile
# build_image() step but standalone, so the image can be repackaged without a full
# pipeline run. Runs inside the edgenos-builder container.
#
# NOTE: nos-init.c is linked with -lgcc (PPC needs libgcc's _restgpr/_savegpr
# register-save helpers; -nodefaultlibs alone leaves them undefined).
set -e
SRC=/build/src
OUT="$SRC/output"
UIMAGE="$OUT/kernel/uImage"
DTB="$OUT/kernel/as5610-52x.dtb"
ROOTFS="$OUT/images/rootfs.sqsh"
for f in "$UIMAGE" "$DTB" "$ROOTFS"; do
    [ -f "$f" ] || { echo "ERROR: missing $f"; exit 1; }
done

FITDIR="$OUT/images/fitbuild"; rm -rf "$FITDIR"; mkdir -p "$FITDIR"
echo "==> extracting raw kernel..."
dumpimage -T kernel -p 0 -o "$FITDIR/kernel.gz" "$UIMAGE" 2>/dev/null || \
    dd if="$UIMAGE" of="$FITDIR/kernel.gz" bs=64 skip=1 2>/dev/null
cp "$DTB" "$FITDIR/as5610_52x.dtb"

echo "==> building initramfs (nos-init, -lgcc)..."
IRDIR=$(mktemp -d)
if powerpc-linux-gnu-gcc -static -nostdlib -nostartfiles -nodefaultlibs -Os -Wall \
        -o "$IRDIR/init" "$SRC/initramfs/nos-init.c" -lgcc 2>/dev/null && [ -f "$IRDIR/init" ]; then
    echo "   nos-init compiled OK ($(stat -c%s "$IRDIR/init") bytes)"
else
    echo "ERROR: nos-init compile failed"; exit 1
fi
(cd "$IRDIR" && find . | cpio -o -H newc 2>/dev/null | gzip -9 > "$FITDIR/initramfs.cpio.gz")
rm -rf "$IRDIR"

echo "==> generating FIT..."
# FIT load map: kernel decompresses at 0x0. DTB/initramfs MUST sit above the
# decompressed kernel or U-Boot aborts with "image is not a fdt" once the
# kernel overwrites the blob. The 6.1 kernel grew past the old 0x00f00000 (15M)
# DTB address (5.15 just fit under it), so DTB->0x03000000 (48M) and
# initramfs->0x03100000 give generous headroom (well within the bootmap).
cat > "$FITDIR/nos.its" << 'ITSEOF'
/dts-v1/;
/ {
    description = "EdgeNOS for AS5610-52X";
    #address-cells = <1>;
    images {
        kernel { description = "PowerPC Kernel"; data = /incbin/("kernel.gz");
            type = "kernel"; arch = "powerpc"; os = "linux"; compression = "gzip";
            load = <0x00000000>; entry = <0x00000000>; hash { algo = "crc32"; }; };
        accton_as5610_52x_dtb { description = "AS5610-52X device tree";
            data = /incbin/("as5610_52x.dtb"); type = "flat_dt"; arch = "powerpc";
            compression = "none"; load = <0x03000000>; hash { algo = "crc32"; }; };
        initramfs { description = "initramfs stub"; data = /incbin/("initramfs.cpio.gz");
            type = "ramdisk"; arch = "powerpc"; os = "linux"; compression = "none";
            load = <0x03100000>; hash { algo = "crc32"; }; };
    };
    configurations {
        default = "accton_as5610_52x";
        accton_as5610_52x { description = "EdgeNOS AS5610-52X"; kernel = "kernel";
            fdt = "accton_as5610_52x_dtb"; ramdisk = "initramfs"; };
    };
};
ITSEOF
(cd "$FITDIR" && mkimage -f nos.its "$OUT/images/uImage-powerpc.itb")
[ -f "$OUT/images/uImage-powerpc.itb" ] || { echo "ERROR: FIT failed"; exit 1; }

echo "==> assembling ONIE installer .bin..."
TMPDIR=$(mktemp -d)
cp "$OUT/images/uImage-powerpc.itb" "$TMPDIR/"
cp "$ROOTFS" "$TMPDIR/"
(cd "$TMPDIR" && tar cf payload.tar uImage-powerpc.itb rootfs.sqsh)
cp "$SRC/installer/install.sh" "$OUT/images/edgenos-as5610-52x.bin"
cat "$TMPDIR/payload.tar" >> "$OUT/images/edgenos-as5610-52x.bin"
chmod +x "$OUT/images/edgenos-as5610-52x.bin"
rm -rf "$TMPDIR" "$FITDIR"
echo "==> DONE:"
ls -lh "$OUT/images/edgenos-as5610-52x.bin"
