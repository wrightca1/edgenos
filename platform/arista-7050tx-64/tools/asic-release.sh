#!/bin/busybox sh
# asic-release.sh -- take the BCM56855 out of reset via the SCD, on EdgeNOS.
#
# WHY THIS IS SAFE TO RUN, and why the target value is not a guess:
#
#   EOS running   (ASIC enumerates at 0000:01:00.0)   0xf6004000 = 0x00000000
#   EdgeNOS       (ASIC absent from PCI)              0xf6004000 = 0x000001FF
#
# So the end state we are driving to is byte-for-byte the value the box runs
# with under EOS. /etc/fdl names the bits:
#
#   resetGpoBlock[0] = scd.newSetClearGpoBlock( "resetGpo0", 0x4000 )
#   trident.reset    = resetGpoBlock[0].newBit( 0 )     <- the BCM56855
#   sol.reset        = resetGpoBlock[0].newBit( 1 )     <- HW entropy/security chip
#   ds100Kr.resetGpo = resetGpoBlock[0].newBit( 8 )     <- DS100KR800 retimer
#
# and the 7050SX2's scdreset documents the polarity as "bit set = reset
# ASSERTED", which matches: 0x1FF is everything held down.
#
# The block is a SET/CLEAR type. By analogy with the interrupt block -- whose
# layout scd.ko's own sysfs gives us as set=+0x00, clear=+0x10, status=+0x20 --
# the clear alias should be +0x10. That is the ONE thing here that is inferred
# rather than observed, so this script tries it first, verifies by read-back,
# and only then falls back to treating 0x4000 as a plain read/write register.
#
# Recovery if anything goes wrong: reboot. The SCD reasserts all resets at
# power-on, which is exactly why EdgeNOS sees 0x1FF in the first place.
#
# Usage:  asic-release.sh              report state only, write nothing
#         asic-release.sh --release    deassert bit 0 (the ASIC) only
#         asic-release.sh --release-all  deassert all nine, matching EOS (0x00)
#
# --release-all also frees bit 8, the DS100KR800 retimer that the QSFP ports
# need, and bit 1, the SOL hardware-entropy chip. EOS runs with 0x00.
set -u

SCD=0xf6000000
RESET=0xf6004000          # resetGpo0: read / set
CLEAR=0xf6004010          # candidate clear alias
ASIC=0000:01:00.0
BRIDGE=0000:00:02.1
TRIDENT_BIT=0x1

rd() { busybox devmem "$1" 32; }

banner() { echo; echo "=== $* ==="; }

banner "identity check"
# Refuse to touch anything unless this really is the SCD we think it is.
VEN=$(cat /sys/bus/pci/devices/0000:02:00.0/vendor 2>/dev/null)
DEV=$(cat /sys/bus/pci/devices/0000:02:00.0/device 2>/dev/null)
echo "  SCD at 0000:02:00.0 = $VEN:$DEV (want 0x3475:0x0001)"
if [ "$VEN" != "0x3475" ] || [ "$DEV" != "0x0001" ]; then
    echo "  ** NOT the expected SCD -- refusing to write"; exit 1
fi

banner "before"
echo "  resetGpo0 (0x4000) = $(rd $RESET)"
echo "  bridge link width  = $(cat /sys/bus/pci/devices/$BRIDGE/current_link_width 2>/dev/null)"
if [ -d /sys/bus/pci/devices/$ASIC ]; then
    echo "  ASIC               = PRESENT ($(cat /sys/bus/pci/devices/$ASIC/vendor):$(cat /sys/bus/pci/devices/$ASIC/device))"
else
    echo "  ASIC               = absent"
fi

case "${1:-}" in
    --release)     WANT_BITS=$TRIDENT_BIT; WHAT="bit 0 (ASIC)" ;;
    --release-all) WANT_BITS=0x1FF;        WHAT="all nine (matches EOS)" ;;
    *) echo; echo "report only; pass --release or --release-all"; exit 0 ;;
esac

BEFORE=$(rd $RESET)
case "$BEFORE" in
    0x000001FF|0x000001ff) ;;
    0x00000000) echo; echo "already released ($BEFORE), nothing to do"; exit 0 ;;
    *) echo; echo "  ** unexpected starting value $BEFORE -- refusing to write"; exit 1 ;;
esac

banner "attempt 1: write $WANT_BITS to the clear alias 0x4010 -- $WHAT"
busybox devmem $CLEAR 32 $WANT_BITS
sleep 1
AFTER=$(rd $RESET)
echo "  resetGpo0 now = $AFTER (was $BEFORE)"

if [ "$AFTER" = "$BEFORE" ]; then
    banner "attempt 2: 0x4010 did nothing -- treat 0x4000 as plain read/write"
    # Clear only bit 0, leave the other eight asserted.
    busybox devmem $RESET 32 $(( 0x1FF & ~WANT_BITS ))
    sleep 1
    AFTER=$(rd $RESET)
    echo "  resetGpo0 now = $AFTER (was $BEFORE)"
fi

if [ "$AFTER" = "$BEFORE" ]; then
    echo
    echo "  ** neither write changed the register. Not escalating further."
    echo "  ** The register may be write-protected -- /etc/fdl has a write"
    echo "  ** protect block at 0x1000 that we have not investigated."
    exit 1
fi

banner "PCIe rescan"
echo 1 > /sys/bus/pci/rescan 2>/dev/null
sleep 3

# Enumerating is not enough to reach the registers. With no driver bound, the
# device's PCI COMMAND register is 0x0000 -- memory decoding OFF -- and every
# BAR read returns 0xFFFFFFFF, which looks exactly like a dead chip. Let the
# kernel enable it (this is what pci_enable_device does).
#
# BUS MASTERING MUST BE SET TOO. This script used to leave it off, with the
# comment "correct until something wants DMA". The SDK wants DMA: the packet
# TX/RX rings and the SBUS DMA that carries most of chip init are all the ASIC
# mastering reads and writes into our reserved pool. With COMMAND=0x0002 the
# device cannot initiate any of it. `echo 1 > enable` does NOT set the bit --
# pci_enable_device and pci_set_master are separate calls -- so write it into
# config space directly.
if [ -d /sys/bus/pci/devices/$ASIC ]; then
    banner "enabling memory decoding + bus mastering"
    echo 1 > /sys/bus/pci/devices/$ASIC/enable 2>/dev/null
    # bit1 = memory space, bit2 = bus master
    printf '\006\000' | dd of=/sys/bus/pci/devices/$ASIC/config bs=1 seek=4 \
        count=2 conv=notrunc 2>/dev/null
    CMD=$(dd if=/sys/bus/pci/devices/$ASIC/config bs=1 count=2 skip=4 2>/dev/null \
          | hexdump -e '1/2 "0x%04x"')
    echo "  PCI COMMAND = $CMD  (bit1 = memory space, bit2 = bus master)"
    case "$CMD" in
        *0006|*0007) echo "  ** bus mastering ON -- the ASIC can DMA **" ;;
        *) echo "  ** bus mastering NOT set -- DMA will not work" ;;
    esac

    # Prove the register bus really answers. CMIC_DEV_REV_ID is at BAR0+0x10224
    # (include/soc/mcm/cmicm.h:5671) and encodes rev<<16 | device.
    REV=$(busybox devmem 0xf4010224 32)
    echo "  CMIC_DEV_REV_ID = $REV  (expect 0x0003B855 = BCM56855_A2)"
    case "$REV" in
        0x0003B855|0x0003b855) echo "  ** CMIC is alive **" ;;
        0xFFFFFFFF|0xffffffff) echo "  ** no response -- memory decoding still off?" ;;
        *) echo "  ** unexpected id" ;;
    esac
fi

banner "after"
echo "  resetGpo0 (0x4000) = $(rd $RESET)"
echo "  bridge link width  = $(cat /sys/bus/pci/devices/$BRIDGE/current_link_width 2>/dev/null)"
echo "  bridge link speed  = $(cat /sys/bus/pci/devices/$BRIDGE/current_link_speed 2>/dev/null)"
if [ -d /sys/bus/pci/devices/$ASIC ]; then
    echo "  ASIC               = PRESENT $(cat /sys/bus/pci/devices/$ASIC/vendor):$(cat /sys/bus/pci/devices/$ASIC/device)"
    echo "  ASIC BARs:"
    head -3 /sys/bus/pci/devices/$ASIC/resource | sed 's/^/    /'
else
    echo "  ASIC               = STILL ABSENT"
fi
echo
echo "bus 01 devices: $(ls /sys/bus/pci/devices/ | grep '^0000:01:' | tr '\n' ' ')"
