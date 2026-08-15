#!/bin/bash
# transit-test.sh -- prove traffic TRANSITS the 7150, and capture what it emits.
#
# This is the prerequisite checklist A4 (the MOD command split) and B1 (the FFU
# ByteMux sources) have been blocked on. Both need to observe how a frame is
# TRANSFORMED, which needs a frame that goes in one port and out another. The
# checklist says "the 7150 currently has no egress capture point"; it does now.
#
#   AS5610 swp7 10.101.101.33 --> 7150 Et2 .34  ingress
#   7150 routes 10.102.1.0/24 via 10.101.101.25
#   7150 Et1 .26 --> AS5610 swp6 .25            egress, captured here
#
# The peer is BOTH ends, so a /32 route is what forces the hairpin: without it
# the peer delivers locally and nothing touches the switch. Removed on exit.
set -u
S="$(cd "$(dirname "$0")" && pwd)"
EG="$S/eg.sh"; P5="$S/p5.sh"
DEST="${DEST:-10.102.1.1}"
CAP=/tmp/transit.pcap

edge(){ timeout 45 "$EG" "$@" 2>/dev/null; }
peer(){ timeout 60 "$P5" "$@" 2>/dev/null; }

echo "=== 1. both ports up on the switch? ==="
edge 'for i in et1 et2; do
        [ -e /sys/class/net/$i ] || { echo "  $i MISSING"; continue; }
        echo "  $i carrier=$(cat /sys/class/net/$i/carrier 2>/dev/null) $(ip -4 addr show $i | grep -o "inet [0-9./]*")"
      done'
if ! edge 'cat /sys/class/net/et2/carrier 2>/dev/null' | grep -q 1; then
    echo "⛔ et2 has no carrier -- Et2 does not come up every boot. Reboot and retry."
    exit 1
fi

echo "=== 2. switch route for $DEST (must leave via et1) ==="
edge "ip route get $DEST"

echo "=== 3. force the hairpin on the peer ==="
peer "ip route replace $DEST/32 via 10.101.101.34 dev swp7"
peer "ip route get $DEST"
trap 'peer "ip route del $DEST/32 via 10.101.101.34 dev swp7" >/dev/null 2>&1' EXIT

echo "=== 4. capture on swp6 (the 7150's EGRESS) while pinging in via swp7 ==="
peer "rm -f $CAP; (tcpdump -i swp6 -s 0 -w $CAP host $DEST >/dev/null 2>&1 &) ; sleep 2
      ping -c 5 -I 10.101.101.33 $DEST >/tmp/transit-ping.txt 2>&1
      sleep 2; killall tcpdump 2>/dev/null; sleep 1
      echo '--- ping ---'; tail -3 /tmp/transit-ping.txt
      echo '--- captured on swp6 ---'; tcpdump -r $CAP -nne 2>/dev/null | head -12"

echo "=== 5. byte-level view of one emitted frame ==="
peer "tcpdump -r $CAP -nnex -c 1 2>/dev/null | head -20"

echo "=== 6. switch-side counters ==="
edge 'for i in et1 et2; do
        echo "  $i rx=$(cat /sys/class/net/$i/statistics/rx_packets 2>/dev/null) tx=$(cat /sys/class/net/$i/statistics/tx_packets 2>/dev/null)"
      done'
