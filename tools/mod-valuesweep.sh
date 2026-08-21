#!/bin/bash
# mod-valuesweep.sh <bank>... -- which MOD_VALUE_RAM banks (= data slices) does this
# flow actually consume?
#
# Each bank is one data slice (bank N = CAM data slice N+17), and only ONE entry
# per slice fires. Marking every CONSTANT word in a bank with 0xFFFFFFFF makes any
# consumed Type-1 byte show up in the emitted frame as 0xff. Selects words (odd
# slots) are left alone, so Type-2 channel bytes are untouched and the entry still
# matches -- this changes content, not structure.
#
# Guards, after the MAPPER incident and the bank-11 half-restore:
#   * save must return exactly 32 words, else refuse
#   * restore runs from an EXIT/INT/TERM trap, guarded on BASHPID so command
#     substitution subshells cannot fire it mid-probe
#   * restore is verified word-for-word, and a health check follows
set -u
S="$(cd "$(dirname "$0")" && pwd)"
BAR=0xe2000000; VR=0x159400
BASE=$(cat "$S/baseline.hex")
mask(){ echo "${1:0:36}xxxx${1:40:8}xxxx${1:52:16}"; }
BASEM=$(mask "$BASE")
MAINPID=$$

for BANK in "$@"; do
  SAVED=$(timeout 60 "$S/eg.sh" "for s in \$(seq 0 31); do printf '%s ' \$(devmem \$(( $BAR + ($VR + 32*$BANK + s) * 4 )) 32); done; echo" 2>/dev/null | tr -s ' ')
  n=$(echo $SAVED | wc -w)
  [ "$n" -eq 32 ] || { echo "bank $BANK: save returned $n not 32 -- skipped"; continue; }

  RES=""; MARK=""; i=0
  for v in $SAVED; do
    a=$(( BAR + (VR + 32*BANK + i) * 4 ))
    RES="$RES devmem $a 32 $v;"
    [ $((i % 2)) -eq 0 ] && MARK="$MARK devmem $a 32 0xFFFFFFFF;"
    i=$((i+1))
  done
  restore(){ timeout 60 "$S/eg.sh" "$RES true" >/dev/null 2>&1; }
  trap 'if [ "${BASHPID:-$$}" = "$MAINPID" ]; then restore; fi' EXIT INT TERM HUP

  timeout 60 "$S/eg.sh" "$MARK true" >/dev/null 2>&1
  H=$("$S/transit-probe-hex.sh" 2>/dev/null || "$S/probe-hex.sh")
  restore
  trap - EXIT INT TERM HUP

  NOW=$(timeout 60 "$S/eg.sh" "for s in \$(seq 0 31); do printf '%s ' \$(devmem \$(( $BAR + ($VR + 32*$BANK + s) * 4 )) 32); done; echo" 2>/dev/null | tr -s ' ')
  [ "$(echo $SAVED)" = "$(echo $NOW)" ] || { echo "bank $BANK: ⛔ RESTORE MISMATCH -- stopping"; exit 2; }

  if   [ -z "$H" ];                      then echo "bank $BANK (slice $((BANK+17))) -> FRAME DROPPED  *** consumed ***"
  elif [ "$(mask "$H")" != "$BASEM" ];   then echo "bank $BANK (slice $((BANK+17))) -> CHANGED $(mask "$H")  *** consumed ***"
  else                                        echo "bank $BANK (slice $((BANK+17))) -> unchanged"
  fi
done
H=$("$S/probe-hex.sh"); [ -n "$H" ] && echo "health ok" || echo "⛔ WEDGED"
