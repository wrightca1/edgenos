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

### Fixed since: FCS + MTU

**1. The ASIC appends the 4-byte Ethernet FCS to punted frames.** portd was passing
it through, making every frame 4 bytes too long. Verified by hexdumping both sides:

```
ASIC raw len=90:  01 00 5e 00 00 05 | 80 a2 35 81 ca b4 | 07 01 03 ef 00 01 ff ff | 08 00 45 c0 00 40 ...
TAP  spliced len=78: 01 00 5e 00 00 05 | 80 a2 35 81 ca b4 | 08 00 | 45 c0 00 40 ...
```

78 bytes with IP total length 0x0040 = 64 -> a clean 44-byte OSPF Hello. The RX
path is now byte-correct; portd strips both the F64 tag and the FCS.

**2. MTU.** The peer's swp6 is **1600** with `MTU mismatch detection: enabled`;
`et1` defaulted to 1500. That alone would stall the adjacency in ExStart even
once hellos are clean. `et1` is now set to 1600.

### What is actually still broken

Both directions are proven independently:
 - our hellos reach the peer (tcpdump at the AS5610)
 - the peer's hellos reach our CPU (`fm6000_rxdump`)
 - unicast works end to end (peer's ping answered by the kernel stack, 6/6)

but ospfd does not pair them into an adjacency. Suspects, in order:
**Our ospfd emits a malformed Hello.** The peer sees length **48** where a
no-neighbour Hello is 44, and the extra 4 bytes decode as a neighbour entry whose
router-id is **different in every packet**:

```
Hello Timer 10s, Dead Timer 40s, Mask 255.255.255.248, Priority 1
Neighbor List:
  17.186.86.252      <- random, changes per Hello
```

The peer therefore never registers us (`Neighbor Count is 0`). Since the RX path
is now verified byte-correct, this is ospfd's own state, not frame corruption on
the way in. Ruled out so far:

 - **FFU/multicast trap** — not needed, hellos already reach the CPU.
 - **Config mismatch** — area 0, /29 mask, hello 10 / dead 40 all match the peer.
 - **`-fcommon` packing** — only `struct ethaddr` uses `__packed` (6 chars, no
   padding), so the no-op is harmless. `__packed` is undefined on glibc and
   silently declares a variable, which is why the build needed `-fcommon`.

Remaining suspects, in order:
 1. Something in the Quagga 1.2.4 + modern-glibc build. Worth trying a distro
    FRR/Quagga binary, or building with `-D__packed='__attribute__((packed))'`
    and without `-fcommon`, to see whether the malformed Hello persists.
 2. Our own multicast being looped back to the CPU and parsed as a peer.
 3. Stale daemon instances: repeated bring-ups leave unreapable `[zebra]`/`[ospfd]`
    entries that `killall -9` will not clear. Always confirm exactly ONE live pair
    before trusting a negative result.

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
