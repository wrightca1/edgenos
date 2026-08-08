# The 1-in-6 alpha8 boot failure — what is known

## Signature

```
routes = 35        OSPF reaches Full and stays there
rx     = 82        vs 125-135 on a good boot -- and 82 BOTH times it was caught
ping   = 100% loss on every round
```

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

## Next step

Confirm by reading the MA/L2 MAC table (`L2L_MAC`, 0x280000-0x300000) on a failing boot and
looking for `80:a2:35:81:ca:b4`. That range is not in the known-safe read set, so check it against
`fm6000_register_dictionary` first -- several ranges off-bus the chip when read.

Prime suspects if it is ageing: `fm6000_l2linit` and `fm6000_sweeperinit` (L2L_SWEEPER is the L2
ageing sweeper, and only 57 of its 177 registers are generated -- the other 120 are multi-write
control left in the replay). Test by disabling those two generators and re-soaking; the failure
rate should change if either is responsible.
