#!/bin/bash
# retimer-init.sh - Initialize DS100DF410 retimers
#
# Applies equalization profiles to all retimers based on their DT label.
# Must run after retimer kernel modules are loaded.
#
# Profile assignment:
#   set_eq1 (default) - Most SFP ports (short traces)
#   set_eq2           - QSFP ports + sfp_rx_eq_10 (longer traces)

RETIMER_CLASS="/sys/class/retimer_dev"

log() { logger -t retimer-init "$*"; echo "retimer-init: $*"; }

set_eq1() {
    local dev="$1"
    echo 12 > "$dev/device/channels"        # Broadcast all channels
    echo 1  > "$dev/device/veo_clk_cdr_cap"  # No 25MHz ref clock
    echo 0  > "$dev/device/pfd_prbs_dfe"     # Unmute output, enable DFE
    echo 64 > "$dev/device/adapt_eq_sm"      # CTLE+DFE mode
    echo 28 > "$dev/device/cdr_rst"          # CDR reset assert
    usleep 20000                              # 20ms settling
    echo 16 > "$dev/device/cdr_rst"          # CDR reset release
}

set_eq2() {
    local dev="$1"
    echo 12 > "$dev/device/channels"
    echo 1  > "$dev/device/veo_clk_cdr_cap"
    echo 0  > "$dev/device/pfd_prbs_dfe"
    echo 64 > "$dev/device/adapt_eq_sm"
    echo 23 > "$dev/device/tap_dem"          # DEM pre-emphasis for longer traces
    echo 28 > "$dev/device/cdr_rst"
    usleep 20000
    echo 16 > "$dev/device/cdr_rst"
}

if [ ! -d "$RETIMER_CLASS" ]; then
    log "No retimer devices found (class not present)"
    exit 0
fi

count=0
for dev in "$RETIMER_CLASS"/retimer*; do
    [ -d "$dev" ] || continue
    [ -f "$dev/label" ] || continue

    label=$(cat "$dev/label")

    case "$label" in
        qsfp_*)
            log "Retimer $(basename "$dev") ($label): set_eq2 (QSFP)"
            set_eq2 "$dev"
            ;;
        sfp_rx_eq_10)
            log "Retimer $(basename "$dev") ($label): set_eq2 (long trace)"
            set_eq2 "$dev"
            ;;
        sfp_*)
            log "Retimer $(basename "$dev") ($label): set_eq1 (SFP default)"
            set_eq1 "$dev"
            ;;
        *)
            log "Retimer $(basename "$dev") ($label): unknown, applying set_eq1"
            set_eq1 "$dev"
            ;;
    esac

    count=$((count + 1))
done

log "Initialized $count retimers"
