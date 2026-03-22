#!/bin/bash
# platform-init.sh - AS5610-52X platform hardware initialization
#
# This replicates the Cumulus Linux boot sequence:
#   S06 kmod      -> load kernel modules
#   S09 hw_init   -> S10gpio_init.sh + S20retimer_init.sh
#
# Module load order (from Cumulus /etc/modules):
#   1. linux-kernel-bde (himem=1) - ASIC PCI driver + DMA pool
#   2. linux-user-bde             - userspace BDE interface
#   3. tun                        - TUN/TAP for packet I/O (52 ports)
#   4. accton_as5610_52x_cpld     - CPLD (LEDs, PSU, fan, watchdog)
#   5. at24                       - EEPROM (board + 48 SFP + 4 QSFP)
#   6. sff_8436_eeprom            - QSFP EEPROM pages
#   7. gpio-pca953x               - GPIO expanders (SFP/QSFP control)
#   8. max6697                    - Temperature sensor (7-channel)
#   9. adm1021                    - Temperature sensor (2-channel)
#  10. ds100df410                 - Retimer/equalizer (32 devices)
#
# After modules: GPIO init, retimer programming, fan speed

# Don't exit on errors - continue loading what we can
set +e

log() { echo "platform-init: $*"; logger -t platform-init "$*" 2>/dev/null; }

MODDIR="/lib/modules/$(uname -r)"
EXTRA="${MODDIR}/extra"

load_mod() {
    local mod="$1"; shift
    if [ -f "${EXTRA}/${mod}.ko" ]; then
        insmod "${EXTRA}/${mod}.ko" "$@" && log "Loaded ${mod}" || log "WARN: ${mod} failed"
    else
        modprobe "$mod" "$@" 2>/dev/null && log "Loaded ${mod}" || true
    fi
}

# ────── Phase 1: Load kernel modules ──────

log "=== Phase 1: Loading kernel modules ==="

# BDE: ASIC PCI driver + DMA pool (must be first)
load_mod linux-kernel-bde dma_size=32
load_mod linux-user-bde

# TUN: packet I/O interfaces
load_mod tun

# CPLD: system management
load_mod accton_as5610_52x_cpld

# I2C devices
load_mod at24
load_mod sff_8436_eeprom

# GPIO expanders (PCA9506 40-bit + PCA9538 8-bit)
load_mod gpio-pca953x

# Temperature sensors
load_mod max6697
load_mod adm1021

# Retimer/equalizer
load_mod retimer_class 2>/dev/null || true
load_mod ds100df410

# ────── Phase 2: GPIO initialization ──────
# Replicates Cumulus S10gpio_init.sh

log "=== Phase 2: Initializing GPIOs ==="

GPIO_SYS="/sys/class/gpio"

export_gpio() {
    local gpio=$1 dir=$2 val=$3
    [ -d "${GPIO_SYS}/gpio${gpio}" ] || echo "$gpio" > "${GPIO_SYS}/export" 2>/dev/null || return
    echo "$dir" > "${GPIO_SYS}/gpio${gpio}/direction" 2>/dev/null || return
    [ -n "$val" ] && echo "$val" > "${GPIO_SYS}/gpio${gpio}/value" 2>/dev/null || true
}

# QSFP reset lines (PCA9538 at gpiochip160): RST_L high = not reset
for gpio in $(seq 160 163); do
    export_gpio $gpio out 1
done

# QSFP modsel lines (PCA9538 at gpiochip164): MODSEL_L low = selected
for gpio in $(seq 164 167); do
    export_gpio $gpio out 0
done

# QSFP lpmode lines (PCA9538 at gpiochip168): LPMODE low = high power
for gpio in $(seq 168 171); do
    export_gpio $gpio out 0
done

# SFP TX disable: PCA9506 at gpiochip24 (40 GPIOs) + gpiochip64 (40 GPIOs)
# TX_DISABLE is active-high, set low to enable TX
for gpio in $(seq 24 64); do
    export_gpio $gpio out 0
done
for gpio in $(seq 97 104); do
    export_gpio $gpio out 0
done

log "GPIO initialization complete"

# ────── Phase 3: Retimer/equalizer programming ──────
# Replicates Cumulus S20retimer_init.sh
# 32 DS100DF410 devices at I2C addr 0x27 on buses 18-69

log "=== Phase 3: Programming retimers ==="

RDIR="/sys/class/retimer_dev"
NUM_RETIMERS=32

if [ -d "$RDIR" ]; then
    retimer_count=$(ls "$RDIR" 2>/dev/null | wc -l)
    if [ "$retimer_count" -eq "$NUM_RETIMERS" ]; then
        for i in $(seq 0 $((NUM_RETIMERS - 1))); do
            rdev="${RDIR}/retimer${i}/device"
            [ -d "$rdev" ] || continue
            label=$(cat "${RDIR}/retimer${i}/label" 2>/dev/null)
            cd "$rdev"

            # Common settings for all retimers
            echo 12 > channels 2>/dev/null || true
            echo 1  > veo_clk_cdr_cap 2>/dev/null || true
            echo 28 > cdr_rst 2>/dev/null || true
            echo 16 > cdr_rst 2>/dev/null || true

            # QSFP and SFP RX EQ get additional tap_dem
            case "$label" in
                qsfp*|sfp_rx_eq_*)
                    echo 23 > tap_dem 2>/dev/null || true
                    ;;
            esac
        done
        log "Programmed $NUM_RETIMERS retimers"
    else
        log "WARN: Expected $NUM_RETIMERS retimers, found $retimer_count"
    fi
else
    log "WARN: No retimer sysfs ($RDIR not found)"
fi

# ────── Phase 4: System status ──────

log "=== Phase 4: System status ==="

CPLD="/sys/devices/ff705000.localbus/ea000000.cpld"
if [ -d "$CPLD" ]; then
    # Set system LED to yellow (booting)
    echo yellow > "${CPLD}/led_diag" 2>/dev/null || true

    # Set fan speed to medium
    echo 128 > "${CPLD}/pwm1" 2>/dev/null || true

    # Read PSU status
    psu1=$(cat "${CPLD}/psu_pwr1_all_ok" 2>/dev/null)
    psu2=$(cat "${CPLD}/psu_pwr2_all_ok" 2>/dev/null)
    fan=$(cat "${CPLD}/system_fan_ok" 2>/dev/null)
    log "PSU1=${psu1:-?} PSU2=${psu2:-?} FAN=${fan:-?}"

    # Set PSU LEDs
    [ "$psu1" = "1" ] && echo green > "${CPLD}/led_psu1" || echo yellow > "${CPLD}/led_psu1"
    [ "$psu2" = "1" ] && echo green > "${CPLD}/led_psu2" || echo yellow > "${CPLD}/led_psu2"
    [ "$fan" = "1" ]  && echo green > "${CPLD}/led_fan"  || echo yellow > "${CPLD}/led_fan"
fi

log "=== Platform initialization complete ==="
