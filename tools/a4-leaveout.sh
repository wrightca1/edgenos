#!/bin/bash
# a4-leaveout.sh <addr> <label> - clear one MOD step's Valid bit and read the wire.
#
# A4's decisive test, finally runnable: transit now works, so a frame's
# transformation is visible on the peer's swp6 (TTL 64 -> 63, checksum
# recomputed). Clear one command's Valid bit (MOD_COMMAND_RAM bit 14), send a
# frame, and see which transformation stops.
#
#   TTL stops decrementing -> that command is DECREMENT
#   checksum goes stale    -> that command is CHECKSUM
#   nothing changes        -> that step does not fire for this frame, or the
#                             command is neither
#
# ⚠ This writes a LIVE forwarding table. The original value is restored on exit
# via trap, including on interrupt -- do not remove that.
set -u
S="$(cd "$(dirname "$0")" && pwd)"
EG="$S/eg.sh"; P5="$S/p5.sh"
ADDR="$1"; LABEL="${2:-step}"
edge(){ timeout 60 "$EG" "$@" 2>/dev/null; }

orig=$(edge "/mnt/flash/fmdump $ADDR 1" | awk '{print $2}')
[ -n "$orig" ] || { echo "could not read $ADDR"; exit 1; }
echo "=== $LABEL @ $ADDR original=$orig ==="
restore(){ edge "fm6000reg 0000:02:00.0 $ADDR 0x$orig >/dev/null 2>&1; printf 'restored: '; /mnt/flash/fmdump $ADDR 1"; }
trap restore EXIT INT TERM

newv=$(printf '0x%x' $(( 0x$orig & ~0x4000 )))
echo "--- clearing Valid: $newv ---"
edge "fm6000reg 0000:02:00.0 $ADDR $newv >/dev/null 2>&1; printf 'now: '; /mnt/flash/fmdump $ADDR 1"

echo "--- transit frame, egress capture on swp6 ---"
timeout 90 "$P5" 'ip route replace 10.102.1.1/32 via 10.101.101.34 dev swp7 2>/dev/null
   rm -f /tmp/a4.pcap; (tcpdump -i swp6 -s0 -w /tmp/a4.pcap host 10.102.1.1 >/dev/null 2>&1 &)
   sleep 2; ping -c 3 -I 10.101.101.33 10.102.1.1 >/dev/null 2>&1
   sleep 2; killall tcpdump 2>/dev/null; sleep 1
   tcpdump -r /tmp/a4.pcap -nnvv -c 1 2>/dev/null | grep -E "ttl|cksum" | head -3
   ip route del 10.102.1.1/32 via 10.101.101.34 dev swp7 2>/dev/null' 2>/dev/null
