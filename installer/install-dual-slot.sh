#!/bin/sh
# EdgeNOS ONIE Installer for Edgecore AS5610-52X — Cumulus-style dual-slot
# Self-extracting: install-dual-slot.sh + data.tar payload after __ARCHIVE__
#
# Partition layout (Cumulus 2.5.0 AS5610 layout, captured 2026-05-11):
#   sda1 = persist (ext2, 128MB)
#   sda2 = extended container
#   sda3 = root-rw / overlay upper (ext2, fills remaining ~3GB)
#   sda5 = kernel FIT slot A (raw, 32MB) [16353KB rounded up]
#   sda6 = squashfs slot 1 (raw, 128MB) [131041KB]
#   sda7 = kernel FIT slot B (raw, 32MB)
#   sda8 = squashfs slot 2 (raw, 128MB)
#
# Boot chain (Cumulus-style):
#   U-Boot:
#     cl.active = 1 | 2 (which slot to boot)
#     boot_active = run set_active1 or set_active2 based on cl.active
#     set_active1/2 = set bootargs active=N + run hw_active1/2
#     hw_active1 = usbboot kernel from sda5, bootargs root=/dev/sda6
#     hw_active2 = usbboot kernel from sda7, bootargs root=/dev/sda8
#     nos_bootcmd = run initargs boot_active
#
# This installer always writes to slot 1 on fresh install. Slot 2 is left
# empty and gets populated during an upgrade via nos-upgrade (see
# config/rootfs/overlay/usr/sbin/nos-upgrade).
#
# CRITICAL: ONIE's BusyBox kernel only exposes 3 logical partitions during
# the install session. We create all four (sda5–sda8) in the partition
# table, but only sda5 and sda6 are guaranteed addressable from the
# installer. After reboot into EdgeNOS, sda7/sda8 device nodes appear and
# nos-upgrade can write slot 2.

set -e
export PATH="/sbin:/usr/sbin:/usr/bin:/bin:$PATH"

NOS_NAME="EdgeNOS"
NOS_VERSION="0.1.0"
SLOT_TARGET="1"   # always slot 1 on fresh install

log() { echo "$NOS_NAME: $*"; }
fatal() { log "FATAL: $*"; exit 1; }

# ── Detect block device ──────────────────────────────────────
BLK_DEV=""
for d in sda sdb; do
    [ -b "/dev/$d" ] && BLK_DEV="$d" && break
done
[ -n "$BLK_DEV" ] && [ -b "/dev/$BLK_DEV" ] || fatal "No block device found"
log "Install device: /dev/$BLK_DEV (slot $SLOT_TARGET)"

# ── Extract payload ──────────────────────────────────────────
extract_payload() {
    local line skip
    line=$(head -n 400 "$0" | awk '/^__ARCHIVE__$/ { print NR; exit }')
    [ -z "$line" ] && fatal "No __ARCHIVE__ marker found"
    skip=$(head -n "$line" "$0" | wc -c)
    {
        dd if="$0" bs=4096 skip=$((skip / 4096)) count=1 2>/dev/null | \
            dd bs=1 skip=$((skip % 4096)) 2>/dev/null
        dd if="$0" bs=4096 skip=$((skip / 4096 + 1)) 2>/dev/null
    }
}

# ── Partition disk (Cumulus-style dual-slot) ─────────────────
#
# Sector math (512-byte sectors, 4 GB USB flash assumption):
#   sda1: 8192–270273      (128 MB)  persist
#   sda2: extended 270274–end
#     sda5: 270336–335872  (32 MB)   kernel A
#     sda6: 335936–598080  (128 MB)  squashfs slot 1
#     sda7: 598144–663680  (32 MB)   kernel B
#     sda8: 663744–925888  (128 MB)  squashfs slot 2
#   sda3: 925952–end       (~2.6 GB) root-rw
#
# fdisk recipe:
#   o = new MBR
#   n p 1 8192 270273         = primary 1
#   n e 2 270274 925888       = extended 2 (covers all four logicals)
#   n l 270336 335872         = logical 5
#   n l 335936 598080         = logical 6
#   n l 598144 663680         = logical 7
#   n l 663744 925888         = logical 8
#   n p 3 925952 (default)    = primary 3
#   w = write
partition_disk() {
    log "Partitioning /dev/$BLK_DEV (Cumulus dual-slot layout)..."

    for p in /dev/${BLK_DEV}*; do
        [ -b "$p" ] && [ "$p" != "/dev/$BLK_DEV" ] && umount "$p" 2>/dev/null || true
    done

    printf "o\nn\np\n1\n8192\n270273\nn\ne\n2\n270274\n925888\nn\nl\n270336\n335872\nn\nl\n335936\n598080\nn\nl\n598144\n663680\nn\nl\n663744\n925888\nn\np\n3\n925952\n\nw\n" \
        | fdisk -u "/dev/$BLK_DEV" || true

    sync
    blockdev --rereadpt "/dev/$BLK_DEV" 2>/dev/null || true
    sleep 2

    log "Formatting /dev/${BLK_DEV}1 (persist)..."
    mke2fs -L "NOS-PERSIST" "/dev/${BLK_DEV}1" || log "WARN: mke2fs sda1 failed"

    log "Formatting /dev/${BLK_DEV}3 (rw overlay)..."
    mke2fs -L "NOS-RW" "/dev/${BLK_DEV}3" || log "WARN: mke2fs sda3 failed"

    mkdir -p /tmp/verify_rw
    if mount "/dev/${BLK_DEV}3" /tmp/verify_rw 2>/dev/null; then
        mkdir -p /tmp/verify_rw/config1 /tmp/verify_rw/config2
        umount /tmp/verify_rw
        log "  sda3 overlay slots created (config1 + config2)"
    fi

    log "Partitioning complete."
}

# ── Install kernel + rootfs into slot 1 ──────────────────────
install_image() {
    log "Installing kernel and rootfs into slot $SLOT_TARGET..."

    local tmpdir kpart spart
    tmpdir=$(mktemp -d)
    extract_payload | tar -xf - -C "$tmpdir"

    if [ "$SLOT_TARGET" = "1" ]; then
        kpart="${BLK_DEV}5"; spart="${BLK_DEV}6"
    else
        kpart="${BLK_DEV}7"; spart="${BLK_DEV}8"
    fi

    [ -f "$tmpdir/uImage-powerpc.itb" ] || fatal "uImage-powerpc.itb not in payload"
    [ -f "$tmpdir/rootfs.sqsh" ]       || fatal "rootfs.sqsh not in payload"

    log "  Writing FIT image to /dev/$kpart..."
    dd if="$tmpdir/uImage-powerpc.itb" of="/dev/$kpart" bs=4k conv=fsync 2>/dev/null

    log "  Writing rootfs to /dev/$spart..."
    dd if="$tmpdir/rootfs.sqsh" of="/dev/$spart" bs=4k conv=fsync 2>/dev/null

    rm -rf "$tmpdir"
    log "Slot $SLOT_TARGET populated."
}

# ── Configure U-Boot (Cumulus-style slot chain) ──────────────
#
# nos_bootcmd runs initargs then boot_active. boot_active dispatches on
# cl.active. hw_active1/2 do `usb start; usbiddev; usbboot $loadaddr
# ${usbdev}:<part>` for the chosen slot and `bootm` the FIT.
configure_uboot() {
    log "Configuring U-Boot environment (dual-slot)..."

    command -v fw_setenv >/dev/null 2>&1 || { log "WARN: fw_setenv not found"; return; }

    local envfile
    envfile=$(mktemp)
    cat > "$envfile" << 'UBOOT_ENV'
fdt_high 0xffffffff
initrd_high 0xffffffff
cl.active 1
slot_state1 0
slot_state2 0
hw_active1 usb start; usbiddev; usbboot 0x02000000 ${usbdev}:5 && bootm 0x02000000#accton_as5610_52x
hw_active2 usb start; usbiddev; usbboot 0x02000000 ${usbdev}:7 && bootm 0x02000000#accton_as5610_52x
set_active1 setenv bootargs console=ttyS0,115200 cma=32M root=/dev/sda6 rootfstype=squashfs ro rootwait earlycon active=1; run hw_active1
set_active2 setenv bootargs console=ttyS0,115200 cma=32M root=/dev/sda8 rootfstype=squashfs ro rootwait earlycon active=2; run hw_active2
boot_active if test ${cl.active} = 1; then run set_active1; else run set_active2; fi
boot_alt if test ${cl.active} = 1; then run set_active2; else run set_active1; fi
nos_bootcmd run boot_active
boot_count 0
UBOOT_ENV
    # Delete onie_boot_reason so check_boot_reason falls through to NOS.
    echo "onie_boot_reason" >> "$envfile"

    fw_setenv -f -s "$envfile" 2>/dev/null || log "WARN: fw_setenv failed"
    rm -f "$envfile"

    log "U-Boot configured: cl.active=1, hw_active1/2 wired."
}

# ── Persistent config ───────────────────────────────────────
setup_persist() {
    local mnt
    mnt=$(mktemp -d)
    mount "/dev/${BLK_DEV}1" "$mnt" || { log "WARN: cannot mount persist"; return; }

    mkdir -p "$mnt/etc" "$mnt/ssh"

    if command -v ssh-keygen >/dev/null 2>&1; then
        if [ ! -f "$mnt/ssh/ssh_host_ed25519_key" ]; then
            ssh-keygen -t ed25519 -f "$mnt/ssh/ssh_host_ed25519_key" -N "" -q 2>/dev/null || true
            ssh-keygen -t rsa -b 2048 -f "$mnt/ssh/ssh_host_rsa_key"     -N "" -q 2>/dev/null || true
        fi
    fi

    cat > "$mnt/etc/nos.conf" << 'CONF'
# EdgeNOS Configuration (persists across slot upgrades)
[system]
hostname = edgenos

[management]
interface = eth0
method = dhcp

[image]
slot_active = 1
CONF

    umount "$mnt"
    rmdir "$mnt"
    log "Persistent config initialized."
}

main() {
    log "=== $NOS_NAME v$NOS_VERSION dual-slot Installer ==="

    partition_disk
    install_image
    configure_uboot
    setup_persist

    log "=== Installation complete (slot $SLOT_TARGET active) ==="
    /sbin/reboot 2>/dev/null || reboot 2>/dev/null || true
}

main "$@"
exit 0
__ARCHIVE__
