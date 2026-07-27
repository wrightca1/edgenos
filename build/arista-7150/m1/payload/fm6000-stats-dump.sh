#!/bin/sh
# fm6000-stats-dump.sh - read the FM6000 STATS counters (for DROP_CODE diagnosis).
#
# Reads the STATS_DISCRETE frame counters (0x1A000, 64 x 64-bit) which are the
# drop/diagnostic counters the STATS action-resolution stage increments per frame
# (keyed on RX_KEY_DROP_CODE etc.). Print non-zero counters. Run BEFORE and AFTER
# a single inject and diff to see which counter (=> which drop reason/stage) fired.
#
# Usage:
#   fm6000-stats-dump.sh            # dump non-zero STATS_DISCRETE frame counters
#   fm6000-stats-dump.sh bank N C   # read one STATS_BANK counter: bank N, counter C
# See arista notes phase48 for the code->stage decode + the STATS_AR config needed
# for per-drop-code counting. SPDX-License-Identifier: GPL-2.0-or-later
B=0000:02:00.0
R(){ fm6000reg $B $1 2>/dev/null | grep -o '[0-9a-f]*$'; }

if [ "${1:-}" = "bank" ]; then
    # STATS_BANK(bank,counter,word) = 0x200000 + (bank*2048 + counter)*2 + word
    bank=$2; ctr=$3
    off=$(printf '0x%x' $(( 0x200000 + (bank*2048 + ctr)*2 )))
    lo=$(R $off); hi=$(R $(printf '0x%x' $(( off + 1 ))))
    echo "STATS_BANK[bank=$bank ctr=$ctr] @ $off = 0x${hi:-0}${lo:-0}"
    exit 0
fi

echo "== STATS_DISCRETE frame counters (0x1A000, non-zero) =="
i=0
while [ $i -lt 64 ]; do
    off=$(printf '0x%x' $(( 0x1A000 + i*2 )))
    lo=$(R $off); hi=$(R $(printf '0x%x' $(( 0x1A000 + i*2 + 1 ))))
    if [ "${lo:-0}" != "0" ] || [ "${hi:-0}" != "0" ]; then
        echo "  DISCRETE[$i] @ $off = 0x${hi:-00000000}${lo:-00000000}"
    fi
    i=$((i+1))
done
echo "== (all-zero => nothing counted; STATS_AR likely unconfigured — see phase48) =="
