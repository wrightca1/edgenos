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

# CPLD fan_pwm sysfs node — DEVICE path ONLY.
#
# The correct CPLD driver (.dev_groups + platform_set_drvdata) exposes fan_pwm
# here, with a valid private context, so reads/writes are safe.
#
# We deliberately do NOT fall back to the driver-path node
# (/sys/bus/platform/drivers/as5610_52x_cpld/fan_pwm). An older buggy CPLD build
# attaches the attribute to the *driver* kobject, where the store handler gets a
# NULL drvdata and dereferences it — writing that node OOPSES THE KERNEL. Only
# the device-path node is ever safe to touch; if it's absent we leave the fans
# at their (safe) hardware default rather than risk the driver-path node.
PWM_SYS=/sys/devices/platform/as5610_52x_cpld/fan_pwm

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

# ASIC INTERNAL JUNCTION threshold (bde_tmon hwmon). The switch ASIC die runs
# MUCH hotter than the board/case sensors (75-95C+ is normal), so it must NOT
# share the board T_CRIT/T_EMERG or a healthy chip trips the board emergency and
# halts the box. We force fans to full when the junction is hot, but we do NOT
# halt on it: the die's safe-vs-danger point isn't characterized on this board
# (an early guess caused a false shutdown), and the chip's own HARDWARE thermal
# protection plus the board/case emergency are the real safety net. Log-only.
T_JUNC_FULL=95          # junction at/above this -> fans to full (no halt)

log() { logger -t fan-controller "$*"; }

# Hottest BOARD/CASE sensor (MAX6697 hwmon channels + NE1617A external ASIC-die
# diode). EXCLUDES the bde_tmon ASIC internal junction (handled separately) and
# ignores implausible readings (>=120C) so a faulty/unconnected sensor can't
# drive the fans or trip the board emergency.
get_max_temp() {
    max=0
    for f in /sys/class/hwmon/hwmon*/temp*_input; do
        [ -r "$f" ] || continue
        [ "$(cat "$(dirname "$f")/name" 2>/dev/null)" = "bde_tmon" ] && continue
        v=$(cat "$f" 2>/dev/null) || continue
        [ -z "$v" ] && continue
        c=$((v / 1000))
        [ "$c" -ge 120 ] && continue
        [ "$c" -gt "$max" ] && max=$c
    done
    # NE1617A external ASIC-die diode via direct I2C. If adm1021 has claimed it
    # (the DT now binds it as an hwmon), this read returns EBUSY and is skipped —
    # the hwmon loop above already counted it.
    for reg in 1 0; do
        v=$(i2cget -y 9 0x18 "$reg" b 2>/dev/null) || continue
        [ -z "$v" ] && continue
        c=$((v))
        [ "$c" -gt 127 ] && c=$((c - 256))
        [ "$c" -ge 120 ] && continue
        [ "$c" -gt "$max" ] && max=$c
    done
    echo "$max"
}

# Hottest ASIC INTERNAL junction (bde_tmon hwmon), or 0 if not present. Runs hot
# by design — kept on its own T_JUNC_* limits, never the board emergency.
get_junction_temp() {
    j=0
    for f in /sys/class/hwmon/hwmon*/temp*_input; do
        [ -r "$f" ] || continue
        [ "$(cat "$(dirname "$f")/name" 2>/dev/null)" = "bde_tmon" ] || continue
        v=$(cat "$f" 2>/dev/null) || continue
        [ -z "$v" ] && continue
        c=$((v / 1000))
        # bde_tmon's read path isn't validated yet and returns a 150C sentinel
        # (TMON_TEMP_MAX_MC) when the sensor isn't ready / reads out of range.
        # Ignore any implausible value (a live die can't exceed ~125C — the chip
        # HW-thermal-shuts-down first), so a bogus reading can't peg the fans.
        [ "$c" -ge 125 ] && continue
        [ "$c" -gt "$j" ] && j=$c
    done
    echo "$j"
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

# Wait briefly for the CPLD driver to create the device-path fan_pwm node at
# boot. If it never shows up (e.g. an old driver that only exposes the unsafe
# driver-path node), exit 0 cleanly and leave the fans at their safe hardware
# default — do NOT restart-loop and do NOT touch the driver-path node.
i=0
while [ ! -w "$PWM_SYS" ]; do
    i=$((i + 1))
    if [ "$i" -gt 30 ]; then
        log "$PWM_SYS absent/unwritable after 30s — leaving fans at hardware \
default (no active control; correct CPLD driver required). Exiting cleanly."
        exit 0
    fi
    sleep 1
done

cur=$PWM_MED
write_pwm "$cur" >/dev/null
emerg_count=0
log "started (sysfs=$PWM_SYS, poll=${POLL_INTERVAL}s)"

while :; do
    temp=$(get_max_temp)
    junc=$(get_junction_temp)
    tgt=$(target_pwm "$temp" "$cur")
    # A hot ASIC junction forces full cooling regardless of the board tier.
    if [ "$junc" -ge "$T_JUNC_FULL" ]; then tgt=$PWM_FULL; fi
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
    # ASIC junction: log when hot (fans already forced full above T_JUNC_FULL),
    # but NEVER halt on it — see the T_JUNC_FULL note. The board emergency above
    # and the chip's hardware thermal protection are the real safety net.
    if [ "$junc" -ge "$T_JUNC_FULL" ]; then
        log "junction hot: ${junc}C (>=${T_JUNC_FULL}C; fans full, not halting)"
    fi
    sleep "$POLL_INTERVAL"
done
