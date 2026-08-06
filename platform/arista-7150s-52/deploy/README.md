# 7150 control plane (zebra + ospfd)

Quagga 1.2.4, same component the AS5610 uses (`core/control-plane/`). Build it for
x86_64 with `CFLAGS=-fcommon` — modern GCC defaults to `-fno-common` and Quagga's
`lib/prefix.h` has a tentative `__packed` definition that then multiply-defines.
`--disable-nhrpd` avoids the libcares dependency.

## Status: OSPF IS UP (2026-08-06)

Full adjacency with the AS5610, confirmed from both sides:

```
ours:  10.101.101.241   Full/DR       et1:10.101.101.26
peer:  10.101.255.26    Full/Backup   swp6:10.101.101.25
```

and a complete OSPF routing table learned from the network, including a default:

```
default       via 10.101.101.25 dev et1  metric 20
10.3.1.0/24   via 10.101.101.25 dev et1  metric 20
10.22.1.0/24  via 10.101.101.25 dev et1  metric 20
10.101.1.0/29 ... 10.101.1.32/29 ...
```

The chain works end to end: **ASIC -> punt -> TAP -> kernel -> ospfd -> zebra ->
kernel FIB.** The remaining link is FIB sync (kernel -> ASIC); `fm6000_route`
already programs the hardware, it just is not driven automatically yet.

## The four things that had to be right

**1. The ASIC expects a 4-byte FCS placeholder on INJECT.** This was the last and
least obvious bug. The ASIC appends an FCS to frames it punts to us, and it
*symmetrically expects one on the way back*. Without it the DMA consumed the last
4 bytes of real payload — so every OSPF Hello lost its final field, which is the
neighbour entry. The symptom was maddening: everything before the last field was
pristine, and the corrupted value looked *random* rather than damaged, because it
was whatever followed in memory.

```
Neighbor List:                 Neighbor List:
  17.186.86.252     ->           10.101.101.241
  (random each Hello)            (the actual peer)
```

`fm6000_portd` now appends the placeholder (`PORTD_TXFCS=1`, on by default in the
service recipe below). **Anything else that injects frames needs the same.**

**2. The ASIC appends an FCS on PUNT** — strip it, or every frame reaches the
kernel 4 bytes too long. (`FCS_LEN` in portd.)

**3. `lo` is down in the initramfs.** Nothing brings loopback up, so `127.0.0.1`
is unreachable, Quagga's VTY cannot be reached at all, and daemon state is
invisible. Bring it up before starting zebra/ospfd — this is what unblocked
diagnosis and took us from "no visibility" to seeing `Init`.

**4. MTU.** The peer's swp6 is 1600 with `MTU mismatch detection: enabled`; `et1`
defaults to 1500, which stalls the adjacency in ExStart.

## Building Quagga for x86_64

Quagga 1.2.4, the same component the AS5610 uses. Apply `quagga-packed.patch`
first: `__packed` is a BSD-ism glibc does not define, so `} __packed;` silently
declares a *global variable* instead of packing the struct — which is why an
unpatched build fails with "multiple definition of `__packed`". Patching it is
correct; reaching for `-fcommon` merely hides it.

```
patch -p1 < quagga-packed.patch
./configure --prefix=/usr --sysconfdir=/etc/quagga --localstatedir=/var/run/quagga \
  --enable-user=root --enable-group=root --enable-vty-group=root \
  --disable-ripd --disable-ripngd --disable-bgpd --disable-isisd --disable-pimd \
  --disable-babeld --disable-nhrpd --disable-doc --disable-ospfclient \
  --disable-ospf6d --disable-watchquagga CFLAGS="-O2"
make -j8
```
Binaries land in `zebra/.libs/zebra` and `ospfd/.libs/ospfd` (the top-level ones
are libtool wrappers). Ship `libzebra.so.1`, `libospf.so.0` and `libm.so.6` in
`/usr/lib`. `--disable-nhrpd` avoids the libcares dependency.

## Running it

```sh
insmod /lib/modules/6.12.0/extra/fm6000dma.ko
insmod /lib/modules/6.12.0/kernel/drivers/net/tun.ko
mkdir -p /dev/net && mknod /dev/net/tun c 10 200      # not in the initramfs

ip link set lo up && ip addr add 127.0.0.1/8 dev lo   # REQUIRED (see #3)
ip addr add 10.101.255.26/32 dev lo                   # stable router-id

PORTD_TXFCS=1 fm6000_portd et1 03ef x 0 &
ip link set et1 address 44:4c:a8:31:5d:ab
ip link set et1 mtu 1600 && ip link set et1 up
ip addr add 10.101.101.26/29 dev et1

export LD_LIBRARY_PATH=/usr/lib
zebra -d -f /etc/quagga/zebra.conf
ospfd -d -f /etc/quagga/ospfd.conf
```

Check with `telnet 127.0.0.1 2604` (password `zebra`), `show ip ospf neighbor`.

**Never `wget -O` over a library in `/lib64` on the running initramfs** — that is
how SSH got broken during bring-up. Stage into `/usr/lib` instead.

Throughput caveat: portd's TX does a `TX_STOP -> fill -> TX_START` per frame
(~10 ms), capping near 100 pps. Fine for a control plane; it is not a data path.
