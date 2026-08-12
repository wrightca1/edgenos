# What makes a port routed: the complete register recipe

**2026-08-12.** A routed front-panel port on the FM6000 is **six registers**. Not the GLORT CAM,
not the L2F port masks, not `SAF_MATRIX`, not `LBS_CAM` — none of which change at all.

This was obtained by **differential observation of the live chip**: configure EOS one way, dump the
chip, configure it the other way, dump again, diff. No EOS file was read, no SDK was traced, and
nothing proprietary is recorded here — the result is a list of register addresses and the values
the hardware holds in two states, which are facts about the chip.

## The question it answers

The previous document (`PORT3-BRINGUP.md`) ended by concluding that `fwd4.txt` captures an EOS in
which **Et1 is routed and Et3 is an access port in VLAN 1**, so the replay contains no path to
port 41 *by construction*. That was correct but unsatisfying — it said what was missing, not what
to build.

The user's question — *"why aren't we routed on et3 if we set our address on our system and our
test system"* — is the right one, and its answer is short: assigning an IP to `et3` under EdgeNOS
configures a **Linux TAP netdev**. `et3` is created by `fm6000_portd`; giving it an address tells
the kernel something and tells the ASIC nothing. The chip's notion of "this port is routed" lives
in the six registers below, and nothing in EdgeNOS was writing any of them.

So the experiment became: make EOS do it, and watch exactly what it touches.

## Method

```
                     Et3 = access port, VLAN 1          Et3 = routed, 10.99.99.1/24
                     (what fwd4.txt captured)           (what we want)
                              |                                   |
                          fmdump N regions  ------ diff ------ fmdump N regions
```

```sh
# on the switch, under EOS
sudo /mnt/flash/fmdump <start_hex> <count> > /tmp/dump.<state>/<region>.txt
```

Regions dumped, from `FM6000_*_BASE` in the register header (read at runtime, never committed):
`GLORT 0x0E000`, `L3AR 0x10000`, `L2L 0x30000`, `PARSER 0x100000`, `MAPPER 0x120000`,
`L2AR 0x140000`, `MOD 0x150000`, `NEXTHOP 0x160000`, `L2F 0x180000`, `FFU 0x300000`.

Diff on the box, not off it — `scp` cannot work through the EOS CLI (`Received message too long`,
because the shell emits CLI banner text), and shipping ~190k lines through the CLI is pointless
when `awk` can do the comparison in place:

```sh
awk 'NR==FNR{a[$1]=$2;next} a[$1]!=$2{print $1, a[$1], $2}' dump.access/$n.txt dump.routed/$n.txt
```

⚠ The EOS CLI parses every line you send, so a here-doc does not survive — each line comes back as
`% Invalid input`. Everything has to be one `bash` invocation with `;` separators. And `fmdump`
needs `sudo`; as `admin` it fails with `resource0: Permission denied`.

## Result: ten differing words in the entire chip

| address | register | access (VLAN 1) | routed |
|---|---|---|---|
| `0x123053` | `MAPPER_SRC_PORT_TABLE[41]` w1 | `0x00000021` | `0x00000049` |
| `0x1213f0` | `MAPPER_VID1_TABLE[1008]` w0 | `0x00000000` | `0x00010000` |
| `0x1223f0` | `MAPPER_VID2_TABLE[1008]` w0 | `0x00000001` | `0x0000001b` |
| `0x122001` | `MAPPER_VID2_TABLE[1]` w0 | `0x00000401` | `0x00000001` |
| `0x0327e0` | `L2L_EVID1_TABLE[1008]` w0 | `0x000003f0` | `0x020003f0` |
| `0x150fc0` | `MOD_L2_VLAN1_TX_TAGGED[1008]` w0 | `0x00000000` | `0x00000001` |
| `0x16001a` | `NEXTHOP_TABLE[13]` w0 | `0x3581cab4` | `0x110e1e61` |
| `0x16001b` | `NEXTHOP_TABLE[13]` w1 | `0x03ef80a2` | `0x03f0bc24` |
| `0x140043`, `0x1401c3`, `0x140403` | L2AR (unindexed) | `0xfffffeff` | `0xfffffcff` |

**And these differ by zero words: `GLORT`, `L3AR`, `L2F`, `PARSER`, `FFU` (`0x300000`–`0x30ffff`),
`FIBM`.**

That last line is the important one. A previous session spent its length programming
`GLORT_CAM`, `GLORT_RAM`, the `L2F` destination mask, the `L2F` per-port permission mask,
`SAF_MATRIX`, `LBS_CAM` and `MCAST_LOOPBACK_SUPPRESS` to make port 41 reachable. **EOS changes
none of them when it makes a port routed.** Every one of those was a correct register write
serving a mechanism that was never involved.

## Reading the result

### 1008 is an internal VLAN, and it is allocated, not derived

`0x3f0` = 1008 indexes `MAPPER_VID1_TABLE`, `MAPPER_VID2_TABLE`, `L2L_EVID1_TABLE` and
`MOD_L2_VLAN1_TX_TAGGED` — all of which have `ENTRIES 4096`. It is a **VLAN id**, and EOS
allocates one internal VLAN per routed interface:

| interface | logical id |
|---|---|
| `Ethernet2` | `0x03ee` |
| `Ethernet1` | `0x03ef` |
| `Ethernet3` | `0x03f0` |

Downward from `0x3f0`, in configuration order — an allocator's counter. **It is not derived from
the port number.** Earlier work assumed port 3 would be `0x03ed` by analogy with Et1's `0x03ef`
and programmed that value; the chip's own answer is `0x03f0`, and it only became `0x03f0` when
the port was configured, because before then no internal VLAN existed for it at all.

This also retires a long-standing ambiguity about Et1's `0x03ef`, which had been recorded as an
"SGLORT". It is the internal VLAN / logical id, and it appears in the next-hop entry as the
**egress VLAN**.

### The field decode

From the register header's `_l_`/`_h_`/`_b_` field bounds:

```
MAPPER_VID1_TABLE   MAP_VID1[11:0]  VID1_TAG[13:12]  VID1_MuxSelect[15:14]  VID1_Flag[16]
MAPPER_VID2_TABLE   MAP_VID2[11:0]  VID2_TAG[13:12]  VID2_MuxSelect[15:14]  VID2_Flag[16]
MAPPER_SRC_PORT_TABLE  SRC_PORT_ID1..4[31:0]  Flag1[32] Flag2[33]  QOS_TAG[38:34]
NEXTHOP_TABLE       NextHop[63:0]           (WIDTH 2, ENTRIES 65536)
```

- `MAPPER_VID1_TABLE[1008]`: `0x00010000` sets **bit 16, `VID1_Flag`** — and nothing else. This
  single bit is the "this VLAN is routed" mark.
- `MAPPER_VID2_TABLE[1008]`: `MAP_VID2` 1 → 27.
- `MAPPER_VID2_TABLE[1]`: VLAN 1's `MAP_VID2` 1025 → 1 — the port leaving the access VLAN.
- `MAPPER_SRC_PORT_TABLE[41]` word 1: only `QOS_TAG[38:34]` moves, 8 → 18. Despite the name this
  behaves as a **per-port profile selector**; the four `SRC_PORT_ID` bytes in word 0 do not change.
- `L2L_EVID1_TABLE[1008]`: `0x3f0` → `0x020003f0` — sets `ET_IDX` (bits 32:25) to 1, leaving
  `MA1_FID1[11:0]` = the VLAN.
- `MOD_L2_VLAN1_TX_TAGGED[1008]`: 0 → 1 — bit 0 of a 76-bit `PortMask[75:0]`.

⚠ **Both of these are strided, and reading them at stride 1 gives a wrong and very convincing
answer.** The header is explicit:

```
L2L_EVID1_TABLE(index, word)         = 0x2*index + 0x32000 + word    WIDTH 2
MOD_L2_VLAN1_TX_TAGGED(index, word)  = 0x4*index + 0x150000 + word   WIDTH 3, PortMask[75:0]
```

Read at stride 1, Et1's `L2L_EVID1_TABLE` appears to be `0x00000000` (it is really word 1 of its
own entry) and `MOD_L2_VLAN1_TX_TAGGED` appears to differ between Et1 and Et3 — leading to
"these two registers are inconsistent across routed ports, so they can't be load-bearing." At the
correct stride all three routed ports are **identical**:

| register | Et2 (1006) | Et1 (1007) | Et3 (1008) |
|---|---|---|---|
| `L2L_EVID1_TABLE` w0 | `0x020043ee` | `0x020043ef` | `0x020003f0` |
| `MOD_L2_VLAN1_TX_TAGGED` w0 | `0x00000001` | `0x00000001` | `0x00000001` |

Every one carries `ET_IDX = 1` with `MA1_FID1 = V`. (Et1/Et2 additionally set `ETAG1 = 1`, an
egress tag id; Et3 has `ETAG1 = 0` and routes correctly, so it is not part of the recipe.)

This is the same class of error as indexing `L2F`'s second dimension 0..3 when the SDK indexes it
by port number. **Check `_WIDTH` in the header before reading any table.** The generated init files
show the stride too — `fm6000_l2linit.c` emits `0x327da=0x3ed, 0x327db=0, 0x327e0=0x3f0` and the
alternating zeros are the giveaway.
- L2AR `0xfffffeff` → `0xfffffcff` clears one more bit in an action mask at three addresses.

### The next-hop entry, and glean → resolved

`NEXTHOP_TABLE` is 2 words. The layout is legible directly from the values:

```
word 0 = MAC[31:0]
word 1 = [31:16] egress logical id (internal VLAN)   [15:0] MAC[47:32]
```

Confirmed against ARP on the live box:

```
NEXTHOP_TABLE[10] = 3581cab4 / 03ef80a2   ->  80a2.3581.cab4 via 0x03ef (Et1)   = 10.101.101.25
NEXTHOP_TABLE[12] = 3581cab5 / 03ee80a2   ->  80a2.3581.cab5 via 0x03ee (Et2)   = 10.101.101.33
NEXTHOP_TABLE[13] = 110e1e61 / 03f0bc24   ->  bc24.110e.1e61 via 0x03f0 (Et3)   = 10.99.99.2
```

`show ip arp` agrees line for line. And entries that carry no MAC —

```
NEXTHOP_TABLE[15] = 0000ff16 / 4fff0000
```

— are **glean** entries: a connected subnet whose hosts are not yet resolved, punt to the CPU.
This was observed live: toggling Et3 out of and back into the routed configuration made
`NEXTHOP_TABLE[13]` revert to the peer's values, and a single ping to `10.99.99.2` made it
reappear as `110e1e61 / 03f0bc24`. The glean → ARP → resolved cycle, watched in the register.

## The FIB is a BST in the FFU, and it is nowhere near where we were looking

The route itself is not in `0x300000`–`0x30ffff`. It is in `FFU_BST_KEY`, whose addressing the
header gives as:

```
FFU_BST_KEY(i2,i1,i0)            = 0x10000*i2 + 0x400*i1 +   i0 + 0x308000     [4][16][1024]
FFU_BST_ACTION_ROUTE(i2,i1,i0,w) = 0x10000*i2 + 0x800*i1 + 2*i0 + 0x300000 + w  [4][16][1024], WIDTH 2
```

so the four trees live at `0x300000`, `0x310000`, `0x320000`, `0x330000` — three of which a
64k-word dump from `0x300000` never reaches. Dumping the full `0x300000`+262144 and searching for
the prefix as a **value** finds it immediately, with no second capture needed:

```
0x32bffc = 0a636300     FFU_BST_KEY(2,15,1020)  = 10.99.99.0
0x33bfdb = 0a636300     FFU_BST_KEY(3,15, 987)  = 10.99.99.0
0x33bfdc = 0a636301                             = 10.99.99.1     (our address)
0x33bfdd = 0a6363ff                             = 10.99.99.255   (broadcast)
0x32bbfd = 0a656518                             = 10.101.101.24  (Et1 subnet base)
0x32bbfe = 0a656520                             = 10.101.101.32
```

The keys are **sorted IPv4 boundaries** — a binary search tree over the address space, which is
what "BST" means and why the entries cluster at subnet edges rather than at prefixes.

`FFU_BST_ACTION_ROUTE` fields:

```
NextHopBaseIndex[15:0]  NextHopRange[22:16]  NextHopEntryType[23]  LPM[31:24]
TagData[43:32]  TagCmd[45:44]  Route[46]  Precedence[49:47]
```

The action paired with the 10.99.99.0 key reads `0x0800000f / 0x00014000` →
`NextHopBaseIndex = 15`, `Route = 1`, `Precedence = 2`. Index 15 is the glean entry above, which
is exactly right for a connected subnet.

### The BST is a sorted boundary array, right-aligned

Dumping whole blocks and counting shows the layout directly:

| tree / block | keys | occupies | |
|---|---:|---|---|
| tree 2, block 14 | 4 | i0 1020–1023 | |
| tree 2, block 15 | 5 | i0 1019–1023 | |
| tree 3, block 14 | 49 | i0 975–1023 | |
| tree 3, block 15 | 48 | i0 976–1023 | |

Every block is **sorted ascending and right-aligned against i0 = 1023**, ending at `7f000001`
(127.0.0.1) — the top of the IPv4 space the box has a route for:

```
... 0a656518  0a656519  0a65651a  0a65651f  0a656520 ...
    0a660100  0a680100  0a690100  0a6b0100  0ac70100  0ac80100  47b54400  7f000001
```

So this is **interval-based LPM**: the sorted boundaries partition the address space and each
boundary carries the action for the range it opens. "BST" describes the search, not the storage —
there are no child pointers, just a sorted array to binary-search.

Tree 3 holds 97 boundaries against the box's ~39 routes, ≈2.5 per route, which matches the
`.0 / .1 / .255` triple observed for `10.99.99.0/24` (network, local address, broadcast). Tree 2
holds 9. Capacity is 4 × 16 × 1024 = 65,536 boundaries.

### Updates are double-buffered, and the root key says which half is live

`FFU_BST_ROOT_KEYS` has exactly one populated entry per engine, at index 15 (right-aligned, the
same convention as the key arrays). The header gives its fields:

```
Key[31:0]   Top4KeyInvert[35:32]   Top4Key[39:36]   Partition[43:40]
```

which decodes the two observed words immediately:

| engine | `Top4Key`/`Invert` | `Partition` |
|---|---|---|
| 2 | `0x1`/`0xe` — ternary `0b0001` exactly | 15 |
| 3 | `0x1`/`0xe` — same | 14 |

`Partition` is the block index, and the two populated blocks per engine are an **active/standby
pair**. The occupancy is the tell: the active block held one *more* key than the standby in both
engines (5 vs 4, 49 vs 48), as if the last update had added a boundary to the half that then went
live.

**Tested by prediction rather than assumed.** Adding `ip route 192.0.2.0/24 Null0` on the live box:

| | before | after add | after remove |
|---|---|---|---|
| root word 1 | `0x0e1e` → partition **14** | `0x0f1e` → partition **15** | `0x0e1e` → partition **14** |
| block 14 keys | 49 | 49 — untouched | 49 |
| block 15 keys | 48 | **50** | 50 — stale, left alone |

The active block is never written. The standby is rebuilt in full, then one field in one root key
flips to publish it. The new key `c0000200` = `192.0.2.0` appeared at `0x33bfff`, i0 = 1023 — the
top slot, because it had become the highest boundary in the table.

So the insertion algorithm is:

1. Read `Partition` from `FFU_BST_ROOT_KEYS[engine][15]` — that is the live block.
2. Build the full sorted boundary array for the new route set.
3. Write it into **the other** block, right-aligned against i0 = 1023, with the paired
   `FFU_BST_ACTION_ROUTE` entries.
4. Flip `Partition[43:40]` in the root key to the block just written.

That is a hitless update, it explains the replay's very large write counts in this region (every
change rewrites a whole partition), and it is everything `fm6000_fibd` needs to program routes.
Note that the standby half is left holding stale keys — 50 above, after the route that produced
them was removed — so a reader must follow `Partition` and must not assume the other half is empty
or current.

### `LPM[31:24]` is 32 − prefix length

Dumping the live block with decoded actions settles the field I had recorded as unknown:

| key | `lpm` | prefix | 32 − len |
|---|---|---|---|
| `10.101.255.1` loopback | `0x00` | /32 | 0 |
| `10.101.101.40` | `0x03` | /29 | 3 |
| `10.102.1.0` | `0x08` | /24 | 8 |
| `71.181.68.0` | `0x0a` | /22 | 10 |

It is the **host-bit count of the route that owns the interval**. That is what an interval scheme
needs: a boundary opens a range that may extend past the end of its own prefix, so the hardware
still has to check the address actually falls inside. My earlier reading — "0x00 on connected,
0x08 on OSPF-learned, both /24s, so not a prefix length" — compared a /32 *boundary entry* against
a /24 *route entry* and concluded the field was something else. Both were prefix lengths.

### The EdgeNOS BST is not a valid sorted array

Two separate defects, both measured with `fm6000_bst -d` on the box.

**1. Engine 3's key memory is never zeroed.** The live block reports 1008 non-zero "boundaries" of
which only ~57 are real routes; the rest read as raw SRAM (`0x1b45cd91`, `0x53feb7c2`). The BST is
four engines strided `0x10000`, and `fm6000_memfill`'s fills were sized for one:

```
BST_ACTION 0x300000 x131072 -> 0x300000-0x31ffff    engines 0,1
BST_KEY    0x308000 x 65536 -> 0x308000-0x317fff    engine 0
engine 3's keys live at        0x338000-0x33bfff    <- no fill at all
```

Engine 3 is the one holding the IPv4 unicast table. EOS's replay writes every *action* entry but
only the *populated* keys, because it expects CRM to have zeroed the array; PROBE MODE does no CRM
fill. Fixed by one 262144-word fill covering all four engines (fourth instance of this defect
class — a reconstructed fill sized for one copy of a structure the chip has several of).

**2. Even the written entries are a superposition of two EOS generations.** The array is
right-aligned, so a smaller table occupies only the top N slots and leaves the older, larger
table's lower entries in place. The result is non-monotonic:

```
0033bfef 0a65651f   10.101.101.31
0033bff0 0a656520   10.101.101.32
0033bff1 0a030100   10.3.1.0        <- decreases
...
0033bfff 7f000001   127.0.0.1
```

`0x33bff1`–`0x33bfff` is one coherent generation; `0x33bfec`–`0x33bff0` are leftovers from an
earlier one. Zeroing at boot is necessary but **not sufficient** — a replay that captures several
generations of EOS rebuilding this table leaves the union of them, since only the last write to
each address survives. A correct load must write all 1024 slots of the block it publishes,
including the zeros, which is what EOS itself does.

This also explains why unused slots matter at all: with zeros below it, a right-aligned sorted
array is still globally sorted (`0,0,…,0,k₁,k₂,…`) and binary search works. With garbage or stale
entries it is not sorted, and the search is undefined.

### What boundaries to emit — the rule, read off a live table

Correlating all 49 boundaries of a converged EOS FIB against `show ip route` and `show ip arp`
gives the complete construction rule. `lpm` is `32 − len` in every single case, and the nexthop
index encodes the entry's *kind*:

| nexthop | kind | precedence | examples |
|---|---|---|---|
| 5 | **glean** — a connected subnet's network and broadcast addresses | 2 | `10.1.1.0`, `10.99.99.255`, `10.101.101.31` |
| 7 | **local** — an address belonging to this box | **3** | `10.99.99.1`, `10.101.101.26`, `10.101.255.1` |
| 11, 12, 13 | **resolved neighbour**, one entry each | 2 | `10.99.99.2`, `10.101.101.25`, `10.101.101.33` |
| 16 | **the gateway** — every remote route shares it | 2 | all the OSPF `/24`s and `/29`s |
| 6 | loopback | 2 | `127.0.0.1` |

So for each route in the RIB, emit one boundary at its network address with `len` = the route's
prefix length and the gateway's nexthop. For each **connected** subnet additionally emit `/32`
boundaries: the network address and the broadcast as glean, this box's own address as local
(precedence 3), and each resolved neighbour pointing at its own nexthop entry. That is why
`10.101.101.24`, a `/29` connected subnet, carries `lpm 0` — the entry is the `/32` network-address
boundary, not the subnet route.

`0.0.0.0/0` is **not** a boundary. Its action sits in the slot immediately below the lowest key,
whose key is zero, because a binary search for anything below the first boundary lands there. On
the captured table that is index 974, one below the 975 where the boundaries start.

### Both halves, and how they compose

```sh
fm6000_fibgen --map et1=0x3ef,et3=0x3f0 --nh-out /tmp/nh | fm6000_bst -p /dev/stdin
fm6000_bst -N /tmp/nh
```

`fm6000_fibgen` is the **policy** half — it walks `/proc/net/route`, `/proc/net/arp` and the
interface addresses and emits the boundary list plus the `NEXTHOP_TABLE` entries it refers to.
`fm6000_bst -p` is the **mechanism** half — sort, right-align, write all 1024 slots of the standby,
flip `Partition`.

Both are validated against the live EOS FIB, and neither validation is circular:

- `fm6000_bst -p`: fed EOS's own 49 boundaries **in reverse order**, it reproduces EOS's block byte
  for byte — 0 key mismatches, 0 action mismatches, base index 975 = 1024−49, every slot below the
  table zero.
- `fm6000_fibgen`: fed EOS's routing state converted into kernel formats, it produces **46
  boundaries against EOS's 46, 0 missing, 0 extra**, with no difference in prefix length or
  precedence, and next-hop entries whose contents match EOS's byte for byte.

Two differences from EOS are deliberate:

- EOS puts its **management** subnet in the hardware FIB (`10.1.1.0/24` on `Management1`, three
  boundaries). `fibgen` ignores any interface absent from `--map`, and management is not an ASIC
  port on EdgeNOS. That is the entire difference between 49 and 46.
- EOS allocates a **separate next-hop index for the gateway role** (16) that duplicates the
  neighbour's entry (12) byte for byte. `fibgen` dedups by `(mac, logical id)`, so remote routes
  share the neighbour's entry. Functionally identical, one entry fewer.

⚠ The **glean, local and loopback** next-hops are referenced by index, not built. They carry logical
id `0x4fff` with trap codes `0xff15`/`0xff16` in the pseudo-MAC, and those codes are not decoded.
The defaults 5/7/6 are EOS's allocation *on this box* and are command-line options — depending on
someone else's allocation is precisely the mistake that produced the wrong `0x3ed` for port 3.

⚠ Also unverified: none of this has yet moved a packet. It reproduces a known-good table exactly,
which is strong evidence the encoding is right, but "writes the same bytes EOS writes" is not the
same claim as "forwards". That needs a boot with the `memfill` BST fix and transit traffic.

### Method note

**Searching for a known value beats capturing a second state.** The two-state diff found the
per-port configuration; it would never have found the route, because the route is present in only
one state and a diff of the wrong address range reports `differs=0` — which is indistinguishable
from "nothing here" and was, briefly, believed.

## What this means for EdgeNOS

The recipe for bringing up a routed front-panel port is now fully specified and none of it needs
an EOS file:

1. Allocate an internal VLAN `V` for the port.
2. `MAPPER_SRC_PORT_TABLE[port]` — set the routed profile in `QOS_TAG[38:34]`.
3. `MAPPER_VID1_TABLE[V]` — set `VID1_Flag` (bit 16).
4. `MAPPER_VID2_TABLE[V]` — set `MAP_VID2`; clear the port out of its old access VLAN's entry.
5. `L2L_EVID1_TABLE[V]` — set bit 25, keep the VLAN body.
6. `MOD_L2_VLAN1_TX_TAGGED[V]` — set 1.
7. `NEXTHOP_TABLE[k]` — `{MAC[31:0]}`, `{V<<16 | MAC[47:32]}` per resolved neighbour; MAC-less
   glean entry for the connected subnet.
8. `FFU_BST_KEY` / `FFU_BST_ACTION_ROUTE` — the prefix boundaries and their next-hop indices.

Steps 1–6 are a static per-port bring-up. Step 7 is what `fm6000_fibd` should be writing when the
kernel resolves ARP, and step 8 is what it should be writing when a route appears. Both are
generator work of the same kind already done for L3AR, and both are now specified by observation
rather than by transcription.

## Applying it under EdgeNOS

`fm6000_rport 41 0x3f0 -v`, run on EdgeNOS (`edgenos-special.swi`) after `FULLSEQ DONE`:

```
  00123052 02000000 -> 02000000   MAPPER_SRC_PORT_TABLE w0
  00123053 00000021 -> 00000049   MAPPER_SRC_PORT_TABLE w1
  001213f0 00000000 -> 00010000   MAPPER_VID1_TABLE
  001223f0 00000000 -> 0000001b   MAPPER_VID2_TABLE
  000327e0 000003f0 -> 020003f0   L2L_EVID1_TABLE w0
  00150fc0 00000000 -> 00000001   MOD_L2_VLAN1_TX_TAGGED w0
  VERIFY PASS (9 words)
```

**The before-state is itself a result.** `0x123053 = 0x21`, `0x327e0 = 0x3f0`, and VID1 / VID2 /
TX_TAGGED all zero is *exactly* the access-port state measured on EOS. `fwd4.txt` really does
capture Et3 as a VLAN-1 access port, now confirmed from the EdgeNOS side without reference to the
EOS capture at all.

### What this does and does not establish

It establishes that the recipe is complete, correctly addressed, and lands on a chip programmed by
our own boot. It does **not** establish that port 41 forwards, and two things stand in the way:

- **The port-3 link is down under EdgeNOS** — `PORT_STATUS(EPL14, lane1)` at `0xe3880` reads
  `0x15`, the wire-init state, and `0xe3884` holds `0x1002` where Et1's lane holds `0x9b87`. The
  bring-up recorded in `PORT3-BRINGUP.md` applies EOS's captured lane-1 block (`lane1.txt`, 30
  non-zero words), and that file is not on flash. Hand-rolling SerDes configuration on a remote
  box is how this box was wedged once already, so it was not attempted.
- **Transit needs routes.** Static per-port state is only half the recipe; the FIB entries and
  next-hop still have to be programmed, and `fm6000_fibd` cannot yet do it (see its header). The
  decode above removes the blocker but the implementation is not written.

So: recipe validated on both operating systems, forwarding not yet demonstrated on EdgeNOS.

## Lab state as left

`Ethernet3` is configured as a routed port with `10.99.99.1/24`, advertised into OSPF area 0 as
`passive-interface`, and the configuration is saved (`write memory`). The test host at
`10.22.1.56` has `10.99.99.2/24` on `eth1` plus routes to `10.101.101.24/29` and `10.101.255.1/32`
via `10.99.99.1`.

Verified on EOS:

- switch → test host: **0% loss**, ARP resolved, `C 10.99.99.0/24 is directly connected, Ethernet3`
- test host → switch Et3 address: **0% loss**
- test host → switch loopback `10.101.255.1`: **0% loss** — traffic ingressing Et3 and routed to a
  destination that is not on a connected interface, which is the ingress half of transit
- test host → peer `10.101.101.25`: **100% loss** — the peer has no return route to
  `10.99.99.0/24` yet. This is a peer-side routing question, not a switch or ASIC one, and it does
  not affect anything above.

**A replay captured from this configuration would, for the first time, contain a path to port 41.**
That was the recommendation at the end of `PORT3-BRINGUP.md` and it still stands — but it is now
optional rather than necessary, because the ten words that constitute the path are enumerated
above and can simply be written.
