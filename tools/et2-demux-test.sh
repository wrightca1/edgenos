#!/bin/bash
# et2-demux-test.sh - does src_word=1 actually demux Et2's frames to et2?
#
# et2 rx=0 is NOT by itself evidence the demux is broken. The peer transmitting
# on swp7 only means frames reach Et2's MAC; nothing says they are PUNTED to the
# CPU. Frames only punt when the switch has a reason to send them up -- an ARP
# for its own et2 address does exactly that, and it must arrive on et2's TAP for
# the neighbour to resolve.
#
#   neighbour for 10.101.101.33 resolves + et2 rx > 0 -> demux works
#   et2 rx stays 0 while et1 rx climbs                -> still landing on et1
#   neither climbs                                    -> nothing is punted from
#                                                        Et2 at all; the demux is
#                                                        untested, not broken
set -u
S="$(cd "$(dirname "$0")" && pwd)"
EG="$S/eg.sh"; P5="$S/p5.sh"
edge(){ timeout 60 "$EG" "$@" 2>/dev/null; }

echo "=== before ==="
edge 'for i in et1 et2; do echo "  $i rx=$(cat /sys/class/net/$i/statistics/rx_packets)"; done
      echo -n "  Et2 PORT_STATUS: "; /mnt/flash/fmdump 0xe4000 1
      ip neigh show dev et2'

echo "=== force ARP + ICMP toward the switch on the Et2 subnet ==="
edge 'ip neigh flush dev et2 2>/dev/null; ping -c4 -W1 -I et2 10.101.101.33 >/dev/null 2>&1 &' >/dev/null
timeout 60 "$P5" 'ping -c 6 -W 1 -I 10.101.101.33 10.101.101.34 2>&1 | tail -3
                  echo "--- peer neigh for the switch ---"; ip neigh show dev swp7'
sleep 3

echo "=== after ==="
edge 'for i in et1 et2; do echo "  $i rx=$(cat /sys/class/net/$i/statistics/rx_packets)"; done
      echo "  switch neigh:"; ip neigh show | grep -E "101.101.(25|33)"'
