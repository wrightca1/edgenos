# The 1-in-6 alpha8 boot failure — what is known

## Signature

```
routes = 35        OSPF reaches Full and stays there
ping   = 100% loss on every round
```

⚠ An earlier version of this file called `rx=82` a signature, on the strength of it appearing in
both captured failures. **It is not.** A later GOOD boot measured `rx=83` with 0% loss. The RX
counter simply reflects how far OSPF has got when the sample is taken; it says nothing about the
fault. Two matching numbers out of two samples was coincidence and should not have been written up
as a signature.

Reproduced deliberately: `hunt.sh` booted alpha8 repeatedly and caught it on boot 3 of 3
(`hunt.log`). The rx=82 figure matched the earlier soak failure exactly, so this is one
repeatable fault, not general flakiness.

## What it is NOT

**Chip configuration is byte-identical between good and bad boots.** `boot1.pre` vs `boot3.pre`
and `boot1.post` vs `boot3.post` diff to nothing across 40 registers covering PIN, scheduler,
EPL status, GLORT, L2F, LBS, SAF, CM, FFU, NEXTHOP and the DMA block. So it is not a generator
writing the wrong value, and not a missed write.

It is also **not the RX ring death** fixed in e124f98: RX keeps moving on the failing boot
(99 -> 108 over 25s), and the OSPF adjacency holds.

## What it is

Measured on the live failing boot:

| traffic | direction | works? |
|---|---|---|
| OSPF hellos (multicast) | in | ✅ rx keeps climbing |
| ARP request (broadcast) | out | ✅ |
| ARP reply (unicast to us) | in | ✅ resolves to 80:a2:35:81:ca:b4 |
| OSPF DBD/LSU (unicast) | both | ✅ during adjacency formation |
| ICMP echo (unicast) | out | ❌ 100% loss |

**Unicast egress fails; broadcast egress and unicast ingress both work.** Flushing ARP and
re-resolving does not fix it, so it is not a stale ARP entry.

Unicast worked long enough for OSPF to reach Full and then stopped, which points at the L2
forwarding table: the neighbour's MAC entry going missing or ageing out. Broadcast still floods,
which is why everything except unicast keeps working.

## ⚠ The leading hypothesis was tested and REFUTED

The theory was that the peer's MAC entry was going missing from the L2 forwarding table, which
would explain unicast failing while broadcast floods.

`asic/fm6000/tools/fm6000_l2scan.c` dumps the table (`L2L_MAC`, 0x280000-0x300000). Reading it is
safe -- probed word-at-a-time with PIN checked after each, no off-bus.

**Failing boot and good boot hold the IDENTICAL 12 entries:**

```
2bd57c: ff315dab 03ef3333 30060300 00000001    <- our own MAC 44:4c:a8:31:5d:ab, GLORT 0x3ef
2bee8c: 00000006 03ef3333 30060300 00000001
... 10 more, same on both
```

The peer's MAC `80:a2:35:81:ca:b4` is **absent in both cases** -- which makes sense in hindsight:
this is a routed point-to-point /29, so forwarding does not depend on learning the peer. The
hypothesis was wrong and the L2 table is not involved.

## Where that leaves it

Confirmed by measurement, not inference:

- chip configuration identical across 40 registers, good vs bad
- L2 MAC table identical
- RX ring alive on the failing boot; OSPF holds at 35 routes
- unicast egress fails; broadcast egress and unicast ingress work
- the RX counter is not diagnostic

**No current hypothesis.** The remaining places to look are the ones not yet observable from the
switch alone: whether our unicast frames actually reach the wire (a far-end capture would settle
it), and portd's per-frame F64/GLORT tagging on the TX path. Both need instrumentation that does
not exist yet.
