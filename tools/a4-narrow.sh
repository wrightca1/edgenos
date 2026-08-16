#!/bin/bash
# a4-narrow.sh <slice> - find WHICH entry in a slice performs the edit.
#
# Run after a4-slice-sweep.sh identifies a slice whose removal changes the TTL
# (or the checksum). A slice has 32 entries but only the VALID ones can fire, and
# EOS populates far fewer than 32, so this walks the valid ones one at a time
# rather than binary-searching -- with ~20 candidates at ~20s each that is
# cheaper than the bookkeeping, and a single-register perturbation is the safe
# variant (a4-leaveout.sh) rather than the bank-wide one that wedged the box.
#
# Reports the COMMAND BYTE of the entry that matters, which is A4's answer.
set -u
S="$(cd "$(dirname "$0")" && pwd)"
EG="$S/eg.sh"
SLICE="${1:?usage: a4-narrow.sh <slice>}"
base=$((0x159000 + 0x20*SLICE))
edge(){ timeout 60 "$EG" "$@" 2>/dev/null; }

echo "=== valid entries in slice $SLICE ==="
list=""
for s in $(seq 0 31); do
    a=$(printf 0x%x $((base+s)))
    v=$(edge "/mnt/flash/fmdump $a 1" | awk '{print $2}')
    [ -n "$v" ] || continue
    dec=$((0x$v))
    if [ $(( dec & 0x4000 )) -ne 0 ]; then
        cmd=$(printf '%02x' $(( dec & 0xff )))
        echo "  slot $s  addr $a  cmd $cmd"
        list="$list $a:$s:$cmd"
    fi
done
[ -n "$list" ] || { echo "no valid entries"; exit 1; }

echo
echo "=== leave-one-out over those entries ==="
for e in $list; do
    a=${e%%:*}; rest=${e#*:}; s=${rest%%:*}; cmd=${rest##*:}
    out=$(timeout 200 bash "$S/a4-leaveout.sh" "$a" "slice$SLICE-slot$s-$cmd" 2>&1)
    ttl=$(echo "$out" | grep -oE "ttl [0-9]+" | head -1)
    bad=$(echo "$out" | grep -c "bad cksum")
    flag=""
    [ "$ttl" != "ttl 63" ] && [ -n "$ttl" ] && flag="  <<< TTL CHANGED -- cmd $cmd is the DECREMENT"
    [ -z "$ttl" ] && flag="  <<< NO FRAME -- cmd $cmd is load-bearing"
    [ "$bad" != "0" ] && flag="$flag  <<< BAD CKSUM -- cmd $cmd fixes the checksum"
    echo "  slot $s cmd $cmd -> ${ttl:-none}${flag}"
done
