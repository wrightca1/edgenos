#!/bin/busybox sh
# TWO ROUTED PORTS, and the forwarding proof that needed them.
#
#   et1 = logical port 1, VLAN 1006, 10.101.101.42/29  -> AS5610 swp8 (.41)
#   et2 = logical port 2, VLAN 1007, 10.101.101.57/29  -> Nexus      (.58)
#
# With one port the chip could never be shown to forward: the only path was back
# out the ingress interface, which Trident2+ drops by design and which is not
# disableable on this silicon (BCM_E_UNAVAIL). With two ports that ambiguity is
# gone. The measurement is:
#
#   AS5610 pings the Nexus THROUGH us. If the chip forwards, the ping succeeds
#   and our CPU packet counter stays flat. If the CPU is forwarding, the counter
#   climbs by two per ping. If nothing is programmed, the ping just fails.
#   Three outcomes, all distinguishable.
set -u
mkdir -p /mnt/flash /tmp /var/run/frr /etc/frr
mount -t vfat /dev/mmcblk0p1 /mnt/flash 2>/dev/null

# NEVER let /bin/full reset the ASIC under a live bridge.
killall sdkpoc-shell zebra ospfd ospf6d 2>/dev/null
/bin/busybox sleep 3

wget -q -O /tmp/sdkpoc-shell http://10.22.1.5:8123/sdkpoc-shell || exit 3
chmod +x /tmp/sdkpoc-shell
md5sum /tmp/sdkpoc-shell

# ospf6d is NOT in the initrd -- it was trimmed to zebra+ospfd for size. Its
# shared-library closure is identical to ospfd's (libc, libfrr, libjson-c), so
# it can simply be dropped into the running rootfs. This does not survive a
# reboot; rolling it into initrd-ospf is a repack.
wget -q -O /usr/lib/frr/ospf6d http://10.22.1.5:8123/ospf6d && chmod +x /usr/lib/frr/ospf6d
wget -q -O /etc/frr/ospfd.conf  http://10.22.1.5:8123/ospfd.conf
wget -q -O /etc/frr/ospf6d.conf http://10.22.1.5:8123/ospf6d.conf

/bin/scdreset wd 8000 | tail -1
/bin/full > /tmp/full.out 2>&1; tail -1 /tmp/full.out

# /bin/full resets the management NIC, which comes back with a bogus MAC and no
# route -- the box then looks dead while sitting at a prompt.
ifconfig eth0 down 2>/dev/null
ifconfig eth0 hw ether 44:4c:a8:eb:93:f6 2>/dev/null
ifconfig eth0 10.1.1.241 netmask 255.255.255.0 up 2>/dev/null
route add -net 10.22.1.0 netmask 255.255.255.0 gw 10.1.1.1 2>/dev/null
route add default gw 10.1.1.1 2>/dev/null

DMA_BASE=0xd0000000 SDKPOC_CONFIG=/config.bcm SDKPOC_BCM=1 SDKPOC_CL72=1 \
SDKPOC_TAP=et1,et2 SDKPOC_TAP_PORT=1,2 SDKPOC_TAP_VLAN=1006,1007 \
SDKPOC_TAP_L3=1 \
    setsid /tmp/sdkpoc-shell > /mnt/flash/2port.log 2>&1 &
i=0
while [ $i -lt 40 ]; do
    grep -q "port(s), rx active" /mnt/flash/2port.log 2>/dev/null && break
    /bin/busybox sleep 5; i=$((i+1))
done
echo "=== bring-up ==="
grep -E "^tap:|^l3:|^fp:" /mnt/flash/2port.log 2>/dev/null

echo "=== addressing ==="
ifconfig et1 10.101.101.42 netmask 255.255.255.248 mtu 1600 up
ifconfig et2 10.101.101.57 netmask 255.255.255.248 mtu 1600 up
ip -6 addr add 2001:470:882d:1040::1/64 dev et1 2>&1
ip -6 addr add 2001:470:882d:1056::1/64 dev et2 2>&1
ip -br addr show et1 2>/dev/null || ifconfig et1 | head -3
ip -br addr show et2 2>/dev/null || ifconfig et2 | head -3

# The chip can only forward to a neighbour it has a host entry for, and that
# entry is built from the kernel's ARP/NDP cache -- so the cache has to be
# populated first. Pinging both peers is what does it.
echo "=== resolve both peers (populates ARP/NDP -> chip host entries) ==="
ping -c 3 -W 2 10.101.101.41 2>&1 | tail -2
ping -c 3 -W 2 10.101.101.58 2>&1 | tail -2
ping6 -c 3 -W 2 2001:470:882d:1056::2 2>&1 | tail -2 || \
    ping -6 -c 3 -W 2 2001:470:882d:1056::2 2>&1 | tail -2
arp -n 2>/dev/null | head -6

echo "=== start FRR (OSPF on et1, as before) ==="
rm -f /var/run/frr/*.pid /var/run/frr/*.vty /var/run/frr/zserv.api 2>/dev/null
chown -R 101:102 /var/run/frr /var/tmp 2>/dev/null
chmod 0777 /var/run/frr /var/tmp 2>/dev/null
/usr/lib/frr/zebra -d -u root -g root -f /etc/frr/zebra.conf \
    -i /var/run/frr/zebra.pid -z /var/run/frr/zserv.api
/bin/busybox sleep 3
/usr/lib/frr/ospfd -d -u root -g root -f /etc/frr/ospfd.conf \
    -i /var/run/frr/ospfd.pid -z /var/run/frr/zserv.api
/bin/busybox sleep 2
/usr/lib/frr/ospf6d -d -u root -g root -f /etc/frr/ospf6d.conf \
    -i /var/run/frr/ospf6d.pid -z /var/run/frr/zserv.api

n=0
while [ $n -lt 24 ]; do
    /bin/busybox sleep 5; n=$((n+1))
    r=$(grep -c et1 /proc/net/route 2>/dev/null)
    [ "$r" -gt 5 ] && { echo "  OSPF routes present after $((n*5))s"; break; }
done

/bin/busybox sleep 20
echo "=== chip state ==="
grep -E "^l3:|^fp:" /mnt/flash/2port.log | tail -14
echo "=== CPU counter baseline for the forwarding measurement ==="
grep "^tap: rx" /mnt/flash/2port.log | tail -2
sync
echo TWOPORT_READY
