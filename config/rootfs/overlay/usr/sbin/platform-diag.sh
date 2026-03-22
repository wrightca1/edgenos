#!/bin/bash
# platform-diag.sh - AS5610-52X Platform Diagnostics
#
# Reads all sensors, SFP presence, PSU status, fan speed, and temperatures.
# Run as root.

# CPLD is memory-mapped at 0xEA000000 via P2020 eLBC CS1
CPLD_BASE=0xEA000000

# I2C bus definitions
BUS_MGMT=0          # PCA9548 mux at 0x70 → buses 2-9
BUS_PSU1=3           # PSU1 CPR-4011 at 0x3E (mux ch 0x02)
BUS_PSU2=4           # PSU2 CPR-4011 at 0x3D (mux ch 0x04)
BUS_THERMAL=9        # MAX6697 at 0x4D, NE1617A at 0x18 (mux ch 0x80)
BUS_SFP_GPIO=65      # PCA9506 GPIO expanders at 0x20-0x24
BUS_QSFP_GPIO=64     # PCA9538 GPIO at 0x70-0x73

# ── CPLD ──────────────────────────────────────────────────────
cpld_read() {
    devmem $((CPLD_BASE + $1)) 8 2>/dev/null
}

echo "========================================="
echo "  AS5610-52X Platform Diagnostics"
echo "========================================="
echo ""

# ── CPLD Status ──────────────────────────────────────────────
echo "── CPLD Status ──"
if [ -c /dev/mem ]; then
    psu2_st=$(cpld_read 0x01)
    psu1_st=$(cpld_read 0x02)
    sys_st=$(cpld_read 0x03)
    fan_speed=$(cpld_read 0x0D)

    if [ -n "$psu1_st" ]; then
        psu1_present=$(( (psu1_st & 0x01) == 0 ))
        psu1_dc_ok=$(( (psu1_st & 0x02) != 0 ))
        echo "  PSU1: present=$psu1_present dc_ok=$psu1_dc_ok (raw=$psu1_st)"
    fi
    if [ -n "$psu2_st" ]; then
        psu2_present=$(( (psu2_st & 0x01) == 0 ))
        psu2_dc_ok=$(( (psu2_st & 0x02) != 0 ))
        echo "  PSU2: present=$psu2_present dc_ok=$psu2_dc_ok (raw=$psu2_st)"
    fi
    if [ -n "$sys_st" ]; then
        fan_present=$(( (sys_st & 0x04) == 0 ))
        fan_fail=$(( (sys_st & 0x08) != 0 ))
        echo "  Fan: present=$fan_present fail=$fan_fail (raw=$sys_st)"
    fi
    if [ -n "$fan_speed" ]; then
        duty=$(( fan_speed * 325 / 100 ))
        echo "  Fan speed: raw=$fan_speed duty=${duty}%"
    fi
else
    echo "  /dev/mem not available (need root)"
fi
echo ""

# ── Temperature Sensors ──────────────────────────────────────
echo "── Temperature Sensors ──"
# MAX6697 on bus 9 at 0x4D
for i in 1 2 3 4 5 6 7; do
    f="/sys/bus/i2c/devices/9-004d/hwmon/hwmon0/temp${i}_input"
    if [ -f "$f" ]; then
        val=$(cat "$f")
        echo "  MAX6697 ch$i: $((val / 1000)).$((val % 1000 / 100))°C"
    fi
done

# NE1617A on bus 9 at 0x18
ne_local=$(i2cget -y 9 0x18 0x00 2>/dev/null)
ne_remote=$(i2cget -y 9 0x18 0x01 2>/dev/null)
[ -n "$ne_local" ] && echo "  NE1617A local: ${ne_local}°C"
[ -n "$ne_remote" ] && echo "  NE1617A remote: ${ne_remote}°C"
echo ""

# ── PSU Status (PMBus) ──────────────────────────────────────
echo "── PSU Status ──"
for psu in 1 2; do
    if [ "$psu" = "1" ]; then bus=$BUS_PSU1; addr=0x3e; else bus=$BUS_PSU2; addr=0x3d; fi
    vin=$(i2cget -y $bus $addr 0x88 w 2>/dev/null)
    if [ -n "$vin" ] && [ "$vin" != "0x0000" ]; then
        vout=$(i2cget -y $bus $addr 0x8b w 2>/dev/null)
        iin=$(i2cget -y $bus $addr 0x89 w 2>/dev/null)
        iout=$(i2cget -y $bus $addr 0x8c w 2>/dev/null)
        temp=$(i2cget -y $bus $addr 0x8d w 2>/dev/null)
        echo "  PSU${psu}: VIN=$vin VOUT=$vout IIN=$iin IOUT=$iout TEMP=$temp"
    else
        echo "  PSU${psu}: not present or standby"
    fi
done
echo ""

# ── SFP Presence ─────────────────────────────────────────────
echo "── SFP/QSFP Presence ──"
# PCA9506 at bus 65: 0x20 has ports 1-40, 0x23 has ports 41-48, 0x24 has QSFP
present_ports=""
for port_group in 0 1 2 3 4; do
    reg_val=$(i2cget -y $BUS_SFP_GPIO 0x20 $port_group 2>/dev/null)
    if [ -n "$reg_val" ]; then
        val=$((reg_val))
        for bit in 0 1 2 3 4 5 6 7; do
            port=$((port_group * 8 + bit + 1))
            [ $port -gt 40 ] && break
            if [ $((val & (1 << bit))) -eq 0 ]; then
                present_ports="$present_ports $port"
            fi
        done
    fi
done

# Ports 41-48 from 0x23
reg_val=$(i2cget -y $BUS_SFP_GPIO 0x23 0x00 2>/dev/null)
if [ -n "$reg_val" ]; then
    val=$((reg_val))
    for bit in 0 1 2 3 4 5 6 7; do
        port=$((40 + bit + 1))
        if [ $((val & (1 << bit))) -eq 0 ]; then
            present_ports="$present_ports $port"
        fi
    done
fi

# QSFP 49-52 from 0x24
qsfp_val=$(i2cget -y $BUS_SFP_GPIO 0x24 0x00 2>/dev/null)
if [ -n "$qsfp_val" ]; then
    val=$((qsfp_val))
    for bit in 0 1 2 3; do
        port=$((48 + bit + 1))
        if [ $((val & (1 << bit))) -eq 0 ]; then
            present_ports="$present_ports $port"
        fi
    done
fi

echo "  Present:$present_ports"
echo ""

# ── SFP Module Info ──────────────────────────────────────────
echo "── SFP Module Details ──"
# Bus mapping: port N → bus (10 + ceil(N/8)*9 + ((N-1)%8) + 1)
# Actually the mapping is complex due to mux tree. Use known mapping:
# Ports 1-8:   buses 11-18
# Ports 9-16:  buses 20-27
# Ports 17-24: buses 29-36
# Ports 25-32: buses 38-45
# Ports 33-40: buses 47-54
# Ports 41-48: buses 56-63
# QSFP 49-52:  buses 66-69
get_sfp_bus() {
    local port=$1
    if [ $port -le 8 ]; then
        echo $((10 + port))
    elif [ $port -le 16 ]; then
        echo $((19 + port - 8))
    elif [ $port -le 24 ]; then
        echo $((28 + port - 16))
    elif [ $port -le 32 ]; then
        echo $((37 + port - 24))
    elif [ $port -le 40 ]; then
        echo $((46 + port - 32))
    elif [ $port -le 48 ]; then
        echo $((55 + port - 40))
    else
        echo $((65 + port - 48))
    fi
}

for port in $present_ports; do
    bus=$(get_sfp_bus $port)
    eeprom="/sys/bus/i2c/devices/${bus}-0050/eeprom"
    if [ -f "$eeprom" ]; then
        vendor=$(dd if=$eeprom bs=1 count=16 skip=20 2>/dev/null | tr -d '\0 ')
        pn=$(dd if=$eeprom bs=1 count=16 skip=40 2>/dev/null | tr -d '\0 ')
        echo "  Port $port (bus $bus): $vendor $pn"
    else
        echo "  Port $port (bus $bus): EEPROM not available"
    fi
done
echo ""

echo "========================================="
echo "  Done"
echo "========================================="
