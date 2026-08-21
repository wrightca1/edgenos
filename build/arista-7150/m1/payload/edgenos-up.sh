#!/bin/sh
# edgenos-up.sh - bring the 7150 up as a router: dataplane + port netdev +
# OSPF control plane + hardware FIB sync. Run once after a cold boot.
#
# ⚠ RUN ONCE PER COLD BOOT. Restarting portd without a chip reset wedges the DMA
# rings and RX silently goes to 0 -- and because `ip link del et1` removes the
# netdev but does NOT kill the old portd, a second run leaves TWO instances
# fighting over the same rings. That was observed live: two pids, et1 rx=0 for
# 96s, ping 100% loss, and it looks exactly like a dataplane defect.
#
# So this script now refuses to run twice rather than silently producing a
# broken box. Set FORCE=1 to override (it will still be wedged).
set -u

if pidof fm6000_portd >/dev/null 2>&1 && [ "${FORCE:-0}" != "1" ]; then
	echo "[up] portd is ALREADY RUNNING (pid $(pidof fm6000_portd))." >&2
	echo "[up] Starting a second one wedges the DMA rings and RX goes to 0." >&2
	echo "[up] Reboot for a clean dataplane, or FORCE=1 to override." >&2
	exit 1
fi
export LD_LIBRARY_PATH=/usr/lib
B=http://<mgmt-net-host>:8001
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

say "3. port netdevs (PORTD_TXFCS=1: the ASIC expects an FCS placeholder on inject)"
# portd drives every port from ONE process -- they share the punt DMA ring, so a
# second instance is not an option.
#
# PORTS is the table: <ifname> <glort> <cidr>. Both front ports are brought up by
# default. Et1 and Et2 land on two DIFFERENT /29s of the same AS5610 peer, which
# is what makes a frame in one and out the other a genuine TRANSIT through this
# switch -- the prerequisite checklist items A4 and B1 are blocked on. Et3 is not
# here because its lane has never linked under EdgeNOS; add it as
# "et3 03ed 10.99.99.1/24" once it does.
#
# ⚠ Et2 does NOT come up every boot -- measured, not suspected. Treat a missing
# et2 carrier as normal rather than as a fault, and never conclude anything from
# a single boot. See docs/PORT3-BRINGUP.md.
MAC="${EDGENOS_MAC:-44:4c:a8:31:5d:ab}"     # the chip's router MAC; every routed
                                            # port carries it, a second one would
                                            # not be matched for routing
PORTS="${EDGENOS_PORTS:-et1 03ef 10.101.101.26/29
et2 03ee 10.101.101.34/29}"

echo "$PORTS" | while read -r i g c; do [ -n "${i:-}" ] && ip link del "$i" 2>/dev/null; done
for_spec=$(echo "$PORTS" | while read -r i g c; do [ -n "${i:-}" ] && printf '%s:%s:%s ' "$i" "$g" "$MAC"; done)
say "   portd: $for_spec-t 0"
PORTD_TXFCS=1 setsid /usr/bin/fm6000_portd $for_spec -t 0 \
    >/tmp/portd.log 2>&1 </dev/null &
sleep 3

echo "$PORTS" | while read -r i g c; do
    [ -n "${i:-}" ] || continue
    if [ ! -e "/sys/class/net/$i" ]; then
        echo "[up]    ⛔ $i: no netdev -- portd did not create it" | tee -a $LOG
        continue
    fi
    ip link set "$i" down 2>/dev/null
    ip link set "$i" address "$MAC"
    ip link set "$i" mtu 1600            # peer runs 1600 + MTU-mismatch detection
    ip link set "$i" up
    ip addr add "$c" dev "$i" 2>/dev/null
    echo "[up]    $i: $(ip -4 addr show "$i" | grep -o 'inet [0-9./]*') mtu=$(cat /sys/class/net/$i/mtu) carrier=$(cat /sys/class/net/$i/carrier 2>/dev/null)" | tee -a $LOG
done

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
