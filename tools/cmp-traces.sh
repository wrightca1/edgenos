#!/bin/bash
# cmp-traces.sh - diff a GOOD Et2 boot against a dark one, ignoring the noise.
#
# Validated first: two dark boots differ ONLY in bit 10 (Receiving) of each
# port's PORT_STATUS, which is constant within a boot and varies between boots
# on Et1 too. Everything else in the trace is byte-identical across dark boots,
# so any surviving difference against a good boot is real.
set -eu
S="$(cd "$(dirname "$0")" && pwd)/traces"
good=$(ls "$S"/GOOD-boot-*.log 2>/dev/null | head -1)
dark=$(ls "$S"/dark-boot-*.log 2>/dev/null | head -1)
[ -n "${good:-}" ] || { echo "no GOOD trace yet"; exit 1; }
echo "good: $good"; echo "dark: $dark"; echo
# mask bit 10 of any 8-hex-digit PORT_STATUS so the known-noisy bit cannot
# manufacture differences
mask(){ sed -E 's/PORT_STATUS=0000([0-9a-f])([0-9a-f])/PORT_STATUS=0000\1\2/; s/=00000cc0/=000008c0/g; s/=00000c15/=00000815/g'; }
echo "=== differences, bit 10 masked ==="
diff <(mask < "$dark") <(mask < "$good") || true
echo
echo "=== the question this answers ==="
echo "  Et2 up already at STEP5 -> divergence is inside the replay"
echo "  Et2 climbing during STEP7 -> training-time race"
grep -E "STEP5|rc=0 PIN=.*PORT_STATUS" "$good" | head -3
