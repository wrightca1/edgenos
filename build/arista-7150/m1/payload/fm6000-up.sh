#!/bin/sh
# fm6000-up.sh - M2 hardware test: release the FM6000 from SCD reset, enumerate it,
# load the clean-room DMA kmod, and run the bring-up diagnostic.
#
# M0 (phase14) proved the FM6000 (8086:155b @ 02:00.0) is HELD IN RESET by the SCD
# until EOS's NorCal init releases it (SCD resetGpo 0x4000). This does that release,
# then rescans PCI and runs our bring-up. Exploratory — the bring-up has TODO(live-
# trace) stubs; the point is to see how far it gets and capture live values.
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
echo "=================================================================="
echo "  M2 FM6000 bring-up test"
echo "=================================================================="

echo "--- SCD present + resetGpo (0x4000) before ---"
scdreg 0x4000

echo "--- FM6000 on the bus before reset-release? ---"
ls /sys/bus/pci/devices/0000:02:00.0 >/dev/null 2>&1 && echo "  02:00.0 ALREADY present" || echo "  02:00.0 absent (held in reset)"

echo "--- releasing FM6000 reset via SCD resetGpo clear (0x4010) ---"
# ResetGpo: set @+0x00 asserts, clear @+0x10 deasserts (write the bit to clear).
# switch_chip reset = bit 0 (per scd-setup). Clear bit0 to bring FM6000 out of reset.
CUR=$(scdreg 0x4000 | sed 's/.*= //')
echo "  current 0x4000 = $CUR ; clearing bit0 via 0x4010"
scdreg 0x4010 0x00000001
sleep 1
scdreg 0x4000

echo "--- PCI rescan ---"
echo 1 > /sys/bus/pci/rescan 2>/dev/null
sleep 2
if [ -e /sys/bus/pci/devices/0000:02:00.0/vendor ]; then
	echo "  FM6000 ENUMERATED: $(cat /sys/bus/pci/devices/0000:02:00.0/vendor):$(cat /sys/bus/pci/devices/0000:02:00.0/device)"
else
	echo "  FM6000 still absent after rescan — reset bit/polarity may differ; try other bits:"
	echo "    scdreg 0x4010 0x2 ; scdreg 0x4010 0x100 ; echo 1 > /sys/bus/pci/rescan"
	echo "  (M2 bring-up needs 02:00.0; stopping here.)"
	exit 0
fi

echo "--- load fm6000dma.ko (clean-room DMA/MSI backing) ---"
modprobe fm6000dma 2>/dev/null || insmod /lib/modules/*/extra/fm6000dma.ko 2>/dev/null
ls -l /dev/fm6000dma 2>/dev/null && echo "  /dev/fm6000dma up" || echo "  /dev/fm6000dma absent (kmod bind failed)"

echo "--- run fm6000_bringup (BIST/microcode/SPICO; surfaces live-trace values) ---"
echo "  NOTE: parser/FFU + SPICO microcode blobs are NOT bundled (proprietary);"
echo "  stage them at /usr/share/firmware/fm6000/ from the box to go past ucode load."
EDGENOS_FM6000_SLOT=0000:02:00.0 fm6000_bringup 0000:02:00.0 2>&1 | head -40
echo "=== M2 test done ==="
