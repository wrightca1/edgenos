#!/bin/sh
# edgenos-up.sh - bring the 7150 up as a router: dataplane + port netdev +
# OSPF control plane + hardware FIB sync. Run once after a cold boot.
#
# NOTE: restarting portd repeatedly without a chip reset can wedge the DMA rings
# (RX silently goes to 0). If RX stops, reboot rather than restarting portd.
set -u
export LD_LIBRARY_PATH=/usr/lib
B=http://10.1.1.30:8001
LOG=/tmp/edgenos-up.log; : > $LOG
say(){ echo "[up] $*" | tee -a $LOG; }

say "1. modules + device nodes"
insmod /lib/modules/6.12.0/extra/fm6000dma.ko 2>/dev/null
insmod /lib/modules/6.12.0/kernel/drivers/net/tun.ko 2>/dev/null
mkdir -p /dev/net /usr/lib /etc/quagga /var/run/quagga
[ -c /dev/net/tun ] || mknod /dev/net/tun c 10 200

say "2. loopback (initramfs leaves it DOWN -- Quagga's VTY needs it)"
ip link set lo up
ip addr add 127.0.0.1/8 dev lo 2>/dev/null
ip addr add 10.101.255.26/32 dev lo 2>/dev/null

say "3. port netdev et1 (PORTD_TXFCS=1: the ASIC expects an FCS placeholder on inject)"
ip link del et1 2>/dev/null
PORTD_TXFCS=1 setsid /usr/bin/fm6000_portd et1 03ef x 0 >/tmp/portd.log 2>&1 </dev/null &
sleep 3
ip link set et1 down 2>/dev/null
ip link set et1 address 44:4c:a8:31:5d:ab
ip link set et1 mtu 1600                 # peer runs 1600 + MTU-mismatch detection
ip link set et1 up
ip addr add 10.101.101.26/29 dev et1 2>/dev/null
say "   et1: $(ip -4 addr show et1 | grep -o 'inet [0-9./]*') mtu=$(cat /sys/class/net/et1/mtu)"

say "4. control plane (zebra + ospfd)"
setsid /usr/bin/zebra -d -f /etc/quagga/zebra.conf </dev/null >/tmp/zebra.log 2>&1; sleep 3
setsid /usr/bin/ospfd -d -f /etc/quagga/ospfd.conf </dev/null >/tmp/ospfd.log 2>&1; sleep 3
say "   daemons: $(ps | grep -cE '[z]ebra|[o]spfd')"

say "5. waiting for the OSPF adjacency"
i=1; while [ $i -le 12 ]; do sleep 8
  N=$(ip route show | wc -l)
  say "   t=$((i*8))s  kernel routes=$N  et1 rx=$(cat /sys/class/net/et1/statistics/rx_packets)"
  [ "$N" -gt 3 ] && break
  i=$((i+1)); done

say "6. hardware FIB sync"
setsid /usr/bin/fm6000_fibd -i 5 -v </dev/null >/tmp/fibd.log 2>&1 &
sleep 8
tail -4 /tmp/fibd.log | sed 's/^/   /'
say "=== UP ==="
