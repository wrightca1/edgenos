#!/bin/sh
# thermal-control.sh - fan control for the Arista DCS-7150S-52.
#
# WHY THIS EXISTS: without it the box has no thermal management at all. The fans
# come up at PWM 255 (~18000 RPM) by hardware default, which is safe but very
# loud, and nothing watches temperature -- so a failed fan or a blocked intake
# would go unnoticed until the ASIC cooked.
#
# Sensors: MAX6658 (lm90-compatible) on SCD SMBus master 0 bus 2 @ 0x4c
#   temp1 = board/local        crit 90 C
#   temp2 = FM6000 die/remote  crit 100 C   <-- the one that matters
# Fans:    raven-fan-driver hwmon "fans", 4 x {fanN_input, pwmN, fanN_present}
#
# SAFETY RULES, in priority order. When in doubt this script makes the fans
# louder, never quieter:
#   1. Any sensor read failure, missing hwmon, or unparsable value -> PWM 255.
#   2. Never command below PWM_FLOOR, so airflow never stops.
#   3. At or above CRIT_WARN on the die -> PWM 255 and a loud log line.
#   4. Ramping down requires the temperature to fall a full hysteresis band,
#      so a sensor hovering on a boundary cannot oscillate the fans.
#   5. A stopped fan (present but 0 RPM) -> PWM 255 and a loud log line.
#
# Usage: thermal-control.sh [-i secs] [-1] [-v]
#   -1  one shot (read, decide, set, exit) -- for testing
#   -v  log every poll, not just changes
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u

POLL=5
ONESHOT=0
VERBOSE=0
while [ $# -gt 0 ]; do
    case "$1" in
        -i) POLL="$2"; shift 2;;
        -1) ONESHOT=1; shift;;
        -v) VERBOSE=1; shift;;
        *) echo "unknown arg: $1" >&2; exit 1;;
    esac
done

PWM_FLOOR=102          # 40% -- never below this; guarantees airflow
PWM_MAX=255
HYST=5                 # degrees C the temp must drop before we step down
CRIT_WARN=85           # die temp at which we go full speed and shout (crit is 100)
I2C_BUS="${THERMAL_I2C_BUS:-2}"   # SCD SMBus master 0 bus 2 (override for testing)
SENSOR_ADDR=0x4c
SENSOR_TYPE=max6658

log() { echo "[thermal] $*"; }

# --- locate hwmon nodes by name, not by index (index order is not stable) ----
find_hwmon() {
    for h in /sys/class/hwmon/hwmon*; do
        [ -e "$h/name" ] || continue
        [ "$(cat "$h/name" 2>/dev/null)" = "$1" ] && { echo "$h"; return 0; }
    done
    return 1
}

# The temp sensor is not instantiated by any driver at boot; do it ourselves.
ensure_sensor() {
    find_hwmon "$SENSOR_TYPE" >/dev/null 2>&1 && return 0
    if [ -w "/sys/bus/i2c/devices/i2c-$I2C_BUS/new_device" ]; then
        log "instantiating $SENSOR_TYPE at $SENSOR_ADDR on i2c-$I2C_BUS"
        echo "$SENSOR_TYPE $SENSOR_ADDR" > "/sys/bus/i2c/devices/i2c-$I2C_BUS/new_device" 2>/dev/null
        sleep 1
    fi
    find_hwmon "$SENSOR_TYPE" >/dev/null 2>&1
}

set_pwm() {                        # set_pwm <value>
    v="$1"
    [ "$v" -lt "$PWM_FLOOR" ] && v="$PWM_FLOOR"      # rule 2
    [ "$v" -gt "$PWM_MAX" ] && v="$PWM_MAX"
    for i in 1 2 3 4; do
        [ -w "$FANS/pwm$i" ] && echo "$v" > "$FANS/pwm$i" 2>/dev/null
    done
    CUR_PWM="$v"
}

# temperature -> pwm. Deliberately conservative; these fans have huge headroom.
pwm_for_temp() {
    t="$1"
    if   [ "$t" -ge "$CRIT_WARN" ]; then echo 255
    elif [ "$t" -ge 75 ]; then echo 255
    elif [ "$t" -ge 65 ]; then echo 217
    elif [ "$t" -ge 55 ]; then echo 178
    elif [ "$t" -ge 45 ]; then echo 140
    else echo "$PWM_FLOOR"
    fi
}

FANS="$(find_hwmon fans || true)"
if [ -z "$FANS" ]; then
    log "FATAL: no 'fans' hwmon device -- cannot control cooling, leaving hardware default"
    exit 1
fi
log "fans at $FANS"

if ensure_sensor; then
    TEMPS="$(find_hwmon "$SENSOR_TYPE")"
    log "sensors at $TEMPS (temp1=board, temp2=FM6000 die)"
else
    TEMPS=""
    log "WARNING: no temperature sensor -- running fans at FULL SPEED (rule 1)"
fi

CUR_PWM=-1
LAST_T=-1
trap 'log "exiting -- leaving fans at full speed for safety"; set_pwm 255; exit 0' INT TERM

while :; do
    die=-1; board=-1; fail=0

    if [ -n "$TEMPS" ] && [ -r "$TEMPS/temp2_input" ]; then
        raw="$(cat "$TEMPS/temp2_input" 2>/dev/null)"
        case "$raw" in ''|*[!0-9-]*) fail=1;; *) die=$((raw / 1000));; esac
        raw="$(cat "$TEMPS/temp1_input" 2>/dev/null)"
        case "$raw" in ''|*[!0-9-]*) : ;; *) board=$((raw / 1000));; esac
    else
        fail=1
    fi

    # rule 5: a fan that is present but not turning
    stopped=""
    for i in 1 2 3 4; do
        p="$(cat "$FANS/fan${i}_present" 2>/dev/null || echo 0)"
        r="$(cat "$FANS/fan${i}_input" 2>/dev/null || echo 0)"
        [ "$p" = "1" ] && [ "$r" = "0" ] && stopped="$stopped $i"
    done

    if [ "$fail" = "1" ]; then                              # rule 1
        [ "$CUR_PWM" != "$PWM_MAX" ] && log "SENSOR READ FAILED -> full speed"
        set_pwm "$PWM_MAX"
    elif [ -n "$stopped" ]; then                            # rule 5
        log "FAN STOPPED:$stopped (present but 0 RPM) -> full speed"
        set_pwm "$PWM_MAX"
    else
        hot="$die"; [ "$board" -gt "$hot" ] && hot="$board"
        want="$(pwm_for_temp "$hot")"

        # rule 4: only ramp down after a full hysteresis band
        if [ "$CUR_PWM" -ge 0 ] && [ "$want" -lt "$CUR_PWM" ] && \
           [ "$LAST_T" -ge 0 ] && [ $((LAST_T - hot)) -lt "$HYST" ]; then
            want="$CUR_PWM"
        else
            LAST_T="$hot"
        fi

        if [ "$die" -ge "$CRIT_WARN" ]; then                # rule 3
            log "CRITICAL: FM6000 die ${die}C (crit 100C) -> full speed"
        fi
        if [ "$want" != "$CUR_PWM" ]; then
            log "die=${die}C board=${board}C -> pwm $want"
        elif [ "$VERBOSE" = "1" ]; then
            log "die=${die}C board=${board}C pwm=$CUR_PWM (hold)"
        fi
        set_pwm "$want"
    fi

    [ "$ONESHOT" = "1" ] && exit 0
    sleep "$POLL"
done
