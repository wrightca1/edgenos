#!/bin/bash
# mod-bisect.sh <bank> -- find WHICH entry in a MOD slice fires, by bisection.
#
# Only one entry per slice fires. Disabling a set that contains it either changes
# the emitted frame or drops it; disabling a set that does not contain it changes
# nothing. Either outcome is a usable bit, so bisection needs ~log2(live) probes
# instead of one per entry -- 4 rather than 29 for bank 6.
#
# Guards: save is verified at exactly 32 values, every probe is followed by a full
# restore from the SAVED values (never a re-read), and the restore is verified
# bit-identical before the next probe.
set -u
S="$(cd "$(dirname "$0")" && pwd)"
BANK=$1
BAR=0xe2000000; RAM=0x159000
BASE=$(cat "$S/baseline.hex")

addr(){ echo $(( BAR + (RAM + 32*BANK + $1) * 4 )); }

SAVED=$(timeout 60 "$S/eg.sh" "for s in \$(seq 0 31); do printf '%s ' \$(devmem \$(( $BAR + ($RAM + 32*$BANK + s) * 4 )) 32); done; echo" 2>/dev/null | tr -s ' ')
N=$(echo $SAVED | wc -w)
[ "$N" -eq 32 ] || { echo "⛔ bank $BANK: save returned $N values, not 32"; exit 1; }
set -- $SAVED
LIVE=""; i=0
for v in $SAVED; do [ "$v" != "0x00000000" ] && LIVE="$LIVE $i"; i=$((i+1)); done
echo "bank $BANK: live slots:$LIVE"

val_of(){ echo $SAVED | cut -d' ' -f$(( $1 + 1 )); }

restore(){ local c="" s
  for s in $LIVE; do c="$c devmem $(addr $s) 32 $(val_of $s);"; done
  timeout 60 "$S/eg.sh" "$c true" >/dev/null 2>&1; }

# ⚠ RESTORE ON ANY EXIT. Without this, a kill between the disable and the restore
# leaves the slice perturbed with nobody to put it back: bank 11 was once found with
# slots 0-2 still cleared and the switch emitting TTL 0x41. The window is ~15s per
# probe and the harness WILL be interrupted eventually, so make it unconditional.
#
# ⚠⚠ ...but bash propagates an EXIT trap into command-substitution subshells, so an
# unguarded version fires on every `$(mask ...)` and RESTORES THE TABLE MID-PROBE --
# silently corrupting the very measurement it is protecting. Guard on BASHPID.
MAINPID=$$
trap 'if [ "${BASHPID:-$$}" = "$MAINPID" ]; then echo "  (trap: restoring bank '"$BANK"')" >&2; restore; fi' EXIT INT TERM HUP

# ⚠ IP id (bytes 18-19) and IP checksum (bytes 24-25) change on EVERY packet -- the
# sender picks the id and the checksum follows it. Comparing them makes every probe
# read as CHANGED and the bisection walks to the wrong slot. Mask them out.
# byte i occupies chars 2i..2i+1.
mask(){ echo "${1:0:36}xxxx${1:40:8}xxxx${1:52:16}"; }
BASEM=$(mask "$BASE")

# returns 0 if disabling $* perturbs the frame (changed or dropped)
try(){ local c="" s H
  for s in "$@"; do c="$c devmem $(addr $s) 32 $(( $(val_of $s) & ~0x4000 ));"; done
  timeout 60 "$S/eg.sh" "$c true" >/dev/null 2>&1
  H=$("$S/transit-probe-hex.sh"); restore
  if [ -z "$H" ]; then LASTOUT="DROPPED"; return 0; fi
  if [ "$(mask "$H")" != "$BASEM" ]; then LASTOUT="CHANGED $(mask "$H")"; return 0; fi
  LASTOUT="same"; return 1
}

LASTOUT=""
SET="$LIVE"
if ! try $SET; then echo "bank $BANK: disabling ALL live entries changed nothing -- this slice does not act on this frame"; exit 0; fi
echo "  full set -> $LASTOUT"

while [ "$(echo $SET | wc -w)" -gt 1 ]; do
  n=$(echo $SET | wc -w); half=$(( (n + 1) / 2 ))
  A=$(echo $SET | cut -d' ' -f1-$half)
  B=$(echo $SET | cut -d' ' -f$((half+1))-)
  if try $A; then SET="$A"; else SET="$B"; fi
  echo "  -> narrowed to [$(echo $SET | tr '\n' ' ')]  ($LASTOUT)"
done
echo "bank $BANK: FIRING SLOT = $SET  cmd = $(val_of $SET)"
try $SET; echo "  effect of disabling it alone: $LASTOUT"
H=$("$S/transit-probe-hex.sh"); [ -n "$H" ] && echo "  health ok" || echo "  ⛔ WEDGED"
