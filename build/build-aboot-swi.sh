#!/bin/sh
# build-aboot-swi.sh - package a kernel + initramfs into an Aboot .swi.
#
# An Arista .swi is a ZIP that Aboot boots by running the top-level `boot0`
# script (it exports $swipath and provides busybox unzip + kexec). Layout:
#   version       - key=value metadata Aboot reads (SWI_ARCH/VERSION/...)
#   boot0         - stage-0 shell entry (unzips the kernel+initrd and kexecs)
#   linux-i386    - the kernel (bzImage)
#   initrd-i386   - the initramfs (cpio.gz)
# The 7150 runs UNSIGNED SWIs (4.16-class boot chain; verified — no swi-signature,
# no key/TPM enforcement), so no signing step is needed. See arista edgenos/
# BOOTLOADER.md + memory edgenos-boot-feasibility.
#
# Usage:
#   build-aboot-swi.sh --kernel K --initramfs I [--boot0 B] --out O
#                      [--rootfs S] [--version V] [--release R]
#   --rootfs adds a squashfs rootfs-i386.sqsh member (M1-python / M2); the
#   initramfs is then responsible for loop-mounting it + switch_root.
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

KERNEL="" INITRAMFS="" BOOT0="" OUT="" ROOTFS="" VERSION="0.0.1-m0" RELEASE="edgenos-m0"

usage() { grep '^# ' "$0" | sed 's/^# //'; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --kernel)    KERNEL="$2";    shift 2;;
        --initramfs) INITRAMFS="$2"; shift 2;;
        --boot0)     BOOT0="$2";     shift 2;;
        --out)       OUT="$2";       shift 2;;
        --rootfs)    ROOTFS="$2";    shift 2;;   # optional squashfs (M1-python/M2)
        --version)   VERSION="$2";   shift 2;;
        --release)   RELEASE="$2";   shift 2;;
        -h|--help)   usage;;
        *) echo "unknown arg: $1" >&2; usage;;
    esac
done

[ -n "$KERNEL" ] && [ -n "$INITRAMFS" ] && [ -n "$OUT" ] || usage
[ -f "$KERNEL" ]    || { echo "no kernel: $KERNEL" >&2; exit 1; }
[ -f "$INITRAMFS" ] || { echo "no initramfs: $INITRAMFS" >&2; exit 1; }

command -v zip >/dev/null || { echo "need 'zip'" >&2; exit 1; }

# Default boot0 (kexec our kernel+initrd) if none supplied.
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

if [ -n "$BOOT0" ]; then
    cp "$BOOT0" "$STAGE/boot0"
else
    cat > "$STAGE/boot0" <<'EOF'
#!/bin/sh
# EdgeNOS Aboot stage-0: unzip our kernel+initramfs and kexec them.
kernel=linux-i386; arch=i386
: "${swipath:=$0}"
if [ -d "${swipath}" ]; then
   cp "${swipath}/${kernel}" "${swipath}/initrd-${arch}" /tmp/
else
   unzip -oq "${swipath}" "${kernel}" "initrd-${arch}" -d /tmp
fi
# nosmp reboot=p,force — the reboot hang is the post-kexec SMP state, not the reset
# method; single-core + skip-teardown lets the CF9 reset (EOS's method) fire.
echo "console=ttyS0,9600 earlyprintk=serial,ttyS0,9600 rdinit=/init panic=10 nosmp reboot=p,force" > /tmp/append
kexec --load --initrd="/tmp/initrd-${arch}" --append="$(cat /tmp/append)" "/tmp/${kernel}"
kexec --exec
EOF
fi
chmod +x "$STAGE/boot0"

cp "$KERNEL"    "$STAGE/linux-i386"
cp "$INITRAMFS" "$STAGE/initrd-i386"

# Optional squashfs rootfs member (M1-python / M2). When present, the initramfs is
# expected to loop-mount rootfs-i386.sqsh from /tmp (tmpfs = RAM) and switch_root.
MEMBERS="version boot0 linux-i386 initrd-i386"
if [ -n "$ROOTFS" ]; then
    [ -f "$ROOTFS" ] || { echo "no rootfs: $ROOTFS" >&2; exit 1; }
    cp "$ROOTFS" "$STAGE/rootfs-i386.sqsh"
    MEMBERS="$MEMBERS rootfs-i386.sqsh"
fi

# version metadata (unsigned; BLESSED=1 lets Aboot boot it without a swi-signature).
cat > "$STAGE/version" <<EOF
BLESSED=1
BUILD_HOST=$(hostname 2>/dev/null || echo edgenos-build)
SWI_ARCH=i686
SWI_FLAVOR=DEFAULT
SWI_MAX_HWEPOCH=1
SWI_RELEASE=${RELEASE}
SWI_VARIANT=US
SWI_VERSION=${VERSION}
BUILD_DATE=$(date -u +%Y%m%dT%H%M%SZ)
EOF

OUT_ABS="$(readlink -f "$OUT" 2>/dev/null || echo "$OUT")"
rm -f "$OUT_ABS"
# shellcheck disable=SC2086
( cd "$STAGE" && zip -qX "$OUT_ABS" $MEMBERS )

echo "built $OUT_ABS"
unzip -l "$OUT_ABS"
echo "NOTE: verify before boot with Aboot's dry-run: 'swi verify $OUT_ABS' (arista edgenos/BOOTLOADER.md checklist)."
