#!/bin/sh
# load-test.sh -- measure frame loss through the 7150 under sustained load.
#
# WHY THIS EXISTS AS A FILE. The loss figure was measured ad-hoc for several
# releases running, which is how two wrong conclusions got published: a
# "2.5% loss caused by the CM watermarks" (the baseline it was compared against
# was an RC=1 boot with OSPF not running) and an "upward trend" that was an
# artifact of using interface-counter deltas, which are contaminated by
# background traffic and once produced NEGATIVE loss (2007 forwarded of 2000).
#
# So: count with tcpdump on the EGRESS side, filtered to our own flow, never
# with interface counters.
#
#   AS5610 swp7 10.101.101.33 --> 7150 Et2 .34   ingress
#   7150 routes 10.102.1.0/24 via 10.101.101.25
#   7150 Et1 .26 --> AS5610 swp6 .25             egress, counted here
#
# ⚠ ARP must be primed in BOTH directions first or Linux backs off and sends
# only a handful of the packets -- that is what made alpha38 look broken.
#
# ⚠ The peer's ping is BUSYBOX: it has no -f, and -A needs replies (10.102.1.1
# has no host, so there are none). A flood-ping harness silently sends nothing
# and reports 100% loss, which looks exactly like a dead dataplane. The load is
# generated with a raw socket from python3 instead.
#
# usage: [COUNT=2000] [PASSES=3] tools/load-test.sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
COUNT="${COUNT:-2000}"
PASSES="${PASSES:-3}"
# Inter-packet gap in seconds. 0 = unpaced burst, which is a BUFFER test, not a
# forwarding test -- the two answer different questions and must not be compared
# to each other. Measured on alpha42: paced 2ms -> 1998/2000, paced 0.5ms ->
# 1995/2000, unpaced -> ~1165/2000. The dataplane forwards essentially losslessly
# when paced; the burst figure measures how much the chip can absorb.
GAP="${GAP:-0.002}"
DST=10.102.1.1

"$HERE/p5.sh" "ip route replace $DST/32 via 10.101.101.34 dev swp7" >/dev/null 2>&1

# ⚠ Prime ARP from the SWITCH, not just the peer. Switch-initiated pings refresh
# BOTH sides' entries; priming only from the peer leaves the switch's neighbour
# for 10.101.101.25 in FAILED, Linux backs off, and only a handful of the
# requested packets are ever sent. That is what once made alpha38 look broken.
"$HERE/eg.sh" 'ping -c 2 -W 1 -I et2 10.101.101.33 >/dev/null 2>&1
               ping -c 2 -W 1 -I et1 10.101.101.25 >/dev/null 2>&1' >/dev/null 2>&1
"$HERE/p5.sh" "ping -c 2 -W 1 -I 10.101.101.33 10.101.101.34 >/dev/null 2>&1" >/dev/null 2>&1

i=1
while [ "$i" -le "$PASSES" ]; do
    # capture, ping and read back in ONE remote shell: a tcpdump backgrounded
    # from a separate ssh dies with that session and captures nothing.
    N=$("$HERE/p5.sh" "rm -f /tmp/lt.pcap
        (tcpdump -i swp6 -s 0 -w /tmp/lt.pcap host $DST >/dev/null 2>&1 &)
        sleep 2
        python3 -c \"
import socket, struct, time
def ck(b):
    t = sum(struct.unpack('!%dH' % (len(b)//2), b))
    t = (t >> 16) + (t & 0xffff); t += t >> 16
    return ~t & 0xffff
s = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_ICMP)
s.bind(('10.101.101.33', 0))
gap = $GAP
for i in range($COUNT):
    h = struct.pack('!BBHHH', 8, 0, 0, 0x4242, i & 0xffff) + bytes(56)
    s.sendto(struct.pack('!BBHHH', 8, 0, ck(h), 0x4242, i & 0xffff) + bytes(56),
             ('$DST', 0))
    if gap:
        time.sleep(gap)
\" >/dev/null 2>&1
        sleep 3; killall tcpdump 2>/dev/null; sleep 1
        tcpdump -r /tmp/lt.pcap -nn 2>/dev/null | grep -c 'echo request'" 2>/dev/null | tr -d ' \r')
    LOSS=$(awk -v n="${N:-0}" -v c="$COUNT" 'BEGIN{printf "%.2f", (c-n)*100/c}')
    echo "pass $i: forwarded=${N:-0} of $COUNT  (loss ${LOSS}%, gap=${GAP}s)"
    i=$((i+1))
done
"$HERE/p5.sh" "ip route del $DST/32 via 10.101.101.34 dev swp7" >/dev/null 2>&1
