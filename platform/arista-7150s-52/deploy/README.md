# 7150 control plane (zebra + ospfd)

Quagga 1.2.4, same component the AS5610 uses (`core/control-plane/`). Build it for
x86_64 with `CFLAGS=-fcommon` — modern GCC defaults to `-fno-common` and Quagga's
`lib/prefix.h` has a tentative `__packed` definition that then multiply-defines.
`--disable-nhrpd` avoids the libcares dependency.

## Status (2026-08-06)

Both daemons run on the switch, `et1` is a real Linux interface (`fm6000_portd`),
and **OSPF hellos are exchanged on the wire in both directions** — confirmed by
tcpdump at the AS5610:

    44:4c:a8:31:5d:ab > 01:00:5e:00:00:05  10.101.101.26 > 224.0.0.5: OSPFv2 Hello
    80:a2:35:81:ca:b4 > 01:00:5e:00:00:05  10.101.101.25 > 224.0.0.5: OSPFv2 Hello

**The adjacency does not form yet — but NOT for the reason first assumed.**

### The FFU trap is NOT needed — the punt already works

`fm6000_rxdump` shows the peer's OSPF hellos already arriving at the CPU, correctly
framed, on the cold-booted chip:

```
01 00 5e 00 00 05 | 80 a2 35 81 ca b4 | 07 01 03 ef 00 01 ff ff | 08 00 45 c0 ... 59
DMAC (AllSPFRouters) SMAC (peer)         F64 tag                   IPv4, proto 0x59 = OSPF
```

So the ASIC already traps 224.0.0.5 to the CPU. The `FP 224/8 trap` that the 5610
needs is either already present in the replayed configuration or unnecessary on
this part. **No FFU rule was written, and none appears to be required.**

### What is actually still broken

Both directions are proven independently:
 - our hellos reach the peer (tcpdump at the AS5610)
 - the peer's hellos reach our CPU (`fm6000_rxdump`)
 - unicast works end to end (peer's ping answered by the kernel stack, 6/6)

but ospfd does not pair them into an adjacency. Suspects, in order:
 1. `portd`'s tag-splice for these frames — the scan matches the ethertype at
    offset +20 and rewrites the header; worth dumping exactly what lands on the
    TAP versus what the peer sent.
 2. Multicast delivery to ospfd's socket. `et1` reports `multicast=0` even while
    frames flow (may just be a TAP driver stat, not proof).
 3. Stale daemon instances: repeated bring-ups leave unreapable `[zebra]`/`[ospfd]`
    entries. They look like zombies, but always confirm only ONE live pair.

Next concrete step: sniff the TAP side directly (add a debug dump to `portd`, or
build a static tcpdump for the image) and compare byte-for-byte against the frame
the peer transmitted.

## Running it

    mkdir -p /dev/net && mknod /dev/net/tun c 10 200   # not in the initramfs
    insmod /lib/modules/6.12.0/kernel/drivers/net/tun.ko
    fm6000_portd et1 03ef x 0 &
    ip link set et1 address 44:4c:a8:31:5d:ab; ip link set et1 up
    ip addr add 10.101.101.26/29 dev et1
    LD_LIBRARY_PATH=/usr/lib zebra -d -f /etc/quagga/zebra.conf
    LD_LIBRARY_PATH=/usr/lib ospfd -d -f /etc/quagga/ospfd.conf

Needs `libzebra.so.1`, `libospf.so.0`, `libm.so.6` in `/usr/lib`.
**Never `wget -O` over a library in `/lib64` on the running initramfs** — that is
how SSH got broken during bring-up.
