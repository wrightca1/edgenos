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
# ⚠ NOT the TAP's carrier. portd's TAP reports carrier=1 as soon as the
# interface is up, whatever the physical lane is doing -- measured 2026-08-15 on
# a boot where Et2's PORT_STATUS was 0x0815 and the lane never locked. Read the
# MAC's own PORT_STATUS instead: RxLinkUp is bit 6.
if ! edge '/mnt/flash/fmdump 0xe4000 1' | grep -qE '0(8|a|c|e)c0'; then
    echo "⛔ Et2 PORT_STATUS shows no link -- Et2 does not come up every boot."
    echo "   (a TAP carrier of 1 means nothing here; this reads the MAC.)"
    exit 1
fi

# ⚠ PRIME ARP IN BOTH DIRECTIONS FIRST.
#
# The peer must resolve 10.101.101.34 (the switch's et2) to send anything at all.
# Once a Linux neighbour entry goes FAILED, the kernel backs off and drops the
# pings rather than re-ARPing immediately -- so the capture comes back EMPTY and
# looks exactly like broken forwarding. That cost a false "alpha38 breaks the
# dataplane" on 2026-08-21: the same test also failed on the known-good image.
#
# Switch-initiated pings refresh both sides' entries. Cheap, and it removes the
# most likely reason for this harness to lie.
edge 'ping -c 2 -W 1 -I et2 10.101.101.33 >/dev/null 2>&1
      ping -c 2 -W 1 -I et1 10.101.101.25 >/dev/null 2>&1' >/dev/null 2>&1
peer "ping -c 2 -W 1 -I 10.101.101.33 10.101.101.34 >/dev/null 2>&1" >/dev/null 2>&1
echo "=== 1b. neighbour state (must not be FAILED) ==="
peer "ip neigh show dev swp7 | grep -F 10.101.101.34 || echo '  (no entry for 10.101.101.34)'"

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
