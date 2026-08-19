#!/bin/busybox sh
# SERVE=<url> overrides the build host these binaries are fetched from.
# It defaults to the address of the lab machine these measurements were taken on;
# set it to your own or the fetches will fail.
# Hardware L3, with the field-processor punt rules that were the missing piece.
#
# This is the same run as edgenos-ospf-test.sh with SDKPOC_TAP_L3=1, which is
# the configuration that previously stalled at ExStart. The difference is
# l3sync.c now installs IFP rules (IP protocol 89 -> CopyToCpu, and our own
# address -> CopyToCpu), reproducing how EOS actually terminates local traffic.
#
# THE TEST HAS TWO GATES, IN ORDER. The second is meaningless without the first:
#
#   gate 1  the adjacency reaches Full WITH L3 ON.
#           Previously it reached ExStart and stopped. If this still fails the
#           punt rules did not do their job and nothing below matters.
#   gate 2  used_route climbs off zero, i.e. routes are really in DEFIP.
#           This can only happen after gate 1, because zebra has no routes to
#           mirror until the adjacency is Full. used_route=0 was always a
#           CONSEQUENCE of gate 1 failing, never an independent bug.
#
# The field-processor counters are printed because "the rule installed" and
# "the rule is being hit" are different claims and only the second one is
# evidence.
#
# Watchdog armed for the run and disarmed at the end.
set -u
mkdir -p /mnt/flash /tmp /var/run/frr /etc/frr
mount -t vfat /dev/mmcblk0p1 /mnt/flash 2>/dev/null
wget -q -O /tmp/sdkpoc-shell ${SERVE:-http://10.22.1.5:8123}/sdkpoc-shell || exit 3
chmod +x /tmp/sdkpoc-shell
md5sum /tmp/sdkpoc-shell

/bin/scdreset wd 8000 | tail -1
/bin/full > /tmp/full.out 2>&1; tail -1 /tmp/full.out

# SDKPOC_TAP_L3=1     the thing under test
# SDKPOC_TAP_VLAN     EOS's structure: a dedicated VLAN per routed port. Needed
#                     (see HARDWARE-L3 sec.13) but not sufficient on its own.
DMA_BASE=0xd0000000 SDKPOC_CONFIG=/config.bcm SDKPOC_BCM=1 SDKPOC_CL72=1 \
SDKPOC_TAP=et1 SDKPOC_TAP_PORT=1 SDKPOC_TAP_L3=1 SDKPOC_TAP_VLAN=1006 \
    setsid /tmp/sdkpoc-shell > /mnt/flash/l3hw.log 2>&1 &
i=0
while [ $i -lt 40 ]; do
    grep -q "^tap: et1" /mnt/flash/l3hw.log 2>/dev/null && break
    /bin/busybox sleep 5; i=$((i+1))
done
echo "=== bring-up ==="
grep -E "^tap:|^l3:|^fp:" /mnt/flash/l3hw.log 2>/dev/null

vq() {
    port=$1; shift
    { printf '%s\n' "$@"; printf 'exit\n'; /bin/busybox sleep 1; } \
        | /bin/busybox nc 127.0.0.1 "$port" 2>/dev/null
}

{
echo "=== interface ==="
# busybox `ip link set ... up` returns success while doing nothing; ifconfig is
# the busybox-native path and is what actually brings the link up.
ifconfig et1 hw ether 00:11:22:33:44:55 2>&1
ifconfig et1 10.101.101.42 netmask 255.255.255.248 mtu 1600 up 2>&1
ifconfig et1 2>&1 | head -3

echo "=== start FRR ==="
rm -f /var/run/frr/*.pid /var/run/frr/*.vty /var/run/frr/zserv.api 2>/dev/null
chown -R 101:102 /var/run/frr /var/tmp 2>/dev/null
chmod 0777 /var/run/frr /var/tmp 2>/dev/null
/usr/lib/frr/zebra -d -u root -g root -f /etc/frr/zebra.conf \
    -i /var/run/frr/zebra.pid -z /var/run/frr/zserv.api 2>&1
/bin/busybox sleep 3
/usr/lib/frr/ospfd -d -u root -g root -f /etc/frr/ospfd.conf \
    -i /var/run/frr/ospfd.pid -z /var/run/frr/zserv.api 2>&1
/bin/busybox sleep 2
ps 2>/dev/null | grep -E "zebra|ospfd" | grep -v grep

echo "=== GATE 1: adjacency must reach Full (was ExStart) ==="
n=0
while [ $n -lt 24 ]; do
    /bin/busybox sleep 5; n=$((n+1))
    st=$(vq 2604 "show ip ospf neighbor" 2>/dev/null | grep 10.101.101.41)
    echo "  t=$((n*5))s $st"
    echo "$st" | grep -q "Full" && { echo "  GATE1 PASS after $((n*5))s"; break; }
done
vq 2604 "show ip ospf neighbor" 2>&1

echo "=== routes learned (zebra) ==="
vq 2601 "show ip route ospf" 2>&1 | head -20
echo "  kernel FIB route count: $(grep -c . /proc/net/route)"
} > /mnt/flash/l3hw.out 2>&1

# The FIB mirror polls from the bridge idle tick, so give it time to see the
# routes zebra just installed.
/bin/busybox sleep 20

{
echo "=== GATE 2: routes in DEFIP (used_route must be > 0) ==="
grep -E "^l3:|^fp:" /mnt/flash/l3hw.log | tail -30
} >> /mnt/flash/l3hw.out 2>&1

/bin/scdreset wd 0 | tail -1
sync
cat /mnt/flash/l3hw.out
echo L3HW_FINISHED
