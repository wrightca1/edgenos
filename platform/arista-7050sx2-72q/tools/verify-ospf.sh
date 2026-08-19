#!/usr/bin/env bash
# Show that OSPF over the hardware port is working. Run from any host that can
# reach the lab (it does NOT need the 7050's management interface, which often
# dies after a cold run -- see docs/OSPF-ADJACENCY-20260818.md section 7).
#
#   tools/verify-ospf.sh              far-end checks only (fastest, independent)
#   tools/verify-ospf.sh --console    also drive the 7050 over its serial console
set -u
FAR=${FAR:-10.1.1.238}          # AS5610, the OSPF peer
VTY=${VTY:-2604}                # quagga/FRR ospfd vty port
PW=${PW:-zebra}
US=${US:-10.101.101.42}         # our router-id / interface address
HERE=$(cd "$(dirname "$0")" && pwd)

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

say "1. OSPF neighbour table on the peer  (expect $US in state Full)"
python3 "$HERE/frr-vty.py" "$FAR" "$VTY" "$PW" "terminal length 0" \
        "show ip ospf neighbor" 2>/dev/null | tr -d '\r' |
    grep -E "Neighbor ID|$US|Full" || echo "  (no neighbour output)"

say "2. Our router in the peer's link-state database  (proves DB sync, not just hellos)"
python3 "$HERE/frr-vty.py" "$FAR" "$VTY" "$PW" "terminal length 0" \
        "show ip ospf database router $US" 2>/dev/null | tr -d '\r' |
    grep -E "Link State ID|Advertising Router|Number of Links|Link connected" ||
    echo "  (not in LSDB)"

say "3. Frames arriving on the peer's port from us"
A=$(sshpass -p as5610 ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=no \
      root@"$FAR" 'cat /sys/class/net/swp8/statistics/rx_packets' 2>/dev/null)
sleep 5
B=$(sshpass -p as5610 ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=no \
      root@"$FAR" 'cat /sys/class/net/swp8/statistics/rx_packets' 2>/dev/null)
echo "  swp8 rx_packets: $A -> $B  (delta $((B-A)) over 5 s; OSPF hellos are every 10 s)"

say "4. Routes the 7050 LEARNED over OSPF and installed in its kernel FIB"
# The 7050's mgmt is often unreachable from other subnets after a cold run, but
# the AS5610 is on its subnet -- hop through it.
sshpass -p as5610 ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=no root@"$FAR" \
  'python3 - <<PYEOF
import socket,time
s=socket.create_connection(("10.1.1.241",23),timeout=8); s.settimeout(2)
b=b""
def d(t=2.0):
    global b; e=time.time()+t
    try:
        while time.time()<e:
            x=s.recv(4096)
            if not x: break
            b+=x
    except Exception: pass
d(2); s.sendall(b"echo ROUTES=$(route -n | grep -c et1)\n"); d(4)
print(b.decode("utf-8","replace"))
PYEOF' 2>/dev/null | grep -E "^ROUTES=|ROUTES=[0-9]" | tail -1 ||
    echo "  (could not reach the 7050)"

[ "${1:-}" = "--console" ] || exit 0

# The 7050 itself, over the lab serial console. ttyUSB1 IS this box -- ttyUSB2 is
# the 7150 and docs/FINDINGS.md is wrong about that.
say "5. On the 7050: ping the peer ACROSS THE HARDWARE PORT"
ssh lab-console "sudo timeout 60 python3 ~/con.py /dev/ttyUSB1 \
    'ping -c 3 -W 2 10.101.101.41 2>&1 | tail -3' -b 9600 -t 25" 2>/dev/null |
    tr -d '\r' | tail -4

say "6. On the 7050: the TAP bridge and FRR"
ssh lab-console "sudo timeout 60 python3 ~/con.py /dev/ttyUSB1 \
    'ifconfig et1 | head -2; ps | grep -c ospfd' -b 9600 -t 20" 2>/dev/null |
    tr -d '\r' | tail -5
