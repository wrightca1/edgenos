#!/bin/sh
# EdgeNOS ONIE Installer for Edgecore AS5610-52X
# Self-extracting: install.sh + data.tar payload after __ARCHIVE__
#
# Partition layout (single-slot, compatible with ONIE busybox):
#   sda1 = persistent config (ext2, 128MB)
#   sda2 = extended partition container
#   sda3 = rw overlay for overlayfs (ext2, rest of disk)
#   sda5 = kernel FIT image (raw, 16MB, starts sector 270336 = 0x42000)
#   sda6 = rootfs squashfs (raw, ~300MB)
#
# Boot chain:
#   U-Boot nos_bootcmd → usb read FIT from sda5 → bootm
#   Kernel → initramfs (nos-init) → mount squashfs sda6 → overlay sda3 → systemd
#
# Key U-Boot vars:
#   fdt_high=0xffffffff     (prevent FDT relocation → hang)
#   initrd_high=0xffffffff  (prevent initrd relocation)
#   onie_boot_reason=nos    (prevent ONIE install-mode loop)
#   nos_bootcmd             (usb read + bootm with bootargs)

set -e
export PATH="/sbin:/usr/sbin:/usr/bin:/bin:$PATH"

NOS_NAME="EdgeNOS"
NOS_VERSION="0.1.0"

log() { echo "$NOS_NAME: $*"; }
fatal() { log "FATAL: $*"; exit 1; }

# ── Detect block device ──────────────────────────────────────
BLK_DEV=""
for d in sda sdb; do
    [ -b "/dev/$d" ] && BLK_DEV="$d" && break
done
[ -n "$BLK_DEV" ] && [ -b "/dev/$BLK_DEV" ] || fatal "No block device found"
log "Install device: /dev/$BLK_DEV"

# ── Extract payload ──────────────────────────────────────────
extract_payload() {
    local line skip
    line=$(head -n 300 "$0" | awk '/^__ARCHIVE__$/ { print NR; exit }')
    [ -z "$line" ] && fatal "No __ARCHIVE__ marker found"
    skip=$(head -n "$line" "$0" | wc -c)
    {
        dd if="$0" bs=4096 skip=$((skip / 4096)) count=1 2>/dev/null | \
            dd bs=1 skip=$((skip % 4096)) 2>/dev/null
        dd if="$0" bs=4096 skip=$((skip / 4096 + 1)) 2>/dev/null
    }
}

# ── Partition disk ───────────────────────────────────────────
partition_disk() {
    log "Partitioning /dev/$BLK_DEV..."

    # Unmount everything
    for p in /dev/${BLK_DEV}*; do
        [ -b "$p" ] && [ "$p" != "/dev/$BLK_DEV" ] && umount "$p" 2>/dev/null || true
    done

    # MBR layout: primary1 + extended2(logical5,logical6) + primary3
    # NOTE: fdisk order matters - create primaries and extended first,
    # then logicals inside extended, then remaining primary
    printf "o\nn\np\n1\n8192\n270273\nn\ne\n2\n270274\n895839\nn\nl\n270336\n303041\nn\nl\n303104\n895839\nn\np\n3\n895840\n\nw\n" \
        | fdisk -u "/dev/$BLK_DEV" || true

    sync
    blockdev --rereadpt "/dev/$BLK_DEV" 2>/dev/null || true
    sleep 2

    # Format persistent config partition
    log "Formatting /dev/${BLK_DEV}1 (persist)..."
    mke2fs -L "NOS-PERSIST" "/dev/${BLK_DEV}1" || log "WARN: mke2fs sda1 failed"

    # Format rw overlay partition - CRITICAL for writable root
    log "Formatting /dev/${BLK_DEV}3 (rw overlay)..."
    mke2fs -L "NOS-RW" "/dev/${BLK_DEV}3" || log "WARN: mke2fs sda3 failed"

    # Verify sda3 is actually formatted
    mkdir -p /tmp/verify_rw
    if mount "/dev/${BLK_DEV}3" /tmp/verify_rw 2>/dev/null; then
        mkdir -p /tmp/verify_rw/upper /tmp/verify_rw/work
        log "  sda3 overlay dirs created (upper + work)"
        umount /tmp/verify_rw
    else
        log "WARN: could not mount sda3 - overlay will not work"
    fi

    log "Partitioning complete."
}

# ── Install kernel + rootfs ──────────────────────────────────
install_image() {
    log "Installing kernel and rootfs..."

    local tmpdir
    tmpdir=$(mktemp -d)
    extract_payload | tar -xf - -C "$tmpdir"

    # Write FIT image to raw sda5 (U-Boot loads via 'usb read')
    if [ -f "$tmpdir/uImage-powerpc.itb" ]; then
        log "  Writing FIT image to /dev/${BLK_DEV}5..."
        dd if="$tmpdir/uImage-powerpc.itb" of="/dev/${BLK_DEV}5" bs=4k conv=fsync 2>/dev/null
    else
        fatal "uImage-powerpc.itb not found in payload"
    fi

    # Write squashfs rootfs to raw sda6
    if [ -f "$tmpdir/rootfs.sqsh" ]; then
        log "  Writing rootfs to /dev/${BLK_DEV}6..."
        dd if="$tmpdir/rootfs.sqsh" of="/dev/${BLK_DEV}6" bs=4k conv=fsync 2>/dev/null
    else
        fatal "rootfs.sqsh not found in payload"
    fi

    rm -rf "$tmpdir"
    log "Image installed."
}

# ── Configure U-Boot environment ─────────────────────────────
configure_uboot() {
    log "Configuring U-Boot environment..."

    command -v fw_setenv >/dev/null 2>&1 || { log "WARN: fw_setenv not found"; return; }

    #
    # CRITICAL boot chain (verified against live Cumulus switch fw_printenv):
    #
    # U-Boot bootcmd: run check_boot_reason; run nos_bootcmd; run onie_bootcmd
    #
    # check_boot_reason: if onie_boot_reason is set (non-empty), boots ONIE.
    #   FIX: we must DELETE onie_boot_reason (not set it to "nos"!)
    #
    # nos_bootcmd: Cumulus uses a chain (flashboot -> usbboot).
    #   We use the proven Cumulus approach with usbboot (not raw usb read),
    #   which reads partition 5 as a raw FIT image.
    #
    # Cumulus boot chain variables (from fw_printenv on live switch):
    #   usbiddev -> identifies USB device number
    #   initargs -> sets console + root device
    #   hw_active1 -> usbboot from partition 5 + bootm
    #   set_active1 -> selects slot 1 partitions
    #   boot_active -> set_active + hw_active
    #   flashboot -> boot_active
    #   lbootcmd -> flashboot
    #   nos_bootcmd -> lbootcmd
    #
    # We simplify to a single nos_bootcmd that does the full sequence.
    # Using 'usbboot' (proven on this hardware) instead of 'usb read'.
    #

    local envfile=$(mktemp)
    cat > "$envfile" << 'UBOOT_ENV'
fdt_high 0xffffffff
initrd_high 0xffffffff
nos_bootcmd usb start; usbiddev; setenv bootargs console=ttyS0,115200 root=/dev/sda6 rootfstype=squashfs ro rootwait; usbboot 0x02000000 ${usbdev}:5 && bootm 0x02000000#accton_as5610_52x
boot_count 0
UBOOT_ENV
    # Delete onie_boot_reason (empty value = delete in fw_setenv -s)
    # If onie_boot_reason is set to ANY value, U-Boot boots ONIE instead of NOS!
    echo "onie_boot_reason" >> "$envfile"

    fw_setenv -f -s "$envfile" 2>/dev/null || log "WARN: fw_setenv failed"
    rm -f "$envfile"

    log "U-Boot environment configured."
}

# ── Setup persistent config ──────────────────────────────────
setup_persist() {
    local mnt
    mnt=$(mktemp -d)
    mount "/dev/${BLK_DEV}1" "$mnt" || { log "WARN: cannot mount persist"; return; }

    mkdir -p "$mnt/etc" "$mnt/ssh"

    if command -v ssh-keygen >/dev/null 2>&1; then
        if [ ! -f "$mnt/ssh/ssh_host_ed25519_key" ]; then
            log "Generating SSH host keys..."
            ssh-keygen -t ed25519 -f "$mnt/ssh/ssh_host_ed25519_key" -N "" -q 2>/dev/null || true
            ssh-keygen -t rsa -b 2048 -f "$mnt/ssh/ssh_host_rsa_key" -N "" -q 2>/dev/null || true
        fi
    fi

    cat > "$mnt/etc/nos.conf" << 'CONF'
# EdgeNOS Configuration (persists across upgrades)
[system]
hostname = edgenos

[management]
interface = eth0
method = dhcp
CONF

    umount "$mnt"
    rmdir "$mnt"
    log "Persistent config initialized."
}

# ── Main ─────────────────────────────────────────────────────
main() {
    log "=== $NOS_NAME v$NOS_VERSION Installer ==="

    partition_disk
    install_image
    configure_uboot
    setup_persist

    log "=== Installation complete ==="
    log "System will boot $NOS_NAME on next reboot."

    /sbin/reboot 2>/dev/null || reboot 2>/dev/null || true
}

main "$@"
exit 0
__ARCHIVE__
