#!/bin/bash
# mod-swap.sh <bank> <slot> <cmdbyte> [<cmdbyte>...]
#
# Replace the COMMAND BYTE of one MOD_COMMAND_RAM entry, leaving Jitter and the
# Valid bit untouched, so the CAM still matches and ONLY the operation changes.
# Capture the egress frame for each byte, then restore and verify.
#
# This is how an opcode is decoded when no entry using it fires naturally: put it
# in a slot that IS known to fire (mod-bisect.sh finds those) and read the result.
#
# Guards, same as mod-bisect.sh and for the same reasons:
#   1. save must return exactly 32 values for the bank, else refuse
#   2. the transit path is confirmed alive before every probe, so an empty
#      capture means the command dropped the frame and not that the path died
#   3. restore is unconditional on exit, guarded on BASHPID so it does not fire
#      inside command substitution, and verified bit-identical afterwards
set -u
S="$(cd "$(dirname "$0")" && pwd)"
BANK=$1; SLOT=$2; shift 2
BAR=0xe2000000; RAM=0x159000
addr(){ echo $(( BAR + (RAM + 32*BANK + $1) * 4 )); }

SAVED=$(timeout 60 "$S/eg.sh" "for s in \$(seq 0 31); do printf '%s ' \$(devmem \$(( $BAR + ($RAM + 32*$BANK + s) * 4 )) 32); done; echo" 2>/dev/null | tr -s ' ')
N=$(echo $SAVED | wc -w)
[ "$N" -eq 32 ] || { echo "⛔ bank $BANK: save returned $N values, not 32 -- refusing"; exit 1; }
val_of(){ echo $SAVED | cut -d' ' -f$(( $1 + 1 )); }
ORIG=$(val_of "$SLOT")
[ $(( ORIG & 0x4000 )) -ne 0 ] || { echo "⛔ bank $BANK slot $SLOT has Valid clear -- wrong slot"; exit 1; }

restore(){ timeout 60 "$S/eg.sh" "devmem $(addr $SLOT) 32 $ORIG; true" >/dev/null 2>&1; }
MAINPID=$$
trap 'if [ "${BASHPID:-$$}" = "$MAINPID" ]; then restore; fi' EXIT INT TERM HUP

# ⚠ Prime et2 before every capture. et2 is the copper DAC port and its neighbour
# state lapses within a minute or two of idleness -- the PCS stays LOCKED
# (LANE_STATUS=0x940, pcsRx=1) while nothing passes, so a port-level check does
# NOT predict whether the transit path works. A short ping burst over et2
# immediately before the probe restores it reliably; a background keepalive does
# not survive long enough to be trusted.
prime(){ timeout 40 "$S/eg.sh" 'ping -c 3 -W 1 10.101.101.33 >/dev/null 2>&1; true' >/dev/null 2>&1; }

path_alive(){ local i H
  for i in 1 2 3; do
      prime
      H=$("$S/transit-probe-hex.sh"); [ -n "$H" ] && return 0
      sleep 5
  done
  return 1; }

printf 'bank %s slot %s  original word 0x%08x (cmd 0x%02x, jitter %d)\n' \
    "$BANK" "$SLOT" "$ORIG" $(( ORIG & 0xff )) $(( (ORIG >> 8) & 0x3f ))
BASE=$(cat "$S/baseline.hex" 2>/dev/null)
echo "baseline: $(( ${#BASE} / 2 )) bytes"

for CMD in "$@"; do
    NEW=$(( (ORIG & ~0xff) | (CMD & 0xff) ))
    if ! path_alive; then echo "⛔ transit path down before probing 0x$(printf %02x $CMD) -- aborting"; exit 4; fi
    timeout 60 "$S/eg.sh" "devmem $(addr $SLOT) 32 $NEW; true" >/dev/null 2>&1
    H=$("$S/transit-probe-hex.sh")
    restore
    if [ -z "$H" ]; then
        printf '  cmd 0x%02x (op%d operand %2d) -> DROPPED\n' "$CMD" $(( CMD >> 5 )) $(( CMD & 0x1f ))
    else
        printf '  cmd 0x%02x (op%d operand %2d) -> %3d bytes  %s\n' \
            "$CMD" $(( CMD >> 5 )) $(( CMD & 0x1f )) $(( ${#H} / 2 )) "${H:0:48}"
    fi
done

NOW=$(timeout 60 "$S/eg.sh" "devmem $(addr $SLOT) 32" 2>/dev/null | tr -d ' \r')
[ "$(( NOW ))" = "$(( ORIG ))" ] && echo "restore verified" || { echo "⛔ RESTORE MISMATCH: $NOW vs $ORIG"; exit 2; }
