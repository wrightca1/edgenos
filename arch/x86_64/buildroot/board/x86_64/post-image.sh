#!/bin/sh
# EdgeNOS x86_64 base: post-image. Assembles the initramfs (busybox + the squashfs/
# overlay boot script) and collects the boot artefacts into BINARIES_DIR:
#   bzImage, initrd.img, rootfs.squashfs, grub.img (BIOS core), boot.img (BIOS MBR),
#   efi-part/EFI/BOOT/bootx64.efi
set -e
BOARD_DIR="$(cd "$(dirname "$0")" && pwd)"
TARGET="$TARGET_DIR"
OUT="$BINARIES_DIR"
WORK="$BUILD_DIR/edgenos-initramfs"

rm -rf "$WORK"; mkdir -p "$WORK"/bin "$WORK"/sbin "$WORK"/lib "$WORK"/usr "$WORK"/proc "$WORK"/sys "$WORK"/dev \
      "$WORK"/mnt/boot "$WORK"/mnt/lower "$WORK"/mnt/data "$WORK"/mnt/root "$WORK"/etc "$WORK"/run
# glibc's loader searches /lib64 and /usr/lib64 (the target satisfies that with symlinks) — mirror it
ln -s lib "$WORK/lib64"
ln -s ../lib "$WORK/usr/lib"
ln -s ../lib "$WORK/usr/lib64"

# busybox + its shared libs (the base busybox is dynamically linked against glibc).
# NB: the target has a merged /usr (lib -> usr/lib), so follow the symlinks (find -H, trailing /).
READELF=$HOST_DIR/bin/x86_64-buildroot-linux-gnu-readelf; [ -x "$READELF" ] || READELF=readelf
cp -aL "$TARGET/bin/busybox" "$WORK/bin/busybox"
for lib in $("$READELF" -d "$TARGET/bin/busybox" 2>/dev/null | awk '/NEEDED/{gsub(/[][]/,"",$5); print $5}'); do
    src=$(find -H "$TARGET/lib/" "$TARGET/lib64/" "$TARGET/usr/lib/" -maxdepth 1 -name "$lib" 2>/dev/null | head -1)
    [ -n "$src" ] || { echo "edgenos: initramfs: library $lib not found in target" >&2; exit 1; }
    cp -aL "$src" "$WORK/lib/$lib"
done
# the dynamic loader
ld=$(find -H "$TARGET/lib/" "$TARGET/lib64/" "$TARGET/usr/lib/" -maxdepth 1 -name 'ld-linux-x86-64.so.2' 2>/dev/null | head -1)
[ -n "$ld" ] || { echo "edgenos: initramfs: ld-linux-x86-64.so.2 not found" >&2; exit 1; }
cp -aL "$ld" "$WORK/lib/ld-linux-x86-64.so.2"
# applet links
for a in sh mount umount losetup findfs blkid switch_root mkdir sleep echo cat ls grep sed cut awk uname dmesg; do
    ln -sf busybox "$WORK/bin/$a"
done
ln -sf ../bin/busybox "$WORK/sbin/switch_root"
install -m 0755 "$BOARD_DIR/initramfs/init" "$WORK/init"

( cd "$WORK" && find . -print0 | cpio --null -o -H newc --quiet --owner=0:0 ) | gzip -9n > "$OUT/initrd.img"
echo "edgenos: initrd.img $(du -h "$OUT/initrd.img" | cut -f1)"

# GRUB images with the early config INSIDE them (memdisk, prefix=(memdisk)/boot/grub — the
# grub-mkstandalone model): no dependence on an embedded rescue-mode config or on a grub.cfg
# at a fixed disk path. Built from the grub2 package's build trees; if those are gone
# (BR_TRIM), the previously generated images are kept.
GRUB_SRC=$(ls -d "$BUILD_DIR"/grub2-*/ 2>/dev/null | head -1)
MKIMAGE=$HOST_DIR/bin/grub-mkimage
if [ -n "$GRUB_SRC" ] && [ -x "$MKIMAGE" ] && [ -d "$GRUB_SRC/build-i386-pc/grub-core" ] && [ -d "$GRUB_SRC/build-x86_64-efi/grub-core" ]; then
    MEM="$BUILD_DIR/edgenos-grub-memdisk"; rm -rf "$MEM"; mkdir -p "$MEM/boot/grub"
    install -m 0644 "$BOARD_DIR/grub-early.cfg" "$MEM/boot/grub/grub.cfg"
    ( cd "$MEM" && tar --owner=0 --group=0 --mtime=@0 -cf "$BUILD_DIR/edgenos-grub-memdisk.tar" boot )
    COMMON="memdisk tar normal linux boot ext2 fat squash4 part_msdos part_gpt search search_label search_fs_uuid configfile echo test sleep serial terminal"
    "$MKIMAGE" -O i386-pc -d "$GRUB_SRC/build-i386-pc/grub-core" -m "$BUILD_DIR/edgenos-grub-memdisk.tar" \
        -p '(memdisk)/boot/grub' -o "$OUT/grub.img" biosdisk $COMMON
    mkdir -p "$OUT/efi-part/EFI/BOOT"
    "$MKIMAGE" -O x86_64-efi -d "$GRUB_SRC/build-x86_64-efi/grub-core" -m "$BUILD_DIR/edgenos-grub-memdisk.tar" \
        -p '(memdisk)/boot/grub' -o "$OUT/efi-part/EFI/BOOT/bootx64.efi" efi_gop efi_uga $COMMON
    # the same early config as the ESP's plain grub.cfg too (harmless; helps manual chainloads)
    install -m 0644 "$BOARD_DIR/grub-early.cfg" "$OUT/efi-part/EFI/BOOT/grub.cfg"
    cp -f "$BOARD_DIR/grub-early.cfg" "$OUT/grub-early.cfg"
    echo "edgenos: GRUB images regenerated with the memdisk early config"
else
    echo "edgenos: grub2 build trees not present — keeping existing grub.img / bootx64.efi"
fi

ls -l "$OUT/bzImage" "$OUT/initrd.img" "$OUT/rootfs.squashfs" "$OUT/grub.img" "$OUT/boot.img" "$OUT/efi-part/EFI/BOOT/bootx64.efi" 2>/dev/null || true
