# IPv6

OSPFv3 on the fabric links, IPv6 in the hardware FIB, and a v6 loopback.
`loopback6` in `datapath.conf`, `ospf6d=yes` in the FRR daemons file.

OSPFv3 forms adjacencies over **link-local** addresses, so this comes up on
interfaces carrying only a v4 global address — no IPv6 addressing plan is
needed to get routing working. Worth knowing before anyone designs one.

## ⚠ IPv6 addressed to the switch itself needs chip entries

Before the v6 FIB sync existed, IPv6 unicast destined to this switch **never
reached the CPU**. Measured from a directly-attached neighbour:

```
ping  <our v4 interface>   3/3      ping6 <our link-local>   0/3  TIMEOUT
ping  <our v4 loopback>    2/2      ping6 <our v6 loopback>  0/2  TIMEOUT
```

IPv6 *multicast* arrived normally, because the L2 table carries explicit
entries for the OSPF groups pointing at the CPU. There was no equivalent for
unicast to our own addresses: the chip held **no IPv6 entries at all**, so
nothing told it those addresses were local and should be terminated.

The visible symptom was an OSPFv3 adjacency stuck in **ExStart** while the far
end retransmitted database descriptions we never saw.

## Three things that each break it silently

**Link-locals must be punted too.** OSPFv3 peers over `fe80::`, so handling
only global addresses leaves the control plane exactly as broken.

**An IPv6 default route carries no `RTA_DST`.** `::/0` is expressed by its
*absence*, where v4 sends `0.0.0.0`. Requiring `RTA_DST` silently drops the
default — which is most of what OSPFv3 provides.

**The netlink startup dump must ask for `AF_UNSPEC`.** Asking only for
`AF_INET` makes every existing IPv6 address invisible until it *changes*, and
for a loopback or a link-local that never happens.

## ⚠ A second fault can hide behind the first

With unicast delivered, one adjacency still would not leave ExStart. It needed
`ipv6 ospf6 mtu-ignore` — a genuine MTU mismatch with the far end.

That setting had been tried earlier and written off as "didn't help". It had
never actually been tested: the database description packets it governs were
not reaching the CPU at the time, so the experiment could not have succeeded
whatever the setting. **A negative result from a test that could not have
worked is not a negative result.** That error cost a long detour and ended in
blaming a far end which was provably doing the right thing.

## Implementation

v6 lives in tables parallel to the v4 ones in `sdkpoc.c` rather than folded
into them. The chip APIs differ only by a flag and a field, but every v4 helper
takes a `uint32_t` by value; widening them would touch a path carrying
production traffic, and parallel tables cost a few hundred bytes and cannot
regress it.

Covered: local-address punt, neighbour to egress object, and route programming
with the same retry-on-next-hop-resolution the v4 side uses.

## Not done

IPv6 **transit** forwarding is not proven. Local termination and route
programming are verified; pushing traffic *through* the box over IPv6 and
watching the chip counters — the way the v4 path was proven with a counted
packet run — has not been done.
