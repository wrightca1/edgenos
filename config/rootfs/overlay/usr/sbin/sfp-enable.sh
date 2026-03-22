#!/bin/bash
# sfp-enable.sh - Enable SFP/QSFP transceivers and initialize retimers
#
# 1. Configure PCA9506 TX_DISABLE as outputs, drive LOW (enable TX)
# 2. Program DS100DF410 retimers via I2C for proper signal equalization
# 3. Configure QSFP GPIO (PCA9538) for module enable
#
# Must run AFTER mdk-init (ASIC needs to be initialized first).

log() { logger -t sfp-enable "$*"; echo "sfp-enable: $*"; }

# ── SFP TX Enable ────────────────────────────────────────────
# PCA9506 registers:
#   0x08-0x0c: Output Port (write value to drive pins)
#   0x18-0x1c: I/O Config  (0=output, 1=input; default 0xff=all input)
#
# Bus 65, addr 0x21 = TX_DISABLE for ports 1-40
# Active HIGH = TX disabled, so drive LOW to enable TX

sfp_tx_enable() {
    log "Enabling SFP TX (bus 65, addr 0x21)..."

    # Set ports 1-40 TX_DISABLE pins as outputs, driven LOW
    for reg_offset in 0 1 2 3 4; do
        config_reg=$((0x18 + reg_offset))
        output_reg=$((0x08 + reg_offset))
        # Config: 0x00 = all outputs
        i2cset -y 65 0x21 $config_reg 0x00 2>/dev/null
        # Output: 0x00 = TX_DISABLE deasserted (TX enabled)
        i2cset -y 65 0x21 $output_reg 0x00 2>/dev/null
    done

    # Also handle ports 41-48 TX_DISABLE via addr 0x23
    # reg 0x18 (config port 0) → outputs for TX_DISABLE
    i2cset -y 65 0x23 0x18 0x00 2>/dev/null
    i2cset -y 65 0x23 0x08 0x00 2>/dev/null

    log "SFP TX enabled for all ports"
}

# ── QSFP Enable ──────────────────────────────────────────────
# PCA9538 on bus 64 at 0x70-0x73 (one per QSFP port 49-52)
# Bit 0: MODSEL_L (0=selected), Bit 1: RST_L (1=not reset), Bit 2: LPMODE (0=high power)

qsfp_enable() {
    log "Enabling QSFP modules (bus 64)..."

    for addr in 0x70 0x71 0x72 0x73; do
        # Config: bit3=input(INT_L), bits 0-2=output → 0x08
        i2cset -y 64 $addr 0x03 0x08 2>/dev/null
        # Output: RST_L=1, MODSEL_L=0, LPMODE=0 → 0x02
        i2cset -y 64 $addr 0x01 0x02 2>/dev/null
    done

    log "QSFP modules enabled"
}

# ── Retimer Init ─────────────────────────────────────────────
# DS100DF410 at 0x27 on per-port I2C buses
# Register map:
#   0xFF: Channel select (12=broadcast all 4 channels)
#   0x36: VEO_CLK_CDR_CAP (1=no 25MHz ref)
#   0x1E: PFD_PRBS_DFE (0=unmute, enable DFE)
#   0x31: ADAPT_EQ_SM (64=CTLE+DFE mode)
#   0x15: TAP_DEM (23=pre-emphasis for long traces)
#   0x0A: CDR_RST (28=assert, 16=release)

retimer_init_eq1() {
    local bus=$1
    i2cset -y $bus 0x27 0xFF 12 2>/dev/null  # Broadcast all channels
    i2cset -y $bus 0x27 0x36 1  2>/dev/null  # No 25MHz ref clock
    i2cset -y $bus 0x27 0x1E 0  2>/dev/null  # Unmute, enable DFE
    i2cset -y $bus 0x27 0x31 64 2>/dev/null  # CTLE+DFE mode
    i2cset -y $bus 0x27 0x0A 28 2>/dev/null  # CDR reset assert
    sleep 0.02
    i2cset -y $bus 0x27 0x0A 16 2>/dev/null  # CDR reset release
}

retimer_init_eq2() {
    local bus=$1
    i2cset -y $bus 0x27 0xFF 12 2>/dev/null
    i2cset -y $bus 0x27 0x36 1  2>/dev/null
    i2cset -y $bus 0x27 0x1E 0  2>/dev/null
    i2cset -y $bus 0x27 0x31 64 2>/dev/null
    i2cset -y $bus 0x27 0x15 23 2>/dev/null  # DEM pre-emphasis (long traces)
    i2cset -y $bus 0x27 0x0A 28 2>/dev/null
    sleep 0.02
    i2cset -y $bus 0x27 0x0A 16 2>/dev/null
}

retimer_init_all() {
    log "Initializing DS100DF410 retimers..."

    # SFP retimers (eq1 profile) - first 4 ports of each mux group
    local count=0
    for bus in 11 12 13 14 20 21 22 23 29 30 31 32 38 39 40 41 47 48 49 50 56 57 58 59 60 61 62 63; do
        if i2cget -y $bus 0x27 0x00 >/dev/null 2>&1; then
            retimer_init_eq1 $bus
            count=$((count + 1))
        fi
    done
    log "  SFP retimers: $count initialized (eq1)"

    # QSFP retimers (eq2 profile)
    local qcount=0
    for bus in 66 67 68 69; do
        if i2cget -y $bus 0x27 0x00 >/dev/null 2>&1; then
            retimer_init_eq2 $bus
            qcount=$((qcount + 1))
        fi
    done
    log "  QSFP retimers: $qcount initialized (eq2)"
}

# ── Main ─────────────────────────────────────────────────────
log "Starting SFP/QSFP enable and retimer initialization"

sfp_tx_enable
qsfp_enable
retimer_init_all

log "Done. Check link status with: mdk-init -p"
