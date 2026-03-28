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
# Fallback: modules may be in /usr/lib/modules/extra (squashfs layout)
[ -d "$EXTRA" ] || EXTRA="/usr/lib/modules/extra"

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
# Try 4MB DMA first (32MB often fails on P2020 without CMA)
load_mod linux-kernel-bde dma_size=4
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

# QSFP control via PCA9538 GPIO expanders
# Find the actual gpiochip base numbers (vary by kernel version)
for chip in /sys/class/gpio/gpiochip*; do
    label=$(cat "$chip/label" 2>/dev/null)
    base=$(cat "$chip/base" 2>/dev/null)
    case "$label" in
        64-007[0-3])
            # PCA9538 QSFP GPIO: pin0=RESET_N pin1=LPMODE pin2=MODSEL_N
            export_gpio $base out 1           # RESET_N = 1 (deassert)
            export_gpio $((base+1)) out 0     # LPMODE = 0 (high power)
            export_gpio $((base+2)) out 0     # MODSEL_N = 0 (selected)
            log "QSFP GPIO: chip=$label base=$base"
            ;;
    esac
done

# SFP TX_DISABLE via PCA9506 GPIO expanders
# EdgeNOS bus 64 = mux 0x76 ch2 (Cumulus was bus 16)
# EdgeNOS bus 65 = mux 0x76 ch3 (Cumulus was bus 17)
# PCA9506: direction reg 0x18+, output reg 0x08+
# Set all outputs to 0 (TX_DISABLE = low = TX enabled)
for bus in 64 65; do
    for addr in 0x20 0x21 0x22 0x23 0x24; do
        if i2cget -y $bus $addr 0 b >/dev/null 2>&1; then
            # Set all 5 ports to output, all bits low
            for port in 0x18 0x19 0x1a 0x1b 0x1c; do
                i2cset -y $bus $addr $port 0x00 b 2>/dev/null
            done
            for port in 0x08 0x09 0x0a 0x0b 0x0c; do
                i2cset -y $bus $addr $port 0x00 b 2>/dev/null
            done
        fi
    done
done
log "SFP TX_DISABLE cleared (buses 64-65)"

log "GPIO initialization complete"

# ────── Phase 3: Retimer/equalizer programming ──────
# Replicates Cumulus S20retimer_init.sh
# 32 DS100DF410 devices at I2C addr 0x27 on buses 18-69

log "=== Phase 3: Programming retimers ==="

# Program DS100DF410 retimers directly via I2C
# Register values captured from Cumulus 2.5.1 boot-time config (March 2026)
# See traces/cumulus_retimer_registers.txt for source
#
# Retimer buses:
#   QSFP: 18-21 (direct on mux 0x77)
#   SFP group 1 (swp1-4):   22-25
#   SFP group 2 (swp9-12):  30-33
#   SFP group 3 (swp17-20): 38-41
#   SFP group 4 (swp25-28): 46-49
#   SFP group 5 (swp33-36): 54-57
#   SFP group 6 (swp41-48): 62-69 (all 8 have retimers)

init_retimer() {
    local bus=$1
    i2cset -f -y $bus 0x27 0xFF 0x0F 2>/dev/null || return 1
    i2cset -f -y $bus 0x27 0x0A 0x10 2>/dev/null  # CDR control
    i2cset -f -y $bus 0x27 0x0B 0x0F 2>/dev/null  # CDR bandwidth
    i2cset -f -y $bus 0x27 0x0C 0x08 2>/dev/null  # CDR mode
    i2cset -f -y $bus 0x27 0x0E 0x93 2>/dev/null  # EQ/DFE config
    i2cset -f -y $bus 0x27 0x0F 0x69 2>/dev/null  # EQ boost
    i2cset -f -y $bus 0x27 0x10 0x3A 2>/dev/null
    i2cset -f -y $bus 0x27 0x11 0x20 2>/dev/null
    i2cset -f -y $bus 0x27 0x12 0xA0 2>/dev/null
    i2cset -f -y $bus 0x27 0x13 0x30 2>/dev/null
    i2cset -f -y $bus 0x27 0x15 0x10 2>/dev/null  # Output amplitude
    i2cset -f -y $bus 0x27 0x16 0x7A 2>/dev/null
    i2cset -f -y $bus 0x27 0x17 0x36 2>/dev/null
    i2cset -f -y $bus 0x27 0x18 0x40 2>/dev/null
    i2cset -f -y $bus 0x27 0x19 0x23 2>/dev/null
    i2cset -f -y $bus 0x27 0x1B 0x03 2>/dev/null
    i2cset -f -y $bus 0x27 0x1C 0x24 2>/dev/null
    i2cset -f -y $bus 0x27 0x1E 0xE9 2>/dev/null  # CTLE config
    i2cset -f -y $bus 0x27 0x1F 0x55 2>/dev/null
    i2cset -f -y $bus 0x27 0x23 0x40 2>/dev/null
    i2cset -f -y $bus 0x27 0x2A 0x30 2>/dev/null
    i2cset -f -y $bus 0x27 0x2C 0x72 2>/dev/null
    i2cset -f -y $bus 0x27 0x2D 0x80 2>/dev/null  # DFE mode
    i2cset -f -y $bus 0x27 0x2F 0x06 2>/dev/null
    i2cset -f -y $bus 0x27 0x31 0x20 2>/dev/null  # TX FIR
    i2cset -f -y $bus 0x27 0x32 0x11 2>/dev/null
    i2cset -f -y $bus 0x27 0x33 0x88 2>/dev/null
    i2cset -f -y $bus 0x27 0x34 0x3F 2>/dev/null
    i2cset -f -y $bus 0x27 0x35 0x1F 2>/dev/null
    i2cset -f -y $bus 0x27 0x36 0x01 2>/dev/null  # Channel config
    i2cset -f -y $bus 0x27 0x3A 0xA5 2>/dev/null
    i2cset -f -y $bus 0x27 0x3E 0x80 2>/dev/null
    return 0
}

retimer_count=0
# EdgeNOS kernel 5.10 bus numbers (depth-first enumeration)
# See I2C_BUS_NUMBER_MAPPING.md for Cumulus vs EdgeNOS mapping
#
# Retimer buses (0x27 on ports that have retimers):
#   QSFP: 66,67,68,69 (mux 0x77 ch0-3)
#   SFP group 1 (swp1-4):   11,12,13,14
#   SFP group 2 (swp9-12):  20,21,22,23
#   SFP group 3 (swp17-20): 29,30,31,32
#   SFP group 4 (swp25-28): 38,39,40,41
#   SFP group 5 (swp33-36): 47,48,49,50
#   SFP group 6 (swp41-48): 56,57,58,59,60,61,62,63 (all 8)
RETIMER_BUSES="66 67 68 69 11 12 13 14 20 21 22 23 29 30 31 32 38 39 40 41 47 48 49 50 56 57 58 59 60 61 62 63"
for bus in $RETIMER_BUSES; do
    if init_retimer $bus; then
        retimer_count=$((retimer_count + 1))
    fi
done
log "Programmed $retimer_count retimers with Cumulus-captured values"

# Fan speed: use CPLD sysfs if available, skip devmem (can crash eLBC/USB)
CPLD_SYSFS="/sys/devices/platform/as5610_52x_cpld"
if [ -f "$CPLD_SYSFS/pwm1" ]; then
    echo 128 > "$CPLD_SYSFS/pwm1" 2>/dev/null
    log "Fan set to medium via sysfs"
else
    log "WARN: CPLD sysfs not available, fan at default speed"
    # Do NOT use devmem for CPLD - it can hang the eLBC and crash USB
fi

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
