#!/bin/sh
# fan-controller.sh - Temperature-based fan speed control for AS5610-52X
#
# Reads temperatures from MAX6697 (hwmon) and NE1617A (ASIC die temp),
# sets fan PWM via CPLD at eLBC 0xEA00000D.
#
# Fan PWM range: 0-31 (0=off, 31=full speed)
# Temperature thresholds (highest temp from all sensors drives fan speed):
#   < 35°C: low speed (8/31 = ~25%)
#   35-45°C: medium speed (14/31 = ~45%)
#   45-55°C: high speed (22/31 = ~70%)
#   > 55°C: full speed (31/31 = 100%)
#   > 70°C: critical - already at full, log warning
#
# CPLD fan register: devmem 0xEA00000D 8 <value>

CPLD_FAN_REG=$((0xEA000000 + 0x0D))
INTERVAL=10  # seconds between checks

FAN_LOW=8
FAN_MED=14
FAN_HIGH=22
FAN_FULL=31

TEMP_LOW=35
TEMP_MED=45
TEMP_HIGH=55
TEMP_CRIT=70

log() {
    echo "$*"
}

get_max_temp() {
    max=0

    # MAX6697 via hwmon (millidegrees)
    for f in /sys/class/hwmon/hwmon*/temp*_input; do
        [ -f "$f" ] || continue
        val=$(cat "$f" 2>/dev/null)
        [ -z "$val" ] && continue
        temp=$((val / 1000))
        [ $temp -gt $max ] && max=$temp
    done

    # NE1617A ASIC die temp (bus 0 or 9, addr 0x18, reg 1 = remote)
    for bus in 0 9; do
        val=$(i2cget -y $bus 0x18 1 b 2>/dev/null)
        if [ -n "$val" ]; then
            temp=$((val))
            [ $temp -gt $max ] && max=$temp
        fi
    done

    echo $max
}

set_fan_speed() {
    devmem $CPLD_FAN_REG 8 "$1" 2>/dev/null
}

# Start at medium speed
set_fan_speed $FAN_MED
log "started (initial speed=$FAN_MED)"
current_speed=$FAN_MED

while true; do
    temp=$(get_max_temp)

    if [ $temp -ge $TEMP_CRIT ]; then
        target=$FAN_FULL
        [ $current_speed -ne $FAN_FULL ] && log "CRITICAL: temp=${temp}C, full speed"
    elif [ $temp -ge $TEMP_HIGH ]; then
        target=$FAN_FULL
        [ $current_speed -ne $FAN_FULL ] && log "HIGH: temp=${temp}C, full speed"
    elif [ $temp -ge $TEMP_MED ]; then
        target=$FAN_HIGH
    elif [ $temp -ge $TEMP_LOW ]; then
        target=$FAN_MED
    else
        target=$FAN_LOW
    fi

    if [ $target -ne $current_speed ]; then
        set_fan_speed $target
        log "temp=${temp}C speed=${target}/31"
        current_speed=$target
    fi

    sleep $INTERVAL
done
