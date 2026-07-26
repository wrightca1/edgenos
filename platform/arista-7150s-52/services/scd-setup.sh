#!/bin/sh
# scd-setup.sh - declare the 7150 SCD board blocks to scd-hwmon.
#
# After `scd` + `scd-hwmon` load, per-board LEDs/resets/i2c-masters/SFP-cages are
# created by writing type-prefixed lines to the SINGLE `new_object` sysfs file on
# the scd PCI device. VERIFIED LIVE on the 7150 (2026-07-21, M1 boot): the driver
# exposes `new_object` (not per-type new_led/new_reset files), and the LED format
# needs a 3rd `kind` field.  Grammar (field order = the driver's PARSE_ calls):
#   led          <addr> <name> <kind>
#   reset        <addr> <name> <bitpos>
#   smbus_master <addr> <id> [bus_count]      (bus_count optional, driver default 8)
#   sfp | qsfp   <addr> <id>
#
# Board topology is authoritative from Arista's own FDL for this SKU:
#   /usr/share/NorCal/SantaRosaP4.fdl (== the 7150S-52; P1..P5 identical base board).
# Cross-checked against the live SCD register dump (notes/reference/scd-dumps).
#   - SFP xcvr cages: base 0x5010, stride 0x10, Ethernet1..52 -> 0x5010..0x5340
#     (FDL altaSfpPorts xcvrOffset column; live dump: 0x5010=0x180 laser-on Et1,
#      empty cages read 0x47 -> txdisable bit6 set). scd-xcvr exposes bit6=txdisable.
#   - Per-port LEDs: base 0x60D0, stride 0x10 (FDL line 111 ledAddr=0x60D0+0x10*(N-1)).
#   - SMBus "Pluto" accel masters: FDL scdSmbusAccelAddrAndIntrBits (index 2 absent).
#   - resetGpo1 @0x4000: FM6000/Alta reset = bit 1 (FDL alta.reset=newBit(1));
#     security(SOL) reset = bit 2. phyTxEnable is a SEPARATE block @0x4100 (dataplane
#     per-port TX enable, handled during EPL/port bring-up, not here).
# SPDX-License-Identifier: GPL-2.0-or-later
set -u

# Find the scd PCI device's new_object attribute.
NO=""
for d in /sys/bus/pci/drivers/scd/0000:* /sys/devices/pci*/*/scd; do
    [ -e "$d/new_object" ] && { NO="$d/new_object"; break; }
done
if [ -z "$NO" ]; then
    echo "scd-setup: scd new_object interface not found (scd+scd-hwmon loaded?)" >&2
    exit 1
fi
echo "scd-setup: using $NO"

obj() {  # obj "<type> <args...>"
    if echo "$1" > "$NO" 2>/dev/null; then
        return 0
    else
        echo "scd-setup: WARN rejected: $1" >&2
    fi
}

# ---- Resets (ResetGpo 0x4000; set +0x00 / clear +0x10) --------------------
# FDL: alta(FM6000).reset = resetGpo1.newBit(1); sol(security).reset = newBit(2).
# Declaring a reset only creates the control node (default = current reg state);
# it does NOT assert. The FM6000 already enumerates on boot (reset deasserted).
obj "reset 0x4000 switch_chip 1"      # FM6000/Alta reset (bit 1)
obj "reset 0x4000 security 2"         # SOL security chip reset (bit 2)

# ---- SMBus/i2c masters ("Pluto" accel blocks): smbus_master <addr> <id> <bus_count>
# FDL scdSmbusAccelAddrAndIntrBits (accel index 2 does not exist on SantaRosa base
# board). bus_count = buses actually wired per master (general/psu + the SFP DOM/i2c
# fan-out). These drive the SFP EEPROM/DOM + sensor/PSU reads; the SFP *laser* itself
# is the scd-xcvr txdisable bit, which does NOT need these masters.
obj "smbus_master 0x8000 0 7"         # general/cpu/switch/IR-pwr/psu1/pwr-ctrl/security
obj "smbus_master 0x8080 1 2"         # psu0 + Si5338 refclk osc
#   (accel id 2 absent)
obj "smbus_master 0x8100 3 8"         # SFP Ethernet1..8
obj "smbus_master 0x8180 4 8"         # SFP Ethernet9..16
obj "smbus_master 0x8200 5 8"         # SFP Ethernet17..24
obj "smbus_master 0x8280 6 2"         # SFP Ethernet25..26
obj "smbus_master 0x8300 7 7"         # SFP Ethernet27..33
obj "smbus_master 0x8380 8 7"         # SFP Ethernet34..40
obj "smbus_master 0x8400 9 7"         # SFP Ethernet41..47
obj "smbus_master 0x8480 10 5"        # SFP Ethernet48..52

# ---- SFP+ transceiver cages: sfp <addr> <id> (id = Ethernet<id>, 1..52) ----
# Base 0x5010, stride 0x10. Creates sfp<id>_{present,rxlos,txfault,txdisable} nodes
# on the scd PCI device (bit6=txdisable, RW). sfp-enable.sh clears txdisable -> laser on.
addr=0x5010
port=1
while [ "$port" -le 52 ]; do
    obj "sfp $addr $port"
    addr=$(printf "0x%x" $(( addr + 0x10 )))
    port=$(( port + 1 ))
done

# ---- Port LEDs (block 0x60D0..0x6400 step 0x10) + status LED (0x6050) -------
# (0x5010.. is the XCVR block above, NOT LEDs — that was the prior bug.)
addr=0x60d0
port=1
while [ "$port" -le 52 ]; do
    obj "led $addr status$port 0"     # kind 0 = standard LED
    addr=$(printf "0x%x" $(( addr + 0x10 )))
    port=$(( port + 1 ))
done
obj "led 0x6050 status_sys 0"         # FDL: Status LED @0x6050

# ---- Interrupt blocks (0x3000/0x3030/0x3060/0x3090) handled by scd core at probe. --

exit 0
