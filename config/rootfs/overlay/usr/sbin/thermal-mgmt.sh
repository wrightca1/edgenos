#!/bin/bash
# thermal-mgmt.sh - Thermal management daemon
# Reads sensors and adjusts fan speed via CPLD

CPLD_FAN_PWM="/sys/bus/platform/devices/as5610_52x_cpld/fan_pwm"
POLL_INTERVAL=10  # seconds

# Thermal thresholds (millidegrees C)
THRESH_LOW=45000
THRESH_MED=55000
THRESH_HIGH=65000
THRESH_CRIT=75000

# Fan duty cycle values (CPLD register 0x0D)
FAN_LOW=4      # ~40%
FAN_MED=7      # ~70%
FAN_HIGH=10    # ~100%

log() { logger -t thermal-mgmt "$*"; }

get_max_temp() {
    local max=0
    local temp

    # Read all hwmon temperature sensors
    for f in /sys/class/hwmon/hwmon*/temp*_input; do
        [ -f "$f" ] || continue
        temp=$(cat "$f" 2>/dev/null) || continue
        if [ "$temp" -gt "$max" ]; then
            max=$temp
        fi
    done
    echo "$max"
}

set_fan_duty() {
    local duty=$1
    if [ -f "$CPLD_FAN_PWM" ]; then
        echo "$duty" > "$CPLD_FAN_PWM"
    fi
}

log "Thermal management started"
set_fan_duty $FAN_MED  # Start at medium

while true; do
    max_temp=$(get_max_temp)

    if [ "$max_temp" -ge "$THRESH_CRIT" ]; then
        log "CRITICAL: Temperature ${max_temp}mC exceeds ${THRESH_CRIT}mC! Emergency shutdown!"
        set_fan_duty $FAN_HIGH
        sleep 30
        # Re-check before shutdown
        max_temp=$(get_max_temp)
        if [ "$max_temp" -ge "$THRESH_CRIT" ]; then
            log "CRITICAL: Still above threshold. Shutting down."
            shutdown -h now "Thermal emergency shutdown"
        fi
    elif [ "$max_temp" -ge "$THRESH_HIGH" ]; then
        set_fan_duty $FAN_HIGH
    elif [ "$max_temp" -ge "$THRESH_MED" ]; then
        set_fan_duty $FAN_MED
    elif [ "$max_temp" -ge "$THRESH_LOW" ]; then
        set_fan_duty $FAN_MED
    else
        set_fan_duty $FAN_LOW
    fi

    sleep $POLL_INTERVAL
done
