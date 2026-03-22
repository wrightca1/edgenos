#!/bin/bash
# fand.sh - Simple fan-to-temperature controller for AS5610-52X
#
# Reads MAX6697 temperatures and adjusts CPLD fan PWM.
# Run as: fand.sh &  (or via systemd)
#
# Thresholds:
#   < 35°C  → 40% (0x0c)  - quiet
#   35-45°C → 55% (0x11)  - medium
#   45-55°C → 70% (0x15)  - high
#   > 55°C  → 100% (0x1f) - full

INTERVAL=10  # seconds between checks
HYSTERESIS=3 # degrees C to prevent oscillation

SPEED_LOW=0x0c    # 40%
SPEED_MED=0x11    # 55%
SPEED_HIGH=0x15   # 70%
SPEED_MAX=0x1f    # 100%

THRESH_MED=35
THRESH_HIGH=45
THRESH_MAX=55

HWMON="/sys/bus/i2c/devices/9-004d/hwmon/hwmon0"
CPLD_TOOL="/usr/sbin/cpld"

log() { logger -t fand "$*"; }

get_max_temp() {
    local max=0
    for f in "$HWMON"/temp*_input; do
        [ -f "$f" ] || continue
        val=$(cat "$f" 2>/dev/null)
        val=$((val / 1000))
        [ $val -gt $max ] && max=$val
    done
    echo $max
}

current_speed=""

set_speed() {
    local new_speed=$1
    if [ "$new_speed" != "$current_speed" ]; then
        $CPLD_TOOL write 0x0d $new_speed >/dev/null 2>&1
        current_speed=$new_speed
        log "Fan speed → $new_speed (temp=${2}°C)"
    fi
}

log "Starting fan controller (interval=${INTERVAL}s)"

# Start at medium
set_speed $SPEED_MED 0

while true; do
    temp=$(get_max_temp)

    if [ $temp -ge $THRESH_MAX ]; then
        set_speed $SPEED_MAX $temp
    elif [ $temp -ge $((THRESH_HIGH - HYSTERESIS)) ] && [ "$current_speed" = "$SPEED_MAX" ]; then
        # Stay at max (hysteresis)
        :
    elif [ $temp -ge $THRESH_HIGH ]; then
        set_speed $SPEED_HIGH $temp
    elif [ $temp -ge $((THRESH_MED - HYSTERESIS)) ] && [ "$current_speed" = "$SPEED_HIGH" ]; then
        # Stay at high (hysteresis)
        :
    elif [ $temp -ge $THRESH_MED ]; then
        set_speed $SPEED_MED $temp
    else
        set_speed $SPEED_LOW $temp
    fi

    sleep $INTERVAL
done
