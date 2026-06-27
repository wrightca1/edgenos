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
#   5. at24                       - EEPROM (board + 48 SFP + 4 QSFP, all via at24)
#   6. gpio-pca953x               - GPIO expanders (SFP/QSFP control)
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
# 64MB matches Cumulus 2.5 (TO_THE_SILICON.md §6); falls back to 32M/16M/8M
# automatically inside the kernel module if dma_alloc_coherent fails.
load_mod linux-kernel-bde dma_size=64
load_mod linux-user-bde

# Chip die-temp sensor (depends on linux-kernel-bde for BAR0 ownership).
# Exposes /sys/class/hwmon/hwmonN/temp1_input in milli-Celsius.
load_mod linux-bde-tmon

# TUN: packet I/O interfaces
load_mod tun

# CPLD: system management
load_mod accton_as5610_52x_cpld

# I2C devices
# QSFP EEPROMs are declared "atmel,24c02" in the DTS so the at24 driver binds
# them (the old "sff,sff8436" had no driver in this 5.10 kernel and never bound).
load_mod at24

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

# ── QSFP RESET_L + MODSEL_L: bring modules 49-52 out of reset and select them ──
# VERIFIED on hardware 2026-06-03: this is what makes the QSFP optic EEPROMs at
# 0x50 ACK (all 4 read id=0x0D, CISCO-AVAGO AFBR-79EBPZ-CS2 40G-SR4, distinct SNs).
#
# Expander PCA9538 0x71 (mux 0x76 ch2 / bus 64) carries RESET_L[3:0] on pins 0-3
# and MODSEL_L[3:0] on pins 4-7 (matches Cumulus S10gpio_init: rst_l + modsel_l on
# the same chip). Set the whole port to output (config=0x00) with value 0x0F:
#   pins 0-3 = 1  -> RESET_L deasserted (modules out of reset)
#   pins 4-7 = 0  -> MODSEL_L asserted  (modules selected for the 2-wire mgmt bus)
# Expander 0x70 pins 0-3 = LPMODE[3:0]; drive 0 for high power (needed for 40G).
#
# NOTE: this REQUIRES the muxes to have i2c-mux-idle-disconnect (see the .dts).
# Without it, driving 0x71 pins 4-7 corrupted the SFP I2C path; with idle-disconnect
# the mux channels are isolated and driving the full 0x71 port is safe (SFP reads
# stay clean). GPIO_PCA953X is built-in and owns 0x70/0x71, so we drive them through
# the gpio sysfs (raw i2cset would fight the kernel driver).
for chip in /sys/class/gpio/gpiochip*; do
    label=$(cat "$chip/label" 2>/dev/null)
    base=$(cat "$chip/base" 2>/dev/null)
    case "$label" in
        64-0071)
            export_gpio $((base+0)) out 1    # RESET_L[0] high (swp49 out of reset)
            export_gpio $((base+1)) out 1    # RESET_L[1] (swp50)
            export_gpio $((base+2)) out 1    # RESET_L[2] (swp51)
            export_gpio $((base+3)) out 1    # RESET_L[3] (swp52)
            export_gpio $((base+4)) out 0    # MODSEL_L[0] low (swp49 selected)
            export_gpio $((base+5)) out 0    # MODSEL_L[1] (swp50)
            export_gpio $((base+6)) out 0    # MODSEL_L[2] (swp51)
            export_gpio $((base+7)) out 0    # MODSEL_L[3] (swp52)
            log "QSFP RESET_L deasserted + MODSEL_L selected (0x71=0x0F): base=$base"
            ;;
        64-0070)
            export_gpio $((base+0)) out 0    # LPMODE[0] low = high power (swp49)
            export_gpio $((base+1)) out 0    # LPMODE[1] (swp50)
            export_gpio $((base+2)) out 0    # LPMODE[2] (swp51)
            export_gpio $((base+3)) out 0    # LPMODE[3] (swp52)
            log "QSFP LPMODE set to high power (0x70 pins0-3=0): base=$base"
            ;;
    esac
done

# SFP TX_DISABLE via PCA9506 GPIO expanders (0x20/0x21/0x22/0x24 ONLY).
# EdgeNOS bus 64 = mux 0x76 ch2, bus 65 = mux 0x76 ch3. PCA9506: cfg reg 0x18+, out 0x08+.
# Set all outputs to 0 (TX_DISABLE = low = TX enabled).
# IMPORTANT: 0x23 is the QSFP control expander (NOT SFP TX) — do NOT zero it here.
# OpenNetworkLinux's init_equalizer leaves 0x23 outputs at their default HIGH; zeroing
# them holds the QSFP modules' control lines low (RESET/MODSEL asserted) so the optic
# EEPROM at 0x50 never ACKs. Handle 0x23 separately below.
for bus in 64 65; do
    for addr in 0x20 0x21 0x22 0x24; do
        if i2cget -y $bus $addr 0 b >/dev/null 2>&1; then
            for port in 0x18 0x19 0x1a 0x1b 0x1c; do
                i2cset -y $bus $addr $port 0x00 b 2>/dev/null
            done
            for port in 0x08 0x09 0x0a 0x0b 0x0c; do
                i2cset -y $bus $addr $port 0x00 b 2>/dev/null
            done
        fi
    done
done
log "SFP TX_DISABLE cleared (0x20/0x21/0x22/0x24 on buses 64-65)"

# QSFP control expander 0x23 (PCA9506 on bus 65 = mux 0x76 ch3).
# Per ONL init_equalizer: config ports 0 & 4 = output; drive outputs HIGH to
# deassert the QSFP control lines (RESET_L high / not held). Presence is input
# port 2 (reg 0x02). Mirrors ONL leaving 0x23 outputs at default 0xFF.
if i2cget -y 65 0x23 0 b >/dev/null 2>&1; then
    i2cset -y 65 0x23 0x18 0x00 b 2>/dev/null   # cfg port0 = output
    i2cset -y 65 0x23 0x1c 0x00 b 2>/dev/null   # cfg port4 = output
    i2cset -y 65 0x23 0x08 0xff b 2>/dev/null   # out port0 = high (deassert)
    i2cset -y 65 0x23 0x0c 0xff b 2>/dev/null   # out port4 = high (deassert)
    log "QSFP control 0x23 outputs deasserted (high)"
fi

# Now that QSFP MODSEL (0x71) + control (0x23) are set, (re)bind at24 to the QSFP
# EEPROMs. at24 was modprobed in Phase 1 BEFORE these GPIOs, so its initial probe of
# the QSFP 0x50 clients failed (modules not yet selected/deasserted) and won't retry
# on its own — bind them explicitly here.
for b in 66 67 68 69; do
    echo "$b-0050" > /sys/bus/i2c/drivers/at24/bind 2>/dev/null || true
done
log "QSFP EEPROM at24 (re)bind attempted (buses 66-69)"

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
    # Match Cumulus S20retimer_init.sh sequence exactly:
    # 1. Select channels (reg 0xFF)
    # 2. Set veo_clk_cdr_cap (reg 0x36)
    # 3. CDR reset: assert (28) then deassert (16) via reg 0x0A
    # 4. Program all other registers
    # Without CDR reset, the retimer CDR never locks and no signal
    # passes from the SFP to the ASIC!
    i2cset -f -y $bus 0x27 0xFF 0x0C 2>/dev/null || return 1  # channels=12
    i2cset -f -y $bus 0x27 0x36 0x01 2>/dev/null  # veo_clk_cdr_cap=1
    i2cset -f -y $bus 0x27 0x0A 0x1C 2>/dev/null  # CDR reset ASSERT (28)
    i2cset -f -y $bus 0x27 0x0A 0x10 2>/dev/null  # CDR reset DEASSERT (16)
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
    # reg 0x36 (veo_clk_cdr_cap) already set above before CDR reset
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
        # Two passes: program everything first (with output still muted),
        # then release CDR reset all at once. Matches Cumulus's
        # S20retimer_init.sh sequence in /etc/init.d.
        for i in $(seq 0 $((NUM_RETIMERS - 1))); do
            rdev="${RDIR}/retimer${i}/device"
            [ -d "$rdev" ] || continue
            label=$(cat "${RDIR}/retimer${i}/label" 2>/dev/null)
            cd "$rdev"

            echo 12 > channels         2>/dev/null || true   # broadcast all 4 ch
            echo 1  > veo_clk_cdr_cap  2>/dev/null || true   # no 25 MHz ref clk

            # pfd_prbs_dfe=0 UNMUTES the retimer's output to the ASIC
            # and enables DFE — without this PCS block_lock stays 0
            # regardless of CDR state. adapt_eq_sm=64 puts the RX in
            # CTLE+DFE adaptive-equalization mode (mandatory for 10G).
            echo 0  > pfd_prbs_dfe     2>/dev/null || true
            echo 64 > adapt_eq_sm      2>/dev/null || true

            # QSFP and SFP RX EQ get additional tap_dem pre-emphasis
            case "$label" in
                qsfp*|sfp_rx_eq_*)
                    echo 23 > tap_dem  2>/dev/null || true
                    ;;
            esac

            echo 28 > cdr_rst          2>/dev/null || true   # CDR rst ASSERT
        done
        usleep 20000  # 20 ms settling before deassert (matches Cumulus)
        for i in $(seq 0 $((NUM_RETIMERS - 1))); do
            rdev="${RDIR}/retimer${i}/device"
            [ -d "$rdev" ] || continue
            echo 16 > "${rdev}/cdr_rst" 2>/dev/null || true  # CDR rst RELEASE
        done
        log "Programmed $NUM_RETIMERS retimers (pfd_prbs_dfe=0, adapt_eq_sm=64, CDR cycled)"
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
