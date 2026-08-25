#!/bin/bash
# transit-probe-hex.sh -- emit one transit frame; print the switch's EGRESS bytes as ONE
# contiguous lowercase hex string (no spaces), so byte N is chars 2N..2N+1.
# TTL is byte 22. Fields that legitimately vary per packet: IP id (18-19),
# IP cksum (24-25), ICMP cksum (36-37), ICMP id/seq (38-41), payload timestamp.
#
# ⚠ THE CAPTURE FILTER MUST NOT DEPEND ON ANYTHING THE EDIT CAN CHANGE.
# This filtered on `host 10.102.1.1`, which is the destination IP -- so any MOD
# perturbation that shifted the cursor corrupted the IP header, the frame stopped
# matching, and an emitted-but-mangled frame was reported as DROPPED. That made
# "dropped" and "mangled" indistinguishable, which is the whole question when
# decoding an opcode. Filter on the switch's source MAC instead: MOD's egress
# edits for a routed frame rewrite the MAC addresses to known values, and a
# cursor shift cannot make the frame stop coming from this port.
#
# `not multicast` excludes the switch's own OSPF hellos, which share the source MAC
# and would otherwise be captured instead of the transit frame -- the routed frame
# is unicast to the peer. Do NOT use `greater 90` for this: it silently matched
# nothing here and made a live path look dead.
S=$(dirname "$0")
MAC=${TRANSIT_MAC:-$(timeout 30 "$S/eg.sh" 'cat /sys/class/net/et1/address' 2>/dev/null | tr -d ' \r\n')}
[ -n "$MAC" ] || { echo "" ; exit 1; }
FILTER=${TRANSIT_FILTER:-"ether src $MAC and not multicast"}
timeout 90 "$S/p5.sh" "ip route replace 10.102.1.1/32 via 10.101.101.34 dev swp7 2>/dev/null
   rm -f /tmp/a4.pcap; (tcpdump -i swp6 -s0 -w /tmp/a4.pcap $FILTER >/dev/null 2>&1 &)
   sleep 2; ping -c 2 -s 56 -I 10.101.101.33 10.102.1.1 >/dev/null 2>&1
   sleep 2; killall tcpdump 2>/dev/null; sleep 1
   tcpdump -r /tmp/a4.pcap -nn -xx -c 1 2>/dev/null | sed -n \"2,20p\" | sed \"s/^[^:]*:[[:space:]]*//\" | tr -d \" \\n\"
   echo" 2>/dev/null | tr -d ' \r'
