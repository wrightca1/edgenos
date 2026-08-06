# FM6000 routing — how EOS programs a route into hardware

**2026-08-06.** Captured live: `fmPlatformTraceRegOps` armed on a running EOS, then a single static
route added. **1,159 writes total** — small enough to read end to end.

```
ip route 10.77.77.0/24 10.101.101.33      ->  S 10.77.77.0/24 via 10.101.101.33, Ethernet2
```

Raw trace: `notes/reference/scd-dumps/fm6000-static-route-LIVE-trace.txt`.

---

## Composition of one route

| writes | block | role |
|---:|---|---|
| 935 | **FFU** (`0x300000+`) | the route table itself |
| 216 | JSS/SBus (`0x00f00x`) | SerDes housekeeping, unrelated |
| 4 | **NEXTHOP** (`0x160000`) | the adjacency (rewrite MAC + egress GLORT) |
| 4 | STATS (`0x01a000`) | counters |

There is **no dedicated LPM/TRIE block**. IPv4 forwarding on the FM6000 is implemented in the
**FFU** — the same multi-slice TCAM used for ACLs.

## 1. NEXTHOP — the adjacency table

Two entries were (re)written, one per egress port:

```
0x160014 <- 0x3581cab4     0x16001e <- 0x3581cab5
0x160015 <- 0x03ef80a2     0x16001f <- 0x03ee80a2
```

Decoded — the AS5610's MAC is `80:a2:35:81:ca:b4`:

| word | contents |
|---|---|
| `+0` | `MAC[5:2]` — the low four bytes of the next-hop MAC (`35 81 ca b4`) |
| `+1` | `GLORT << 16 \| MAC[1:0]` — egress GLORT in the top half, `80 a2` in the bottom |

So `0x03ef80a2` = GLORT `0x03ef` (**Et1**) + MAC prefix `80a2`, and `0x03ee80a2` = GLORT `0x03ee`
(**Et2**). Entries are **10 words apart** (`0x14` → `0x1e`).

That is a complete routing adjacency: *rewrite the destination MAC to this, and send it out this
GLORT.* It ties straight into the GLORT scheme in `GLORT-MAPPING.md`.

## 2. FFU — the route table is a sorted prefix array

The new prefix `10.77.77.0` = `0x0a4d4d00` appears at `0x33bfdc`, slotted into a **sorted array**:

```
0x33bfd9 = 0x0a140100    10.20.1.0
0x33bfda = 0x0a150100    10.21.1.0
0x33bfdb = 0x0a160100    10.22.1.0
0x33bfdc = 0x0a4d4d00    10.77.77.0   <-- inserted here, in sorted order
0x33bfdd = 0x0a636300    10.99.99.0
0x33bfde = 0x0a640100    10.100.1.0
0x33bfdf = 0x0a650100    10.101.1.0
```

**This is why one route costs 935 writes:** inserting into a sorted array rewrites the entire tail.
The same series also appears at `0x33bbd2…` — a second copy `0x400` words below, i.e. the table is
double-buffered (or held in two slices) and rewritten wholesale.

Per-slice TCAM entries are written in 4-word groups at stride `0x4000`:

```
0x3b0010..13   0x3b4010..13   0x3b8010..13   0x3bc010..13
```

with the `+1`/`+3` words constant (`0x0000001f`, `0x0000002f`) — key/mask control — and `+0`/`+2`
carrying key material.

## 3. What this gives us

Everything needed to implement hardware routing ourselves:

1. **Adjacency** — write `(MAC, GLORT)` pairs into `NEXTHOP` at `0x160000 + 10*index`. Small,
   well-understood, and it uses our own GLORT allocation.
2. **Prefix table** — maintain the sorted prefix array in the FFU and rewrite the affected tail.
   Straightforward to generate; the cost is writes, not complexity.
3. **Slice config** — the `0x3b0/0x3b4/0x3b8/0x3bc` groups configure the FFU slices that perform the
   lookup. This is the part still to be decoded properly.

**Next step:** capture a *second* route addition and diff it against this one. The delta isolates
exactly which writes are "insert one prefix" versus one-time slice setup — which is the last thing
needed before we can generate routes ourselves rather than replay them.

Note this is also the first concrete evidence that ECMP is expressible: two NEXTHOP entries already
exist (one per port), which is what the pre-existing `10.99.99.0/24` ECMP route uses.
