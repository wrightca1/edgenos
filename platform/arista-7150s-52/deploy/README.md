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

**The adjacency does not form yet.** The remaining gap is the multicast CPU punt:
the ASIC must trap 224.0.0.0/8 to the CPU so ospfd actually receives the peer's
hellos. EdgeNOS already documents this for the 5610 (`core/control-plane/build-quagga.sh`):

  > "edged mirrors the kernel FIB to the chip ... so ANY daemon that installs to
  >  the kernel gets HW-programmed for free — OSPF needs only this daemon + the
  >  control-traffic CPU punt (already in edged: FP 224/8 trap + CPU_CONTROL_1
  >  TTL1 traps + MC copy-replication regs)."

So the FM6000 needs the equivalent: an FFU rule matching 224.0.0.0/8 with a
CPU-GLORT action. Unicast punt already works (a ping from the peer is answered by
the kernel stack through `et1`).

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
