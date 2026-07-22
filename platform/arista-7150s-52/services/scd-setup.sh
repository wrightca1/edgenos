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
#   smbus_master <addr> <id> <bus_count>
#   sfp | qsfp   <addr> <id>
# Confirmed SCD blocks: Phase-3i live + phase13/phase14. Per-cage SMBus/xcvr
# addresses are per-SKU FDL data -> TODO(probe) at bring-up.
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
obj "reset 0x4000 switch_chip 0"      # FM6000 reset (bit 0) — deassert to bring FM6000 onto PCIe
obj "reset 0x4000 phy 1"

# ---- Port LEDs (block 0x5010..0x5340 step 0x10) + system LED (0x6940) ------
addr=0x5010
port=1
while [ "$port" -le 52 ]; do
    obj "led $addr status$port 0"     # kind 0 = standard LED
    addr=$(printf "0x%x" $(( addr + 0x10 )))
    port=$(( port + 1 ))
done
obj "led 0x6940 status_sys 0"

# ---- Interrupt blocks (0x3000/0x3030/0x3060) handled by scd core at probe. --

# ---- SMBus/i2c masters ("Pluto" accel blocks): smbus_master <addr> <id> <bus_count>
# TODO(probe): raven SMBus-master base addresses + bus counts (from FDL / i2cdetect).
#   obj "smbus_master 0x<base> 0 8"
echo "scd-setup: NOTE SMBus master addresses TODO(probe) — see edgenos/PLATFORM.md" >&2

# ---- SFP cages: sfp <addr> <id> (id 1..52). TODO(probe) per-cage xcvr addrs. --
echo "scd-setup: NOTE SFP cage (xcvr) addresses TODO(probe)" >&2

exit 0
