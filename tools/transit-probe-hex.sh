#!/bin/bash
# transit-probe-hex.sh -- emit one transit frame; print the switch's EGRESS bytes as ONE
# contiguous lowercase hex string (no spaces), so byte N is chars 2N..2N+1.
# TTL is byte 22. Fields that legitimately vary per packet: IP id (18-19),
# IP cksum (24-25), ICMP cksum (36-37), ICMP id/seq (38-41), payload timestamp.
S=$(dirname "$0")
timeout 90 "$S/p5.sh" 'ip route replace 10.102.1.1/32 via 10.101.101.34 dev swp7 2>/dev/null
   rm -f /tmp/a4.pcap; (tcpdump -i swp6 -s0 -w /tmp/a4.pcap host 10.102.1.1 >/dev/null 2>&1 &)
   sleep 2; ping -c 2 -s 56 -I 10.101.101.33 10.102.1.1 >/dev/null 2>&1
   sleep 2; killall tcpdump 2>/dev/null; sleep 1
   tcpdump -r /tmp/a4.pcap -nn -xx -c 1 2>/dev/null | sed -n "2,20p" | sed "s/^[^:]*:[[:space:]]*//" | tr -d " \n"
   echo' 2>/dev/null | tr -d ' \r'
