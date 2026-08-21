#!/bin/bash
# mod-slotsweep.sh <bank> <slot> [<slot>...] -- disable ONE MOD entry at a time and
# report the switch's egress bytes, masked for fields that vary per packet.
#
# Only ONE entry per slice fires per frame, so most slots change nothing; the
# firing one is the signal. Whole-slice disable is useless in slices 15/16 --
# it drops the frame outright (no entry matches at all, catch-all included).
#
# DECREMENT's signature: TTL stops changing (byte 22 -> 0x40) WITHOUT the rest of
# the frame shifting. If other stable bytes move instead, the entry is a SKIP that
# was positioning the decrement, not the decrement itself.
set -u
S="$(cd "$(dirname "$0")" && pwd)"
BANK=$1; shift
BASE=$(cat "$S/baseline.hex")

mask(){ python3 - "$1" <<'PY'
import sys
h=sys.argv[1]
if len(h) < 68: print("SHORT:"+h); raise SystemExit
b=[h[i:i+2] for i in range(0,len(h),2)]
for i in (18,19,24,25):            # IP id, IP checksum -- vary per packet
    if i < len(b): b[i]='..'
print(''.join(b[:34]))             # through the two IP addresses
PY
}

echo "baseline: $(mask "$BASE")   TTL=0x${BASE:44:2}"
echo
for slot in "$@"; do
  OUT=$("$S/mod-perturb.sh" slot "$BANK" "$slot" 2>&1)
  rc=$?
  HEX=$(echo "$OUT" | grep -E '^[0-9a-f]{40,}$' | head -1)
  if [ $rc -ne 0 ]; then
      printf "bank %s slot %2s -> ERROR rc=%s %s\n" "$BANK" "$slot" "$rc" "$(echo "$OUT" | tail -1)"
      exit $rc
  fi
  if [ -z "$HEX" ]; then
      printf "bank %s slot %2s -> NO FRAME\n" "$BANK" "$slot"
      continue
  fi
  M=$(mask "$HEX"); T="0x${HEX:44:2}"
  if [ "$M" = "$(mask "$BASE")" ]; then
      printf "bank %s slot %2s -> unchanged            TTL=%s\n" "$BANK" "$slot" "$T"
  else
      printf "bank %s slot %2s -> ***CHANGED***  TTL=%s\n              %s\n" "$BANK" "$slot" "$T" "$M"
  fi
done
