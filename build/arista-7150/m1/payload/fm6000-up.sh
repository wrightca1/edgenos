#!/bin/sh
# fm6000-up.sh - M2 FM6000 bring-up (VERIFIED live 2026-07 up to clock-lock).
#
# CORRECT ORDER (critical): the SCD holds the FM6000 in reset at power-on
# (0x4000=0x106). The FM6000 must come OUT of reset with its refclk ALREADY
# present, so program the Si5338 FIRST, then release the reset. If you release
# the reset before the clock, the FM6000 is stuck out-of-reset-without-clock and
# software CANNOT re-assert the reset (the reset bits latch at power-on; writing
# 0x4000 does not set them, and a PCIe secondary-bus-reset does not recover it) -
# only a board power-cycle re-resets it. So run this on a FRESH boot before
# anything releases 0x4000 bits 1,2.
#
# Sequence (all live-verified except the final enumerate, gated by the above):
#   1. SCD SMBus master for accel#1 @ 0x8080  (base 0x8000 + accelId*0x80; EOS
#      Si5338 uses accelId=1 -> 0x8080). Si5338 sits on that master's busId=1.
#   2. raven "Quartzy clock" GPIO-enable: SB700/Sb820 AcpiMmio @ 0xFED80000
#      (== 00:14.0 resource5), write +0xdbf=1 and GPIO191 (+0x1bf)=0x40. Without
#      it the Si5338 PLL will not lock. (EOS Si5338.configure raven path.)
#   3. program the Si5338 (i2c 0x70) with the CORRECT map (si5338, ignoreLos like
#      EOS). Confirm lock: si5338 <bus> -p -> reg6 PLL_LOL=0, LOS_CLKIN=0.
#      *** MAP: our SID=SantaRosaClock -> EOS Si5338.py uses Cotati-Clock-0010
#      (10MHz input), NOT Rosa-Quartzy-0101 (156.25MHz). phase35: the wrong Rosa
#      map still "locks" (LOL=0) but leaves the switch-side domain UNCLOCKED
#      (0x160=0, accel#0 dead) and the FM6000 without a valid refclk. Cotati wakes
#      the domain (0x160=0x2a0000, accel#0=0x10002400) and lets the PCIe link train.
#   4. release FM6000 reset: scdreg 0x4010 0x6  (0x106 -> 0x100).
#   5. PCI rescan -> 02:00.0 appears -> fm6000dma.ko + fm6000_bringup.
# SPDX-License-Identifier: GPL-2.0-or-later
set -u

# phase35 instrumentation: margining-step markers to the console (serial) + kmsg so the
# LAST step before a reboot is visible even though /var/log/fm6000 is tmpfs (wiped on reboot).
cm() { echo "[FM6UP] $*" > /dev/kmsg 2>/dev/null; echo "[FM6UP] $*" > /dev/console 2>/dev/null; echo "[FM6UP] $*"; }

SMBUS_BASE="${SMBUS_BASE:-0x8080}"     # accel#1 (Si5338's) = 0x8000 + 1*0x80
SMBUS_ID="${SMBUS_ID:-1}"
SMBUS_BUSES="${SMBUS_BUSES:-8}"
SI5338_ADDR="${SI5338_ADDR:-0x70}"
ACPIMMIO="${ACPIMMIO:-0xFED80000}"     # SB700/Sb820 AcpiMmio (00:14.0 resource5)
# phase35: SantaRosaClock -> Cotati-Clock-0010 (correct). Rosa-Quartzy-0101 is the
# WRONG map for this SKU (it's for SID "Rosa*Quartzy"); prefer Cotati, fall back only
# if Cotati isn't staged. Override with REGMAP=... if a future SID needs Rosa.
REGMAP="${REGMAP:-}"
for c in /usr/share/firmware/Cotati-Clock-0010.si5338 \
         /usr/share/firmware/fm6000/Cotati-Clock-0010.si5338 \
         /mnt/flash/Cotati-Clock-0010.si5338 \
         /usr/share/firmware/Rosa-Quartzy-0101.si5338 \
         /mnt/flash/Rosa-Quartzy-0101.si5338; do
	[ -z "$REGMAP" ] && [ -f "$c" ] && REGMAP="$c"
done
case "$REGMAP" in *Rosa-Quartzy*) echo "WARN: falling back to Rosa-Quartzy map - Cotati-Clock-0010 not staged (wrong clock tree for SantaRosaClock!)";; esac

echo "=================================================================="
echo "  M2 FM6000 bring-up (clock FIRST, then reset-release)"
echo "=================================================================="
echo "--- SCD resetGpo 0x4000 (expect 0x106 = held in reset on a fresh boot) ---"
scdreg 0x4000

# --- 1. SCD SMBus master (accel#1) ------------------------------------------
NO=""
for d in /sys/bus/pci/drivers/scd/0000:*/new_object; do [ -e "$d" ] && NO="$d"; done
if [ -n "$NO" ]; then
	echo "smbus_master $SMBUS_BASE $SMBUS_ID $SMBUS_BUSES" > "$NO" 2>/dev/null \
		&& echo "smbus_master @$SMBUS_BASE registered" || echo "WARN: smbus_master register failed"
	sleep 1
fi

# --- 2. raven Quartzy GPIO-enable (SB700 AcpiMmio) --------------------------
if command -v devmem >/dev/null 2>&1; then
	echo "--- GPIO-enable Quartzy clock via AcpiMmio $ACPIMMIO ---"
	sig=$(devmem $ACPIMMIO 32 2>/dev/null)
	echo "  AcpiMmio sig (expect 0x43851002 = Sb820): $sig"
	devmem $(printf '0x%X' $(( $ACPIMMIO + 0xdbf ))) 8 0x01
	devmem $(printf '0x%X' $(( $ACPIMMIO + 0x1bf ))) 8 0x40
	echo "  GPIO191 now: $(devmem $(printf '0x%X' $(( $ACPIMMIO + 0x1bf ))) 8 2>/dev/null)"
else
	echo "WARN: no devmem - cannot enable Quartzy clock; Si5338 PLL will not lock"
fi

# --- 3. program + verify Si5338 ---------------------------------------------
BUS=""
for b in $(ls /dev/i2c-* 2>/dev/null | sed 's#.*/i2c-##' | sort -n); do
	si5338 "$b" -p -a "$SI5338_ADDR" 2>/dev/null | grep -q ' ACK ' && { BUS="$b"; break; }
done
if [ -z "$BUS" ]; then
	echo "ERROR: no Si5338 (0x$SI5338_ADDR) found on any i2c bus - check smbus_master base"
	exit 1
fi
echo "--- Si5338 found on i2c-$BUS; programming Rosa-Quartzy map ---"
[ -n "$REGMAP" ] || { echo "ERROR: no .si5338 regmap staged (/usr/share/firmware/)"; exit 1; }
si5338 "$BUS" "$REGMAP" -a "$SI5338_ADDR"
echo "--- clock status (want PLL_LOL=0 LOS_CLKIN=0) ---"
si5338 "$BUS" -p -a "$SI5338_ADDR"

# --- 3.5. margin FM6000 core VDD_ALTA 1.057V -> 1.2V (phase35, chip IN RESET) --
# The FM6000 boots undervolted (Chl822X NVM default = 1.057V); at 1.057V the PCIe
# link trains but never completes (DLLLA stays 0). EOS margins to AltaVdd=1.2V via
# the Chl822X VID registers AT BOOT with the chip held in reset. Doing it on a live
# (out-of-reset) rail power-cycles the board (phase35), so it MUST be here, before
# the reset release. The Cotati clock above woke the switch-side domain, so accel#0
# (the VRM) is reachable now. EOS-exact order (Chl822x.py:1729-1744 initialize()):
# write loop1Vid (0x8F=1.2V to selects 1/2/3, 0x69=0.96V min to select 0) THEN
# enable VID mode (gpuDvid 0xCE bit0=1); the regulator slews 1.057->1.2V.
if [ "${FM6000_MARGIN:-1}" = "1" ]; then
	echo "--- margin FM6000 core -> 1.2V (UCD parked + Chl822X VID; chip in reset) ---"
	echo "smbus_master 0x8000 0 8" > "$NO" 2>/dev/null; sleep 1   # register accel#0 (VRM+UCD)
	CHLB=""; UCDB=""
	for a in /sys/class/i2c-dev/i2c-*; do n=$(cat "$a/name" 2>/dev/null); b=$(basename "$a"|sed s/i2c-//)
		case "$n" in *"master 0 bus 3") CHLB=$b;; *"master 0 bus 5") UCDB=$b;; esac
	done
	if [ -n "$CHLB" ] && [ -n "$UCDB" ]; then
		echo "$CHLB 0x70 3 3 3 0" > "$(dirname "$NO")/smbus_tweaks" 2>/dev/null
		echo "$UCDB 0x4e 3 3 3 0" > "$(dirname "$NO")/smbus_tweaks" 2>/dev/null; sleep 1
		# Each cm "Sn ..." prints to the serial BEFORE its i2c op, so the LAST Sn on the console
		# = the step in progress when the box rebooted. S3/S9/S12 also read VOUT.
		cm "S0 start Chl=i2c-$CHLB UCD=i2c-$UCDB VOUT=$(i2cget -y $CHLB 0x70 0x8b w 2>/dev/null)"
		# EOS parks the UCD Alta rails first (Chl822XConfigTool.py:528-536) before the VID change.
		cm "S1 parkUCD p8 OP=0x40"; i2cset -y $UCDB 0x4e 0x00 0x08 2>/dev/null; sleep 0.15; i2cset -y $UCDB 0x4e 0x01 0x40 2>/dev/null; sleep 0.15
		cm "S2 parkUCD p9 OP=0x40"; i2cset -y $UCDB 0x4e 0x00 0x09 2>/dev/null; sleep 0.15; i2cset -y $UCDB 0x4e 0x01 0x40 2>/dev/null; sleep 0.6
		cm "S3 UCDparked VOUT=$(i2cget -y $CHLB 0x70 0x8b w 2>/dev/null)"
		cm "S4 VID C6=0x69"; i2cset -y $CHLB 0x70 0xD5 0x69C6 w 2>/dev/null; sleep 0.2
		cm "S5 VID C7=0x8F"; i2cset -y $CHLB 0x70 0xD5 0x8FC7 w 2>/dev/null; sleep 0.2
		cm "S6 VID C8=0x8F"; i2cset -y $CHLB 0x70 0xD5 0x8FC8 w 2>/dev/null; sleep 0.2
		cm "S7 VID C9=0x8F"; i2cset -y $CHLB 0x70 0xD5 0x8FC9 w 2>/dev/null; sleep 0.2
		cm "S8 enableVID 0xCE=1"; i2cset -y $CHLB 0x70 0xD5 0x01CE w 2>/dev/null; sleep 0.6
		cm "S9 VIDenabled VOUT=$(i2cget -y $CHLB 0x70 0x8b w 2>/dev/null)"
		cm "S10 restoreUCD p8 OP=0x80"; i2cset -y $UCDB 0x4e 0x00 0x08 2>/dev/null; sleep 0.15; i2cset -y $UCDB 0x4e 0x01 0x80 2>/dev/null; sleep 0.15
		cm "S11 restoreUCD p9 OP=0x80"; i2cset -y $UCDB 0x4e 0x00 0x09 2>/dev/null; sleep 0.15; i2cset -y $UCDB 0x4e 0x01 0x80 2>/dev/null; sleep 0.6
		cm "S12 done VOUT=$(i2cget -y $CHLB 0x70 0x8b w 2>/dev/null)"
	else
		echo "  WARN: Chl822X(bus3)/UCD(bus5) not both found (CHLB=$CHLB UCDB=$UCDB) - skip margin, FM6000 stays 1.057V"
	fi
fi

# --- 4. release FM6000 reset (AFTER clock is up) ----------------------------
echo "--- releasing FM6000 reset (0x4010 <= 0x6) ---"
scdreg 0x4010 0x00000006
sleep 1
echo "  0x4000 now: $(scdreg 0x4000 | grep -o '0x[0-9a-f]*$')  (expect 0x100)"

# --- 5. enumerate -----------------------------------------------------------
echo "--- PCI rescan ---"
echo 1 > /sys/bus/pci/rescan 2>/dev/null
sleep 3
# phase31: report DLLLA on the root port regardless of outcome (EOS enumerates via
# the kernel `pcielw` driver on DLLLA=Link-Active; M1 has none, so we read it by hand).
echo "--- root-port 00:04.0 link (DLLLA bit13 = is the FM6000 driving PCIe?) ---"
pcicfg 0000:00:04.0 link 2>/dev/null
if [ -e /sys/bus/pci/devices/0000:02:00.0/vendor ]; then
	echo "*** FM6000 ENUMERATED: $(cat /sys/bus/pci/devices/0000:02:00.0/vendor):$(cat /sys/bus/pci/devices/0000:02:00.0/device) ***"
else
	echo "FM6000 still absent - running DLLLA diagnosis (phase31) to say WHY:"
	DIR=$(cd "$(dirname "$0")" && pwd)
	sh "$DIR/dllla-check.sh" 2>/dev/null || dllla-check.sh 2>/dev/null
	echo "  -> DLLLA=1: link is up, enum gap is PCI-resource. Try: dllla-check.sh fix"
	echo "             (widen 00:04.0 window + rescan) or boot with 'pci=realloc pci=hpmemsize=32M'."
	echo "  -> DLLLA=0: chip not driving PCIe. If reset was released earlier this boot the FM6000 is"
	echo "             stuck (needs power-cycle); else it's a chip-boot problem (clock/voltage/reset)."
	exit 0
fi

echo "--- fm6000dma.ko + fm6000_bringup ---"
modprobe fm6000dma 2>/dev/null || insmod /lib/modules/*/extra/fm6000dma.ko 2>/dev/null
EDGENOS_FM6000_SLOT=0000:02:00.0 fm6000_bringup 0000:02:00.0 2>&1 | head -40
echo "=== M2 bring-up done ==="
