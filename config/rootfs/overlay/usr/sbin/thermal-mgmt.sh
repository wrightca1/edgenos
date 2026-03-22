#!/bin/bash
# thermal-mgmt.sh - Thermal management daemon for AS5610-52X
#
# Reads temperature sensors and adjusts fan speed via CPLD PWM.
#
# Hardware (from COMPLETE_CHIP_INVENTORY.md):
#   MAX6697 (7-channel) at I2C bus 9, addr 0x4d -> hwmon1
#   MAX1617 (2-channel) at I2C bus 9, addr 0x18 -> hwmon2
#   Fan PWM via CPLD at /sys/devices/ff705000.localbus/ea000000.cpld/pwm1
#   Fan PWM range: 0 (off) to 248 (full speed)
#
# Cumulus uses pwmd daemon for this; we use a simple shell script.

CPLD="/sys/devices/ff705000.localbus/ea000000.cpld"
CPLD_PWM="${CPLD}/pwm1"
CPLD_FAN_LED="${CPLD}/led_fan"
POLL_INTERVAL=10  # seconds

# Thermal thresholds (millidegrees C)
THRESH_LOW=40000
THRESH_MED=50000
THRESH_HIGH=60000
THRESH_CRIT=75000

# Fan PWM values (CPLD register, 0-248)
FAN_LOW=64       # ~25%
FAN_MED=128      # ~50%
FAN_HIGH=200     # ~80%
FAN_FULL=248     # 100%

log() { logger -t thermal-mgmt "$*"; }

get_max_temp() {
    local max=0
    for f in /sys/class/hwmon/hwmon*/temp*_input; do
        [ -f "$f" ] || continue
        temp=$(cat "$f" 2>/dev/null) || continue
        [ "$temp" -gt "$max" ] && max=$temp
    done
    echo "$max"
}

set_fan() {
    local pwm=$1
    [ -f "$CPLD_PWM" ] && echo "$pwm" > "$CPLD_PWM" 2>/dev/null
}

log "Thermal management started (poll=${POLL_INTERVAL}s)"
set_fan $FAN_MED  # Start at medium

while true; do
    max_temp=$(get_max_temp)
    temp_c=$((max_temp / 1000))

    if [ "$max_temp" -ge "$THRESH_CRIT" ]; then
        log "CRITICAL: ${temp_c}C! Full fan + shutdown warning"
        set_fan $FAN_FULL
        [ -f "$CPLD_FAN_LED" ] && echo yellow > "$CPLD_FAN_LED"
        sleep 30
        max_temp=$(get_max_temp)
        if [ "$max_temp" -ge "$THRESH_CRIT" ]; then
            log "CRITICAL: Still ${temp_c}C. Emergency shutdown."
            shutdown -h now "Thermal emergency"
        fi
    elif [ "$max_temp" -ge "$THRESH_HIGH" ]; then
        set_fan $FAN_HIGH
    elif [ "$max_temp" -ge "$THRESH_MED" ]; then
        set_fan $FAN_MED
    else
        set_fan $FAN_LOW
    fi

    sleep $POLL_INTERVAL
done
