#!/bin/bash
# transit-probe-tos.sh [tos] -- transit probe with a NON-ZERO IP TOS byte.
# Slice 13's stock command sits on the TOS byte, whose value is 0 in ordinary
# traffic -- so any op that writes 0 there is invisible. Setting TOS makes it visible.
S=$(dirname "$0"); TOS=${1:-16}
timeout 90 "$S/p5.sh" "ip route replace 10.102.1.1/32 via 10.101.101.34 dev swp7 2>/dev/null
   rm -f /tmp/t.pcap; (tcpdump -i swp6 -s0 -w /tmp/t.pcap host 10.102.1.1 >/dev/null 2>&1 &)
   sleep 2
   python3 -c \"
import socket
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
s.setsockopt(socket.IPPROTO_IP,socket.IP_TOS,$TOS)
s.bind(('10.101.101.33',0))
for i in range(3): s.sendto(b'A'*56,('10.102.1.1',9))
\" 2>/dev/null
   sleep 2; killall tcpdump 2>/dev/null; sleep 1
   tcpdump -r /tmp/t.pcap -nn -xx -c 1 2>/dev/null | sed -n '2,20p' | sed 's/^[^:]*:[[:space:]]*//' | tr -d ' \n'
   echo" 2>/dev/null | tr -d ' \r'
