#!/bin/sh
# fm6000-up.sh - M2 FM6000 bring-up: program the Si5338 refclk, release the SCD
# reset, enumerate the FM6000, load the clean-room DMA kmod, run the diagnostic.
#
# Sequence (mined from EOS NorCal/Si5338 — see notes/analysis/phase18):
#   1. SCD SMBus master -> /dev/i2c-N               (new_smbus_master on scd new_object)
#   2. Si5338 clock program (accel1/bus1, i2c 0x70) (si5338 <bus> <regmap>)
#   3. reset-release: SCD resetGpo 0x4000 bits 1,2  (write 0x6 to clear-reg 0x4010)
#   4. PCI rescan -> 02:00.0 appears
#   5. fm6000dma.ko + fm6000_bringup                (BIST/microcode/SPICO diag)
#
# WITHOUT the Si5338 refclk the FM6000 PCIe link never trains and 02:00.0 never
# enumerates (confirmed live, phase17: reset-release alone left bus 2 empty).
# Proprietary payloads (the .si5338 regmap, FM6000 microcode) are NOT bundled —
# stage them on-box under /usr/share/firmware/. Exploratory: bring-up has
# TODO(live-trace) stubs; the point is to see how far it gets + capture values.
# SPDX-License-Identifier: GPL-2.0-or-later
set -u

# --- tunables (override via env) --------------------------------------------
SMBUS_BASE="${SMBUS_BASE:-0x6000}"    # SCD accel block base (TODO(probe): phase17 saw an
SMBUS_ACCEL="${SMBUS_ACCEL:-1}"       #   SMBus-shaped block ~0x6000; EOS uses accelId=1)
SMBUS_BUSES="${SMBUS_BUSES:-8}"
SI5338_ADDR="${SI5338_ADDR:-0x70}"
# regmap: staged on-box (board data, not vendored). First match wins.
REGMAP="${REGMAP:-}"
for c in /usr/share/firmware/Rosa-Quartzy-0101.si5338 \
         /usr/share/firmware/fm6000/Rosa-Quartzy-0101.si5338 \
         /mnt/flash/Rosa-Quartzy-0101.si5338; do
	[ -z "$REGMAP" ] && [ -f "$c" ] && REGMAP="$c"
done

echo "=================================================================="
echo "  M2 FM6000 bring-up (Si5338 clock -> reset-release -> enumerate)"
echo "=================================================================="

find_no() {   # locate the scd new_object sysfs
	for d in /sys/bus/pci/drivers/scd/0000:* /sys/devices/pci*/*/scd; do
		[ -e "$d/new_object" ] && { echo "$d/new_object"; return 0; }
	done
	return 1
}

echo "--- SCD resetGpo (0x4000) before ---"
scdreg 0x4000

echo "--- FM6000 on the bus before? ---"
if ls /sys/bus/pci/devices/0000:02:00.0 >/dev/null 2>&1; then
	echo "  02:00.0 ALREADY present (skip to bring-up)"
else
	echo "  02:00.0 absent (held in reset / no refclk)"
fi

# --- 1. SCD SMBus master -> /dev/i2c-N --------------------------------------
echo "--- creating SCD SMBus master (accel=$SMBUS_ACCEL base=$SMBUS_BASE) ---"
NO=$(find_no) || echo "  WARN: scd new_object not found (scd+scd-hwmon loaded?)"
if [ -n "${NO:-}" ]; then
	echo "smbus_master $SMBUS_BASE $SMBUS_ACCEL $SMBUS_BUSES" > "$NO" 2>/dev/null \
		&& echo "  registered smbus_master" \
		|| echo "  WARN: smbus_master register failed (base/accel wrong? TODO(probe))"
	sleep 1
fi
echo "  /dev/i2c adapters now:"; ls /dev/i2c-* 2>/dev/null || echo "    (none — need i2c-dev + scd-smbus)"

# --- 2. Si5338 clock program ------------------------------------------------
# Find the bus that has the Si5338 at 0x70. Prefer env SI5338_BUS; else probe.
prog_clock() {
	[ -z "$REGMAP" ] && { echo "  no .si5338 regmap staged — skip clock (stage under /usr/share/firmware/)"; return 1; }
	if [ -n "${SI5338_BUS:-}" ]; then
		echo "  programming Si5338 on i2c-$SI5338_BUS from $REGMAP"
		si5338 "$SI5338_BUS" "$REGMAP" -a "$SI5338_ADDR"; return $?
	fi
	# probe each adapter for an ACK at SI5338_ADDR (i2cdetect if present)
	for b in $(ls /dev/i2c-* 2>/dev/null | sed 's#.*/i2c-##' | sort -n); do
		if command -v i2cdetect >/dev/null 2>&1; then
			i2cdetect -y "$b" 2>/dev/null | grep -qiE " 70 " || continue
		fi
		echo "  trying Si5338 on i2c-$b ..."
		si5338 "$b" "$REGMAP" -a "$SI5338_ADDR" && return 0
	done
	echo "  WARN: could not program Si5338 on any bus (set SI5338_BUS=N to force)"
	return 1
}
echo "--- programming Si5338 refclk ---"
prog_clock || echo "  (continuing — if the box kept EOS's clock programming, refclk may already be live)"

# --- 3. reset-release: SCD resetGpo 0x4000 bits 1,2 (write 0x6 -> 0x4010) ----
echo "--- releasing FM6000 reset (clear bits 1,2 via 0x4010 <= 0x6) ---"
# phase17 live: ours read 0x106, EOS 0x100 -> bits 1,2 = the FM6000 reset.
# ResetGpo: set @+0x00 asserts, clear @+0x10 deasserts (write the bits to clear).
scdreg 0x4000
scdreg 0x4010 0x00000006
sleep 1
echo "  0x4000 after:"; scdreg 0x4000

# --- 4. PCI rescan ----------------------------------------------------------
echo "--- PCI rescan ---"
echo 1 > /sys/bus/pci/rescan 2>/dev/null
sleep 2
if [ -e /sys/bus/pci/devices/0000:02:00.0/vendor ]; then
	echo "  FM6000 ENUMERATED: $(cat /sys/bus/pci/devices/0000:02:00.0/vendor):$(cat /sys/bus/pci/devices/0000:02:00.0/device)"
else
	echo "  FM6000 still absent after rescan."
	echo "    If the clock didn't program, refclk is missing -> link won't train."
	echo "    Retry other reset bits if needed: scdreg 0x4010 0x2 ; scdreg 0x4010 0x4"
	echo "  (M2 bring-up needs 02:00.0; stopping here.)"
	exit 0
fi

# --- 5. DMA kmod + bring-up diagnostic --------------------------------------
echo "--- load fm6000dma.ko (clean-room DMA/MSI backing) ---"
modprobe fm6000dma 2>/dev/null || insmod /lib/modules/*/extra/fm6000dma.ko 2>/dev/null
ls -l /dev/fm6000dma 2>/dev/null && echo "  /dev/fm6000dma up" || echo "  /dev/fm6000dma absent (kmod bind failed)"

echo "--- run fm6000_bringup (BIST/microcode/SPICO; surfaces live-trace values) ---"
echo "  NOTE: parser/FFU + SPICO microcode blobs are NOT bundled (proprietary);"
echo "  stage them at /usr/share/firmware/fm6000/ from the box to go past ucode load."
EDGENOS_FM6000_SLOT=0000:02:00.0 fm6000_bringup 0000:02:00.0 2>&1 | head -40
echo "=== M2 test done ==="
