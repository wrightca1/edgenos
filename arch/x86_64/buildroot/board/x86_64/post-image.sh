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

# GRUB first stage (BIOS MBR code) for genimage: Buildroot leaves it in the grub2 build tree
bootimg=$(ls "$BUILD_DIR"/grub2-*/build-i386-pc/grub-core/boot.img 2>/dev/null | head -1)
[ -n "$bootimg" ] || bootimg=$(find "$HOST_DIR/lib/grub" "$TARGET/lib/grub" -name boot.img 2>/dev/null | head -1)
[ -n "$bootimg" ] || { echo "edgenos: GRUB boot.img not found (BIOS MBR stage)" >&2; exit 1; }
cp -f "$bootimg" "$OUT/boot.img"
ls -l "$OUT/bzImage" "$OUT/initrd.img" "$OUT/rootfs.squashfs" "$OUT/grub.img" "$OUT/boot.img" "$OUT/efi-part/EFI/BOOT/bootx64.efi" 2>/dev/null || true
