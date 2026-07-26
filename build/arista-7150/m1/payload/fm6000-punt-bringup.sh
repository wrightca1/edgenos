#!/bin/sh
# fm6000-punt-bringup.sh - turnkey, watchdog-SAFE CPU-punt bring-up on M1.
#
# Runs the full proven sequence with the SCD watchdog armed THE WHOLE TIME (a
# background re-armer), so ANY wedge auto-power-cycles to EOS in <=30s instead of
# hard-hanging the box. Learned the hard way 2026-07-26 (see arista memory
# fm6000-bringup-safety). Order:
#   accel#0 -> Cotati clock -> pcie-init(enumerate) -> boot-ctrl+MSB+L2 microcode
#   -> fm6000_l2_probe (minimal, paced CPU-loopback config).
#
# Prereqs staged in /tmp: fm6000reg pcicfg fm6000load fm6000_l2_probe
#   fm6000-pcie-init.sh ucode_l2.raw ; and /usr/share/firmware/Cotati-Clock-0010.si5338.
# Leaves the watchdog ARMED on exit (so a follow-up `fpdma_probe tx 0xff00` is also
# protected). Run `fm6000-punt-bringup.sh disarm` when done to stop it.
# SPDX-License-Identifier: GPL-2.0-or-later
B=0000:02:00.0
PATH=/tmp:/usr/bin:$PATH; export PATH
R(){ fm6000reg $B $1 2>/dev/null | grep -o '[0-9a-f]*$'; }

WDFILE=/tmp/.fm6000_wd_rearmer.pid
if [ "${1:-}" = "disarm" ]; then
    [ -f "$WDFILE" ] && kill "$(cat $WDFILE)" 2>/dev/null; rm -f "$WDFILE"
    scdreg 0x0120 0x0 >/dev/null 2>&1; echo "watchdog disarmed ($(scdreg 0x0120))"; exit 0
fi

echo "== arming SCD watchdog + background re-armer (30s powercycle safety) =="
( while :; do scdreg 0x0120 0xC0000BB8 >/dev/null 2>&1; sleep 10; done ) &
echo $! > "$WDFILE"
sleep 1; echo "  wd=$(scdreg 0x0120 | grep -o '0x[0-9a-f]*$')"

echo "== 1. register SCD accel#0 (creates i2c-10 = FM6000 mgmt slave) =="
NO=""; for d in /sys/bus/pci/drivers/scd/0000:*/new_object; do [ -e "$d" ] && NO="$d"; done
echo "smbus_master 0x8000 0 8" > "$NO" 2>/dev/null && echo "  accel#0 ok" || echo "  (already registered?)"

echo "== 2. Cotati clock (wake switch domain: SCD 0x160 -> 0x2a0000) =="
A=0xFED80000
devmem $(printf 0x%X $((A+0xdbf))) 8 0x01; devmem $(printf 0x%X $((A+0x1bf))) 8 0x40
si5338 1 /usr/share/firmware/Cotati-Clock-0010.si5338 -a 0x70 >/dev/null 2>&1
echo "  SCD 0x160 = $(scdreg 0x160 | grep -o '0x[0-9a-f]*$')  (want 0x002a0000)"

echo "== 3. pre-enum PCIe/SerDes init (enumerate FM6000) =="
sh /tmp/fm6000-pcie-init.sh 2>&1 | tail -2
[ -e /sys/bus/pci/devices/$B/vendor ] || { echo "FM6000 not enumerated - abort"; exit 1; }

echo "== 4. boot-ctrl + MSB release + L2 pipeline microcode =="
V=$(pcicfg $B 0x04 | grep -o '0x[0-9a-f]*$'); pcicfg $B 0x04 $(printf '0x%x' $(( V | 0x6 ))) >/dev/null 2>&1
for c in 2 1 3; do fm6000reg $B 0x1C022 $c >/dev/null 2>&1; sleep 1; done
fm6000reg $B 0x00009 0x0 >/dev/null 2>&1; sleep 1
echo "  SOFT_RESET=0x$(R 0x00009) PLL_STAT=0x$(R 0x1C046)"
fm6000load $B /tmp/ucode_l2.raw
echo "  GLORT_CAM[0]=0x$(R 0x0E000)  (want 0x007fffff, NOT ffffffff)"

echo "== 5. CPU-loopback L2 config (minimal, paced, wedge-aborting) =="
fm6000_l2_probe 2>&1 | grep -E 'programmed|ABORT|state (BEFORE|AFTER)|GLORT_CAM\[8\]|glort_ram0'

echo "== DONE. Watchdog STILL ARMED. Now: fpdma_probe tx 0xff00 ; then this script 'disarm' =="
