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

# ── Image-validation helpers (mirror /usr/sbin/nos-upgrade) ──
# part_size <block-dev> -> capacity in bytes via sysfs (universal), else blockdev, else 0.
part_size() {
    name=$(basename "$1")
    if [ -r "/sys/class/block/$name/size" ]; then
        echo $(( $(cat "/sys/class/block/$name/size") * 512 ))
    elif command -v blockdev >/dev/null 2>&1; then
        blockdev --getsize64 "$1" 2>/dev/null || echo 0
    else
        echo 0
    fi
}
# first 4 bytes as 8 lowercase hex chars (big-endian)
magic_u32() { od -An -tx1 -N4 "$1" 2>/dev/null | tr -d ' \n'; }
# sha_of <file>  /  sha_head <nbytes> <file|dev>  -> hash, preferring sha256 then md5; empty if neither
sha_of()   { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; elif command -v md5sum >/dev/null 2>&1; then md5sum "$1" | cut -d' ' -f1; else echo ""; fi; }
sha_head() { if command -v sha256sum >/dev/null 2>&1; then head -c "$1" "$2" | sha256sum | cut -d' ' -f1; elif command -v md5sum >/dev/null 2>&1; then head -c "$1" "$2" | md5sum | cut -d' ' -f1; else echo ""; fi; }

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

    local tmpdir kpart spart ksrc rsrc kmagic rmagic ksz rsz kcap rcap a b
    tmpdir=$(mktemp -d)
    extract_payload | tar -xf - -C "$tmpdir"

    if [ "$SLOT_TARGET" = "1" ]; then
        kpart="${BLK_DEV}5"; spart="${BLK_DEV}6"
    else
        kpart="${BLK_DEV}7"; spart="${BLK_DEV}8"
    fi

    ksrc="$tmpdir/uImage-powerpc.itb"
    rsrc="$tmpdir/rootfs.sqsh"
    [ -f "$ksrc" ] || fatal "uImage-powerpc.itb not in payload"
    [ -f "$rsrc" ] || fatal "rootfs.sqsh not in payload"

    # Content magic: kernel = FIT (d00dfeed), rootfs = squashfs ("hsqs").
    # Catches a truncated/corrupt installer download before we touch the disk.
    kmagic=$(magic_u32 "$ksrc")
    [ "$kmagic" = "d00dfeed" ] || fatal "kernel payload is not a FIT image (magic=$kmagic, expected d00dfeed)"
    rmagic=$(head -c4 "$rsrc" 2>/dev/null)
    [ "$rmagic" = "hsqs" ] || fatal "rootfs payload is not squashfs (magic='$rmagic', expected 'hsqs')"

    # Capacity: each member must fit its slot partition (skip-with-warning if unknown).
    ksz=$(wc -c < "$ksrc"); rsz=$(wc -c < "$rsrc")
    kcap=$(part_size "/dev/$kpart"); rcap=$(part_size "/dev/$spart")
    if [ "$kcap" -gt 0 ] && [ "$ksz" -gt "$kcap" ]; then fatal "FIT image ($ksz B) exceeds /dev/$kpart capacity ($kcap B)"; fi
    if [ "$rcap" -gt 0 ] && [ "$rsz" -gt "$rcap" ]; then fatal "rootfs ($rsz B) exceeds /dev/$spart capacity ($rcap B)"; fi
    log "  payload OK: kernel $ksz B (cap $kcap), rootfs $rsz B (cap $rcap)"

    log "  Writing FIT image to /dev/$kpart..."
    dd if="$ksrc" of="/dev/$kpart" bs=4k conv=fsync 2>/dev/null
    log "  Writing rootfs to /dev/$spart..."
    dd if="$rsrc" of="/dev/$spart" bs=4k conv=fsync 2>/dev/null
    sync

    # Read-back verify (best-effort: sha256 -> md5 -> skip-with-warning).
    a=$(sha_of "$ksrc"); b=$(sha_head "$ksz" "/dev/$kpart")
    if [ -n "$a" ]; then
        [ "$a" = "$b" ] || fatal "kernel read-back mismatch on /dev/$kpart (write corrupt)"
        log "  kernel verified ($a)"
    else
        log "  WARN: no hash tool in ONIE; skipped kernel read-back verify"
    fi
    a=$(sha_of "$rsrc"); b=$(sha_head "$rsz" "/dev/$spart")
    if [ -n "$a" ]; then
        [ "$a" = "$b" ] || fatal "rootfs read-back mismatch on /dev/$spart (write corrupt)"
        log "  rootfs verified ($a)"
    fi

    rm -rf "$tmpdir"
    log "Slot $SLOT_TARGET populated and verified."
}

# ── Configure U-Boot (Cumulus-style slot chain) ──────────────
#
# boot_active dispatches on cl.active; hw_active1/2 do `usb start; usbiddev;
# usbboot $loadaddr ${usbdev}:<part>` for the chosen slot and `bootm` the FIT.
#
# AUTO-ROLLBACK (this U-Boot has no CONFIG_BOOTCOUNT_LIMIT, so it's scripted):
# nos_bootcmd increments boot_count (saveenv) before booting; if boot_count
# exceeds boot_limit it flips cl.active to the other slot and resets the
# counter, then boots. nos-boot-success.service resets boot_count=0 once edged
# is confirmed up, so a slot that boots Linux but never starts the datapath is
# NOT marked good and auto-rolls-back. If nos_bootcmd ever errors, bootcmd
# falls through to onie_bootcmd (ONIE) — the ultimate backstop.
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
nos_bootcmd setexpr boot_count ${boot_count} + 1; saveenv; if itest ${boot_count} -gt ${boot_limit}; then echo NOS auto-rollback: slot ${cl.active} boot_count ${boot_count} exceeded, switching; if test ${cl.active} = 1; then setenv cl.active 2; else setenv cl.active 1; fi; setenv boot_count 0; saveenv; fi; run boot_active
boot_count 0
boot_limit 3
UBOOT_ENV
    # Delete onie_boot_reason so check_boot_reason falls through to NOS.
    echo "onie_boot_reason" >> "$envfile"

    fw_setenv -f -s "$envfile" 2>/dev/null || log "WARN: fw_setenv failed"
    rm -f "$envfile"

    log "U-Boot configured: cl.active=1, hw_active1/2 wired, auto-rollback (boot_limit=3)."
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
