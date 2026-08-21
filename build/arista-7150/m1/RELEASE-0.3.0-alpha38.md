# 0.3.0-alpha38 — L3AR slice 0's action banks + the shared profile tables

**Provenance crosses 90%.** This closes L3AR: every slice, every action bank, every profile table.

## What changed

`asic/fm6000/fm6000_l3artables.c` (from `asic/fm6000/tools/gen_l3ar_tables.py`) fills the two
gaps the per-slice generators leave — **450 writes**.

### ★ Slice 0's RAM3, RAM4 and RAM5 had never been programmed by us

`fm6000_l3arinit` emits slice 0's CAM plus RAM1/RAM2 only. It was written under the premise
`l3ar_decode.py` used to state — *"the action is a flag rewrite, and that is all it is"* — which
`docs/L3AR-STRUCTURE.md` retracted at the start of this session. There are five RAM banks, and
RAM3/4/5 hold the L2-lookup, ALU, policer, QoS, GLORT and action-data muxes.

Slice 0 is the **forwarding** stage and all 32 of its rules populate all three banks. So 160
addresses of live forwarding actions were coming from EOS's replay the entire time. The
retraction predicted exactly this; here is the evidence and the fix.

### The 19 profile tables are shared, so no slice owned them

Indexed by the 5-bit profile numbers the slices select, and entries are referenced from more
than one slice — slice 2 selects csGlort profile 5, which slice 1 also uses. Each slice
generator emitted only what its own rules referenced, and the union still left gaps.

⚠ **Single ownership.** `gen_l3ar_slice1` already emits SGLORT 0-1 and csGlort 0/5/10; those 10
addresses are excluded here so exactly one generator owns each. Checked: the two agree on all 10.

⚠ **A fabricated-write bug, caught by the verify.** The first version emitted zeros for
`W8ABCD` words EOS never writes (`0x11c83`, `0x11c87`, `0x11c88`) because the extractor filled
missing words with 0. Writing where EOS writes nothing is not authoring, it is invention. Now
per-word: absent words stay absent.

`--verify`: **450 of 450 identical, 0 differing, 0 fabricated.**

## Measured

| | alpha37 | alpha38 |
|---|---|---|
| executed writes | 130,672 | **130,610** |
| ours | 117,375 (89.8%) | **117,825 (90.2%)** |
| et1 / et2 | `0940` / `0940` | `0940` / `0940` |

Transit passes: frames in on et2, out on et1, SMAC rewritten to the router MAC, DMAC to the
next-hop, **TTL `0x3f` = 63**.

## ⚠ A false regression, and the harness fix it produced

alpha38 was first judged to **break forwarding**: the transit capture came back empty. It was
reverted to alpha37 — and **the same test failed on alpha37 too**, the image that had passed it
an hour earlier with no code change.

The cause was neither image. The peer's neighbour entry for `10.101.101.34` (the switch's et2)
had gone **FAILED**, and once in that state Linux backs off rather than re-ARPing, so the peer
transmitted 3 of 20 packets. Nothing was captured because nothing was sent. Switch-initiated
pings in both directions refreshed the entry and both images then passed.

`tools/transit-test.sh` now primes ARP in both directions and prints the neighbour state before
testing. Without that, the harness reports "no forwarding" for a condition that has nothing to
do with the switch — the single most likely way for it to lie.

md5 `dfef159cc3735b9aedafa25daaa81899`, verified on the switch.
