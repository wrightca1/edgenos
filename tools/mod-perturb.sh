#!/bin/bash
# mod-perturb.sh -- disable a set of MOD_COMMAND_RAM entries, capture the switch's
# egress bytes, restore, and verify the restore. Then health-check.
#
#   mod-perturb.sh bank <b> [<b>...]      disable whole bank(s)
#   mod-perturb.sh slot <bank> <slot>     disable ONE entry
#
# Guards, all three from the 2026-08-15 incident that wedged the box:
#   1. the save must return EXACTLY 32 values per bank, else refuse
#   2. restore is verified bit-identical against the saved values
#   3. a health check runs after restore; a wedge is terminal (reboot)
#
# Writes go through devmem: byte = BAR0 + word*4, BAR0 = 0xe2000000.
# Valid bit is 0x4000; disabling clears it.
set -u
S="$(cd "$(dirname "$0")" && pwd)"
MODE=$1; shift
BASE=0x159000; BAR=0xe2000000

save_bank(){ timeout 60 "$S/eg.sh" "
B=\$(($BAR))
for s in \$(seq 0 31); do printf '%s ' \$(devmem \$((B + ($BASE + 32*$1 + s) * 4)) 32); done; echo" 2>/dev/null | tr -s ' '; }

declare -A SAVED
BANKS=""
case "$MODE" in
  bank) BANKS="$*" ;;
  slot) BANKS="$1" ;;
esac

for b in $BANKS; do
  v=$(save_bank "$b")
  n=$(echo $v | wc -w)
  [ "$n" -eq 32 ] || { echo "⛔ bank $b: save returned $n values, not 32 -- refusing"; exit 1; }
  SAVED[$b]="$v"
done

DIS=""; RES=""
for b in $BANKS; do
  i=0
  for v in ${SAVED[$b]}; do
    addr=$(( BAR + (BASE + 32*b + i) * 4 ))
    RES="$RES devmem $addr 32 $v;"
    if [ "$MODE" = bank ] || { [ "$MODE" = slot ] && [ "$i" = "${2:-}" ]; }; then
      DIS="$DIS devmem $addr 32 $(( v & ~0x4000 ));"
    fi
    i=$((i+1))
  done
done

timeout 60 "$S/eg.sh" "$DIS true" >/dev/null 2>&1
OUT=$("$S/transit-probe-hex.sh")
timeout 60 "$S/eg.sh" "$RES true" >/dev/null 2>&1

for b in $BANKS; do
  now=$(save_bank "$b")
  [ "$(echo ${SAVED[$b]})" = "$(echo $now)" ] || { echo "⛔ bank $b RESTORE MISMATCH"; exit 2; }
done

echo "$OUT"
# health check: a restored table does not un-wedge a dataplane in flight
H=$("$S/transit-probe-hex.sh")
[ -n "$(echo $H)" ] || { echo "⛔ WEDGED -- restore did not recover. Reboot required."; exit 3; }
