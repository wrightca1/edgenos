#!/bin/sh
# scd-setup.sh - declare the 7150 SCD board blocks to scd-hwmon.
#
# After `scd` + `scd-hwmon` load, the per-board LEDs / resets / GPIOs / i2c
# masters / SFP cages are created by writing `new_<object> <addr> ...` lines to
# the scd device's sysfs. This encodes the 7150 "raven"/Santa Rosa map.
#
# Confirmed blocks (Phase-3i live + Bodega.py, arista edgenos/SCD.md) are set
# below. The per-cage SMBus/xcvr *addresses* are per-SKU FDL data — the ones
# marked TODO(probe) are filled from a live i2cdetect on first bring-up (the
# normal path; EOS reads them from the compiled raven config).
# SPDX-License-Identifier: GPL-2.0-or-later
set -u

# Find the SCD device's sysfs dir (the node exposing the new_* attributes).
SCD=""
for d in /sys/bus/pci/drivers/scd/0000:* /sys/devices/pci*/*/scd; do
    [ -e "$d/new_led" ] && { SCD="$d"; break; }
done
if [ -z "$SCD" ]; then
    echo "scd-setup: scd-hwmon new_* interface not found (scd loaded?)" >&2
    exit 1
fi
echo "scd-setup: using $SCD"

wr() {  # wr <attr> <line>
    if [ -w "$SCD/$1" ]; then
        echo "$2" > "$SCD/$1" || echo "scd-setup: WARN failed: $1 <= $2" >&2
    fi
}

# ---- Resets (ResetGpo 0x4000; set +0x00 / clear +0x10) --------------------
wr new_reset "0x4000 switch_chip 0"      # FM6000 reset (bit 0)
wr new_reset "0x4000 phy 1"

# ---- Port LEDs (block 0x5010..0x5340 step 0x10) + system LED (0x6940) ------
addr=0x5010
port=1
while [ "$port" -le 52 ]; do
    wr new_led "$addr status$port"
    addr=$(printf "0x%x" $(( addr + 0x10 )))
    port=$(( port + 1 ))
done
wr new_led "0x6940 status_sys"

# ---- Interrupt blocks (0x3000/0x3030/0x3060: maskSet/maskClear/status) -----
# Handled by the scd core at probe from these bases; no new_* line required.

# ---- SMBus/i2c masters ("Pluto" accel blocks) -----------------------------
# new_smbus_master <addr> <accel_id> <bus_count>
# TODO(probe): the raven's SMBus-master base addresses + bus counts come from the
# FDL. Capture with a live i2cdetect during bring-up, then list them here, e.g.:
#   wr new_smbus_master "0x8000 0 8"
echo "scd-setup: NOTE SMBus master addresses are TODO(probe) — see edgenos/PLATFORM.md" >&2

# ---- SFP cages (one scd-xcvr per front-panel port) ------------------------
# new_sfp <addr> <id>   (id = 1..52). Address = per-cage xcvr status reg.
# TODO(probe): per-cage xcvr addresses from FDL/i2cdetect. Once known:
#   id=1; a=0x<base>; while [ $id -le 52 ]; do wr new_sfp "$a $id"; ...; done
echo "scd-setup: NOTE SFP cage (xcvr) addresses are TODO(probe)" >&2

exit 0
