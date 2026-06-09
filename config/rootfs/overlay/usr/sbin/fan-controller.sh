#!/bin/sh
# fan-controller.sh — Cumulus-pwmd-style fan controller for AS5610-52X
#
# Reads every available temperature sensor (MAX6697 7-channel via hwmon +
# NE1617A 2-channel via direct I2C for the ASIC die) and drives the CPLD
# fan PWM through the as5610_52x_cpld driver's sysfs (NOT devmem).
#
# Behavior follows Cumulus's pwmd in spirit: continuous polling, smooth
# ramp on changes, hysteresis to avoid oscillation, and a hard minimum
# floor at the highest temp tier so a single hot reading can't underspeed
# the fans.
#
# CPLD fan PWM register width is 5 bits (0-31). Anything > 0x1F wraps.

set -u

# CPLD fan_pwm sysfs node. With the current driver (.dev_groups) it lives at the
# DEVICE path; with the older loaded .ko it's at the DRIVER path. Probe both so
# this works on a running box and after a reflash. PWM_SYS is resolved at startup
# (see the writable-node check below).
PWM_CANDIDATES="/sys/devices/platform/as5610_52x_cpld/fan_pwm /sys/bus/platform/drivers/as5610_52x_cpld/fan_pwm"
PWM_SYS=""

POLL_INTERVAL=5         # seconds between samples
RAMP_STEP=2             # max PWM units changed per poll (smooth ramp)
HYSTERESIS=3            # degC band for the "stepping down" boundary

# PWM tiers (0..31)
PWM_QUIET=10            # ~32%   idle
PWM_LOW=14              # ~45%
PWM_MED=20              # ~64%
PWM_HIGH=26             # ~83%
PWM_FULL=31             # 100%

# Temperature tiers (degC). Highest sensor reading drives the tier.
T_LOW=40
T_MED=50
T_HIGH=60
T_CRIT=72

# Emergency shutdown (degC). If the hottest sensor stays at/above T_EMERG for
# EMERG_GRACE consecutive polls (so a single transient spike can't trip it),
# halt the box to protect the hardware. This is the safety net that used to
# live in the now-retired thermal-mgmt.sh; fans are already at full from T_CRIT.
T_EMERG=75
EMERG_GRACE=6           # consecutive over-T_EMERG polls before halt (~30s at 5s poll)

log() { logger -t fan-controller "$*"; }

get_max_temp() {
    max=0
    for f in /sys/class/hwmon/hwmon*/temp*_input; do
        [ -r "$f" ] || continue
        v=$(cat "$f" 2>/dev/null) || continue
        [ -z "$v" ] && continue
        c=$((v / 1000))
        [ "$c" -gt "$max" ] && max=$c
    done
    for reg in 1 0; do
        v=$(i2cget -y 9 0x18 "$reg" b 2>/dev/null) || continue
        [ -z "$v" ] && continue
        c=$((v))
        [ "$c" -gt 127 ] && c=$((c - 256))
        [ "$c" -gt "$max" ] && max=$c
    done
    echo "$max"
}

target_pwm() {
    t=$1
    cur=$2
    if [ "$t" -ge "$T_CRIT" ]; then echo "$PWM_FULL"; return; fi
    if [ "$t" -ge "$T_HIGH" ]; then echo "$PWM_HIGH"; return; fi
    if [ "$t" -ge "$T_MED"  ]; then echo "$PWM_MED";  return; fi
    if [ "$t" -ge "$T_LOW"  ]; then echo "$PWM_LOW";  return; fi
    if [ "$cur" -le "$PWM_QUIET" ] || [ "$t" -lt "$((T_LOW - HYSTERESIS))" ]; then
        echo "$PWM_QUIET"
    else
        echo "$PWM_LOW"
    fi
}

write_pwm() {
    v=$1
    [ "$v" -lt 0  ] && v=0
    [ "$v" -gt 31 ] && v=31
    if ! echo "$v" > "$PWM_SYS" 2>/dev/null; then
        log "ERROR: failed to write $PWM_SYS=$v"
        return 1
    fi
    echo "$v"
}

# Resolve the fan_pwm node to whichever candidate path is writable. Note we only
# ever WRITE this node (get_max_temp reads hwmon, not fan_pwm), so the older
# driver's read-side segfault on fan_pwm never bites us.
for cand in $PWM_CANDIDATES; do
    if [ -w "$cand" ]; then PWM_SYS="$cand"; break; fi
done
if [ -z "$PWM_SYS" ]; then
    log "FATAL: no writable CPLD fan_pwm node (is as5610_52x_cpld loaded?). Exiting."
    exit 1
fi

cur=$PWM_MED
write_pwm "$cur" >/dev/null
emerg_count=0
log "started (sysfs=$PWM_SYS, poll=${POLL_INTERVAL}s)"

while :; do
    temp=$(get_max_temp)
    tgt=$(target_pwm "$temp" "$cur")
    if   [ "$tgt" -gt "$cur" ]; then
        new=$((cur + RAMP_STEP))
        [ "$new" -gt "$tgt" ] && new=$tgt
    elif [ "$tgt" -lt "$cur" ]; then
        new=$((cur - RAMP_STEP))
        [ "$new" -lt "$tgt" ] && new=$tgt
    else
        new=$cur
    fi
    if [ "$new" -ne "$cur" ]; then
        cur=$(write_pwm "$new")
        log "temp=${temp}C target=${tgt} pwm=${cur}/31"
    fi
    if [ "$temp" -ge "$T_CRIT" ]; then
        log "CRITICAL temp=${temp}C (>=${T_CRIT}C) - fan at full"
    fi
    # Emergency: sustained over-temp -> halt to protect hardware.
    if [ "$temp" -ge "$T_EMERG" ]; then
        emerg_count=$((emerg_count + 1))
        log "EMERGENCY temp=${temp}C (>=${T_EMERG}C) - strike ${emerg_count}/${EMERG_GRACE}, fans at full"
        if [ "$emerg_count" -ge "$EMERG_GRACE" ]; then
            log "EMERGENCY: temp>=${T_EMERG}C sustained ${EMERG_GRACE} polls - halting now"
            shutdown -h now "Thermal emergency: ${temp}C" 2>/dev/null \
                || poweroff -f 2>/dev/null
        fi
    else
        emerg_count=0
    fi
    sleep "$POLL_INTERVAL"
done
