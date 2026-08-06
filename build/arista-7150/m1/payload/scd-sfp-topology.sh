#!/bin/sh
# scd-sfp-topology.sh — configure the sonic scd-hwmon driver for the DCS-7150S-52 ("SantaRosa") SFP topology.
# Extracted from EOS SantaRosaP2.fdl. Gives EdgeNOS full access to all 52 SFP+ cages:
#   - EEPROM (media type / DOM) via i2c at 0x50 on accel 3-10 buses
#   - presence / rxlos / txfault / txdisable / rate_select via SCD xcvr control regs 0x5010..0x5340
#
# Usage: sh scd-sfp-topology.sh            (writes topology to the scd driver's new_object)
# Prereq: sonic scd-hwmon module loaded; find its sysfs dir (the PCI dev 0000:04:00.0 / scd).
set -u
# locate the scd new_object sysfs node
NO=""
for c in /sys/bus/pci/devices/0000:04:00.0/new_object \
         /sys/module/scd*/drivers/*/0000:04:00.0/new_object \
         $(find /sys -name new_object 2>/dev/null | grep -i scd | head -1); do
  [ -w "$c" ] && { NO="$c"; break; }
done
[ -z "$NO" ] && { echo "ERR: scd new_object sysfs not found (is scd-hwmon loaded?)"; exit 1; }
echo "[scd-sfp] using $NO"

emit(){ echo "$1" > "$NO" 2>/dev/null && echo "  + $1" || echo "  ! FAILED: $1"; }

# --- SMBus masters (base, accel_id, bus_count) — SantaRosaP2.fdl:117 ---
# accel 0/1 = board mgmt (temp/psu/cpld/Si5338); 3-10 = SFP EEPROM channels.
emit "new_smbus_master 0x8000 0 8"
emit "new_smbus_master 0x8080 1 8"
emit "new_smbus_master 0x8100 3 8"    # SFP  1- 8  bus0-7
emit "new_smbus_master 0x8180 4 8"    # SFP  9-16  bus0-7
emit "new_smbus_master 0x8200 5 8"    # SFP 17-24  bus0-7
emit "new_smbus_master 0x8280 6 8"    # SFP 25-26  bus0-1
emit "new_smbus_master 0x8300 7 8"    # SFP 27-33  bus0-6
emit "new_smbus_master 0x8380 8 8"    # SFP 34-40  bus0-6
emit "new_smbus_master 0x8400 9 8"    # SFP 41-47  bus0-6
emit "new_smbus_master 0x8480 10 8"   # SFP 48-52  bus0-4

# --- SFP+ control/status regs: addr = 0x5000 + id*0x10 (ports 1..52) ---
id=1
while [ $id -le 52 ]; do
  printf -v _a '0x%X' $((0x5000 + id*0x10)) 2>/dev/null || _a=$(printf '0x%X' $((0x5000 + id*0x10)))
  emit "new_sfp $_a $id"
  id=$((id+1))
done

echo "[scd-sfp] done. SFP control at scd sysfs sfp<id>_{present,rxlos,txfault,txdisable,rate_select0/1}"
echo "[scd-sfp] EEPROM: bind optoe2 at 0x50 on each SFP bus, e.g.:"
echo "    echo optoe2 0x50 > /sys/bus/i2c/devices/i2c-<N>/new_device"
echo "[scd-sfp] port->(accel,bus): 1-8=a3 b0-7, 9-16=a4, 17-24=a5, 25-26=a6 b0-1,"
echo "    27-33=a7 b0-6, 34-40=a8, 41-47=a9, 48-52=a10 b0-4"
