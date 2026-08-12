# Bringing up front-panel port 3 on EdgeNOS

**2026-08-11.** Front-panel port 3 is now up under EdgeNOS. This is the second connected port, and
it is what unblocks the transit traffic that A4 (MOD command split) and B1 (FFU ByteMux) need —
neither can be settled with CPU-terminated traffic alone.

## The port

From `asic/fm6000/fm6000_serdes_ports.h`:

```
{ 1, 40, 14, 0, ... }   front-panel 1 = alta 40, EPL14 lane 0   (Et1)
{ 3, 41, 14, 1, ... }   front-panel 3 = alta 41, EPL14 lane 1
```

Port 3 is **the same EPL as Et1, one lane over**, and both share `rxpol=0, txpol=1`. EPL14 is
already up and clocked for Et1, which is why this is lane-level work and not a full EPL bring-up.

## The recipe

Not derived from first principles — **read off a working EOS boot**, which has the port up as
`Et3 connected 10GBASE-SR` because the test host is plugged in. See `EOS-VS-EDGENOS-DIFF.md` for
how that reference was captured.

```sh
# 1. enable EPL port 1 and select its PCS  (EPL14 shared config)
fm6000reg 0000:02:00.0 0xe3b01 0x7e1d7899     # EPL_CFG_A: bit 20 = Active_1
fm6000reg 0000:02:00.0 0xe3b02 0x00090033     # EPL_CFG_B: bits 4-7 = Port1PcsSel = 3

# 2. apply EOS's lane-1 block, 0xe3880-0xe38ff (30 non-zero words)
fmload lane1.txt
```

`EPL_CFG_A` bit 20 and `EPL_CFG_B` bits 4–7 were identified from the header
(`FM6000_EPL_CFG_A_b_Active_1`, `FM6000_EPL_CFG_B_l_Port1PcsSel`) — the two bits are the *only*
genuine configuration differences between EOS and EdgeNOS in the whole EPL region.

⚠ Step 1 alone is **not** sufficient: with only `Active_1`/`Port1PcsSel` set the lane stayed at
`0x00000015`. The lane-1 block carries the rest.

## Result

```
PORT_STATUS(EPL14, lane1) = 0xe3880
   before: 0x00000015   LinkFaultDebounced + LinkFaultMac + LinkFaultRx, RxLinkUp clear
   after:  0x000008c0   RxLinkUp, HeartbeatOk, SerXmit      <- stable over 30s
```

Et1 is unaffected throughout (`lane0` alternating `0x8c0`/`0xec0`, which is the Receiving bit
toggling on live traffic), `routes=34`, `PIN_STRAP=0x208`.

## Ingress is proven working

Controlled measurement — the test host given `10.99.99.2/24` on eth1 and made to transmit, while
`PORT_STATUS(EPL14, lane1)` is sampled once a second:

```
host idle:          0x8c0 0x8c0 0x8c0 0x8c0 0x8c0 0x8c0
host transmitting:  0xcc0 0xcc0 0xcc0 0xcc0 0xcc0 0xcc0     <- bit 10, Receiving
```

Six consecutive samples each way. The ASIC receives the test host's frames on port 3. The RX half
of the path is done.

**Nothing egresses yet**: `tcpdump -i eth1` on the test host captured 0 packets over 12s, and its
`rx_packets` counter did not move. Port 3 has link and receives, but is in no forwarding domain.

## What is still needed for transit traffic

Link is up; forwarding through it is not configured.

- **GLORT.** `PARSER_INIT_FIELDS[41]` reads `0x00010103` — logical id `0x103`, default SGLORT
  `0x0001`. **EOS leaves it at the default too**, because Et3 is a switched VLAN-1 port rather than
  a routed one; `GLORT-MAPPING.md` records that only configured-up routed ports (Et1 `0x03ef`,
  Et2 `0x03ee`) get real GLORTs. So a unique SGLORT plus a `GLORT_CAM` entry is needed only if we
  want to *inject* on port 3 from the CPU.
- **For transit we may not need that at all.** Wire→wire forwarding does not involve the CPU: a
  frame ingressing port 3 and egressing Et1 exercises the full pipeline including MOD, with no
  portd on port 3.
- **The capture direction that matters** is `peer → Et1 → switch → port 3 → test host`, because
  the test host is ours and can run tcpdump. That is the egress observation A4 has been blocked on
  since the beginning — MOD's edits become directly visible.

## Note on signals

`routes=34` is the reliable health metric here, not ping. Ping collapses to 100% loss on this box
for unrelated reasons (checklist D5) — it did so during this very session while OSPF was converged
with 34 routes and the FIB programmed. Do not read a ping failure as a forwarding failure.


---

## 2026-08-11, pulling the egress thread: three findings

### 1. The DGLORT→DestMask path is not populated at all

`GLORT_RAM.DMaskBaseIdx` indexes `FM6000_MCAST_DEST_TABLE` (`0x240000`, 4096 entries × 4 words,
`DestMask[75:0]` — one bit per port, so bit 40 = Et1, bit 41 = port 3).

Dumped all 4096 entries on a live chip with Et1 forwarding: **every word is zero.** No entry
selects port 40, or any port. So the chain GLORT_CAM → GLORT_RAM.DMaskBaseIdx → MCAST_DEST_TABLE
is not how a CPU-injected frame reaches Et1 — it cannot be, because the table is empty.

Combined with the earlier result that no `GLORT_CAM` entry matches `0x03ef` at all, the conclusion
is firm: **the ISL/F64 DGLORT does not resolve through the GLORT/DMask machinery.** Whatever
carries a CPU-injected frame to its egress port is something else, and both of the obvious
candidates are now eliminated by measurement rather than argument.

### 2. portd instances share DMA rings — you cannot run two

Running a second portd for `et3` alongside the `et1` one broke the dataplane: `routes` fell
34 → 3 while the et3 instance consumed **1,707 frames**. Killing it restored `routes=35`
immediately.

`edgenos-up.sh` warns about this for a *second run of the script*; the same hazard applies to a
second portd on a **different port**, because the rings are shared and **not demultiplexed by
port**. The second instance simply steals frames the first one needed.

So port 3 cannot be given its own portd as things stand. Supporting two CPU-attached ports needs
portd extended to demultiplex one ring set across ports — presumably on the ISL tag's source
GLORT, which is exactly what `PARSER_INIT_FIELDS[port]` stamps on ingress.

### 3. ⚠ RETRACTED: port 3's ingress does NOT reach the CPU

I originally read the 1,707 frames the et3 portd consumed as proof that port 3's receive path
worked end to end. **That was wrong, and it followed from finding (2).** The DMA rings are shared
and *not demultiplexed by port*, so an et3 portd reads whatever is in the ring — which was Et1's
traffic. The count proved the rings were being drained, not where the frames came from.

Direct check with `fm6000_rxdump` and no portd running, while the test host transmitted: **every
punted frame carries tag word 1 = `0x03ef`**, Et1's SGLORT. Nothing from port 3 is punted at all.

What is established for port 3 is the MAC-level measurement only: `PORT_STATUS` bit 10 `Receiving`
tracks the test host's transmission exactly (`0x8c0` idle, `0xcc0` transmitting, six samples each
way). The MAC receives; the frames go no further, because port 41 is in no forwarding domain.

## The punt frame format, decoded

`fm6000_rxdump` with no portd competing shows the real layout — and it is simpler than portd
assumes:

```
33 33 00 00 00 05 | 80 a2 35 81 ca b4 | 07 01  03 ef  00 01  ff ff | 86 dd ...
DMAC (0..5)         SMAC (6..11)        F64 tag: 4 x 16-bit at 12    ethertype (20)
```

- **There is no receive prefix.** The frame starts at offset 0 every time. portd scans offsets
  0..39 for a plausible ethertype because the framing was never characterised; it always finds 0.
- The **F64 tag is inline at offset 12**, four 16-bit words, exactly where portd puts it on inject.
- **Tag word 1 is the GLORT: source on RX, destination on TX.** Every punted frame from Et1 carries
  `0x03ef`, which is precisely `PARSER_INIT_FIELDS[40] >> 16`. portd's TX tag is
  `{0x0100, 0x03ef, 0xff00, 0x0000}` — same slot, opposite direction.
- Word 0 varies with frame type (`0x0701` on IPv6/OSPF multicast, `0x0301` on MLD) — a flags or
  priority field, not yet decoded.
- Word 3 is `0xffff` on RX, `0x0000` on TX.

**This is the demux key multi-port portd needs**: tag word 1 identifies the ingress port, provided
each port has a distinct SGLORT.

## ⚠ PARSER_INIT_FIELDS cannot be written after boot

Giving port 41 a unique SGLORT is a single register write —
`PARSER_INIT_FIELDS[41] = (0x03ed << 16) | 0x103` at `0x1082a4` — and **it does not stick**. The
register still reads `0x00010103` immediately afterwards. `fm6000reg` writes work fine elsewhere
(EPL_CFG_A/CFG_B took in this same session), so this is the parser tables specifically.

That is phase 76/78's *writability is a boot state* again: the replay's `PARSER_INIT_FIELDS` writes
land during fullseq, when the memory subsystem is writable, and post-boot writes do not. So port
41's SGLORT has to be set **in the boot path** — in a generator or spliced into the replay — not
interactively. That is a small, well-defined change and it is the next concrete step.


---

## Two constraints that shape the fix

### `PARSER_INIT_FIELDS` cannot simply be moved into a generator — already tried, already backed out

The obvious fix for "post-boot writes don't stick" is to have our own code write
`PARSER_INIT_FIELDS` during boot, via the `gen_list` mechanism that already strips replay writes
in favour of a generator. **That was tried and reverted on measured evidence.** From
`fm6000_tbl3init.c`'s own header:

> `PARSER_INIT_FIELDS` 970 writes. Passed its first boot 7/8 rounds clean, then a 3-boot soak gave
> 2 clean and 1 at 90-100% loss. TBL3 alone soaks 3 of 3 clean, so this looks like a reliability
> regression rather than variance.
>
> 970 writes is not worth degrading a platform that already fails 1 boot in 6.

(`ESCHED_DRR_Q` was backed out the same way: OSPF up at 35 routes, RX alive, 100% ping loss on 8
of 8 rounds, bisected to that register alone.)

So "own the GLORT allocation" cannot be done by lifting this table wholesale. The options left are
a **single-value edit** to the operator-supplied replay (one register, not a table move), or a
targeted write **inside `fm6000-fullseq.sh`** immediately after STEP5, while the memory subsystem
is still writable. The second is ours and does not touch the replay; both need an SWI rebuild.

⚠ And note the SGLORT alone is **necessary but not sufficient**: port 41 is in no forwarding
domain, so its frames are not punted at all. A unique SGLORT makes port-3 frames *identifiable* in
the punt stream; it does not make them *appear* there.

### One DMA-ring consumer per boot — and that includes `fm6000_rxdump`

`edgenos-up.sh` warns that restarting portd wedges the rings. The same applies to **any** tool that
opens them. Running `fm6000_rxdump` to capture the punt format and then starting portd via
`edgenos-up.sh` produced exactly the documented symptom:

```
[up]  t=96s  kernel routes=2  et1 rx=0
```

RX silently zero, no adjacency, and it looks precisely like a dataplane defect. A reboot and a
single consumer restored `routes=35, et1 rx=33`.

So: **capture with `rxdump`, or run portd — not both in one boot.** Reboot between them.


---

## Port 41 now has a real SGLORT — done, and it changes nothing yet

### What the replay actually does

The replay **does** write `PARSER_INIT_FIELDS[41]`, nine times, ending at `0x00010103`. (An earlier
note here said it never did; that was a `tail`-truncated grep, the same mistake that hid port 0
from the `PARSER_INIT_STATE` scan. Both are corrected.)

Reading the two ports side by side shows exactly what EOS does and does not do:

```
port 40 (Et1)   0 -> 0x00010000 -> 0x00010001 -> 0x03ef0001 -> 0x03ef0101
port 41 (p3)    0 -> 0x00010000 -> 0x00010003 -> 0x00010103
                                                 ^ never gets an SGLORT
```

EOS assigns `0x03ef` to Et1 because it configured that port up, and leaves port 41 on the default
`0x0001` because it never did. Nothing is special about port 41 — it is simply unconfigured.

### The change

One line of the operator-supplied replay, at its final write for that register:

```
312646:  001082a4 00010103   ->   001082a4 03ed0103
```

Backed up as `/mnt/flash/fwd4-preport3.txt`. Verified across a reboot:

```
PARSER_INIT_FIELDS[41] = 0x03ed0103    (was 0x00010103)
PARSER_INIT_FIELDS[40] = 0x03ef0101    (Et1, untouched)
```

This is the surgical option from the previous section — a single value, not the wholesale table
move that `fm6000_tbl3init.c` records as a measured reliability regression. It also sidesteps the
post-boot writability wall, because the write happens inside the replay where the memory is
writable.

### And it produces no new behaviour, as expected

With port 3 enabled and the test host transmitting continuously:

```
lane1 PORT_STATUS = 0x00000cc0     Receiving — the MAC has the frames
et1 capture, 40 frames            0 with the host's MAC, 0 with its payload pattern
```

So the SGLORT does what it says — it stamps an identity on frames ingressing port 41 — and that is
all it does. **Punting is a separate decision**, made by the forwarding tables, and port 41 is in
no VLAN or forwarding domain. This was stated in advance rather than discovered afterwards, and
the measurement confirms it.

What the change buys is that *when* port-3 frames do start reaching the CPU, they will be
identifiable by tag word 1 = `0x03ed`, which is the demux key multi-port portd needs. It is a
prerequisite that is now satisfied and verified, not a fix.

### Next

Port 41's VLAN/forwarding-domain membership. That is where the frames are being discarded, and it
is the last thing between here and transit traffic.


---

## The destination-resolution chain, found — and it is not MCAST_DEST

`fm6000_l2_probe` / `fm6000_l2.c` in this tree already implement the path, and reading them
answers the question the last several sections circled:

```
DGLORT --> GLORT_CAM[cam_idx] --> GLORT_RAM[cam_idx].DMaskBaseIdx = gid --> L2F_TABLE_256[gid]
                                                                            = 76-bit port bitmask
```

**The destination mask lives in `L2F_TABLE_256` (`0x1a0000 + 4*gid`), not `MCAST_DEST_TABLE`.**
That is why dumping all 4096 `MCAST_DEST_TABLE` entries found them uniformly zero on a chip that
forwards perfectly — it is simply not the table in use.

### Programmed for port 41 and verified

`fm6000_l2_probe 41 8 16` points the special-delivery GLORT at port 41 instead of the CPU:

```
GLORT_CAM[8]  = 0xff000000     matches exactly 0xff00
GLORT_RAM[8]  = 0x00000040     DMaskBaseIdx = 0x40>>2 = 16
L2F[16]       = w0 0x00000000  w1 0x00000200   <- bit 9 of word 1 = port 41
```

All three verified by readback. Dataplane stayed healthy throughout (routes 34, both lanes up).

### And injecting still does not egress port 3

```
fm6000_txinline 20 0100 ff00 ff00 0000
   frame[0:24]: 80 a2 35 81 ca b4 | 44 4c a8 31 5d ab | 01 00 ff 00 ff 00 00 00 | 08 00
   tag@12 = 0100 ff00 ff00 0000, queued=60 DONE=60 STATUS=0x00000012

lane1 PORT_STATUS = 0x000008c0     no Transmitting bit
test host tcpdump  = nothing
```

Frames are queued and completed by the DMA engine, the GLORT resolves to a mask naming port 41,
and nothing reaches the wire. So the mask is necessary but something downstream of it still
discards the frame — egress-port enable, VLAN membership on the egress side, or the MOD/egress
stage refusing a port it has no configuration for.

That is the current edge. Worth noting the same tag with `03ef` egresses Et1 perfectly, so the
inject path itself is sound and the difference is entirely port 41's configuration.


---

## Looking at EOS for the egress config — what it gave us

EOS has `Et3 connected 10GBASE-SR`, so it is a valid reference for port 41's egress. Dumped
`L2F_TABLE_256` (`0x1a0000`, 64K words) under both.

### EOS's masks that include port 41

```
L2F_256[0,2] [1,2] [2,2]   ports {0, 41}              CPU + port 3
L2F_256[3,0] [3,1] [3,3]   ports {0, 3, 20, 40, 41}   CPU + every configured port
```

EdgeNOS had **none** of them. Its only entry with bit 41 set was `[3,2]`, listing 51 scattered
ports — SRAM noise presented as a forwarding decision.

### Exactly three words differ

```
0x1a0009  eos=0x00000200   L2F_256[0,2].w1   bit 41
0x1a0409  eos=0x00000200   L2F_256[1,2].w1   bit 41
0x1a0809  eos=0x00000200   L2F_256[2,2].w1   bit 41
```

### ★ Wide entries commit on the last word

Writing `0x1a0009` alone **did nothing** — readback unchanged, which looks exactly like the
post-boot writability wall and is not. Writing the *whole* entry (w0, w1, w2 in order) took
immediately:

```
before  0x1a0008: 0x00000001 0x00000000 0x00000000
after   0x1a0008: 0x00000001 0x00000200 0x00000000
```

**L2F_TABLE_256 is a 3-word-wide entry and commits on the last word; a single-word write is
discarded.** This is worth remembering generally: a table that appears unwritable may simply be
wide. `fm6000_l2.c` always writes full entries, which is why its writes have always worked.

### And a third memfill gap

The garbage in `[3,2]` is another short fill run — the same defect as MOD and MAPPER:

```
{0x1a0000, 3072, ...}   covers to 0x1a0bff
garbage begins at       0x1a0c08   (602 words, all zero on EOS)
```

Fixed to 4096. Three of these now, all from fill lengths that were reconstructed rather than read.

### Still no egress

With the masks matching EOS, port 3 link up and traffic flowing, the test host still receives
nothing and `lane1` shows no Transmitting bit. The masks are correct but **nothing we generate
resolves to them** — the remaining unknown is which lookup selects `L2F_256[0..2, 2]`, i.e. what
`GLORT_RAM[i].DMaskBaseIdx` values point at those gids and which DGLORT reaches them.

That is now a small, well-posed question against a known-good reference, which is a much better
place than where this started.


---

## Frame type implemented and live — still no egress

The parser now carries the special-delivery rule, verified on the running chip:

```
LIVE: slice 3 entry 2   hw0=0x8000/care=0x8000   SetFlags[0,3]
EOS:  slice 3 entry 27  hw0=0x8000/care=0x8000   SetFlags[0,3]
one entry sets ACTION_FLAGS bit 0, same count as EOS
```

With every element below now verified against EOS on the same boot:

| element | state |
|---|---|
| port 3 link | up, `lane1 = 0x8c0` |
| port 41 SGLORT | `0x03ed` in `PARSER_INIT_FIELDS[41]` |
| `GLORT_CAM[8]` | matches `0xff00` |
| `GLORT_RAM[8]` | `DMaskBaseIdx = 16` |
| `L2F[16]` | `w1 = 0x200` = port 41 |
| parser SPECIAL rule | live, matching EOS's encoding |
| inject | `tag@12 = 8100 ff00 ff00 0000` |

**Nothing egresses.** `lane1` never sets Transmitting, the test host's `rx_packets` does not move,
and a NORMAL/SPECIAL A/B gives identical results.

So the frame type was necessary to decode and implement — our parser genuinely could not express
it, and now can — but it is **not sufficient**, exactly as the SGLORT and the L2F mask were not.
Three prerequisites satisfied in a row without reaching the goal.

### What that pattern suggests

Each fix has been verified in isolation and none has changed the outcome, which points at
something structural rather than another missing table entry. The untested link in the chain is
**L2AR** — the block that makes the actual forwarding decision, and the one this project has never
decoded (checklist A2, blocked on `DMT_PROFILE` / CPU-code / mirror semantics). Every element we
have verified is *upstream* of it (parser, GLORT resolution, destination mask) or *downstream*
(MOD egress). L2AR sits in the middle and is the one thing we cannot currently inspect.

The honest read is that CPU-inject to an arbitrary port needs L2AR configuration we have not
decoded, and that A2 is therefore not merely the next item on the list but a prerequisite for
this one.


---

## Following the replay's own recipe for Et1

Rather than decode L2AR, ask what the replay already does to make Et1 forward, and copy it. Every
replay write whose value contains Et1's GLORT `0x03ef` — 30 in total:

| page | n | table | Et1's value |
|---|---:|---|---|
| `0x00d000` | 12 | `L2L_SWEEPER` | `0x03ef3333` |
| `0x108000` | 7 | `PARSER_INIT_FIELDS` | `0x03ef0101` |
| `0x160000` | 5 | `NEXTHOP_TABLE` | `0x03ef80a2` |
| `0x034000` | 2 | `L2L_IVID1_TABLE[0x3ef]` | `0x020003ef` |
| `0x032000` | 2 | `L2L_EVID1_TABLE[0x3ef]` | `0x020043ef` |
| `0x037000` | 1 | `L2L_IVID2_TABLE[0x3ef]` | `0x000003ef` |
| `0x036000` | 1 | `L2L_EVID2_TABLE[0x3ef]` | `0x000003ef` |

That is the complete list of tables the working port's GLORT appears in — a much better target than
"decode L2AR", and it came straight out of the replay.

### The VID tables were genuinely missing

```
EVID1[0x3ef] = 0x020043ef      EVID1[0x3ed] = 0x000003ed
IVID1[0x3ef] = 0x020003ef      IVID1[0x3ed] = 0x000003ed
```

Et1 carries a `0x0200` prefix (bit 25, the `ET_IDX`/`IT_IDX` field) and `ETAG1 = 2`; our GLORT had
neither — only the identity value the replay writes across the whole table. Programmed
`EVID1[0x3ed] = 0x020043ed` and `IVID1[0x3ed] = 0x020003ed`, verified by readback.

### And still nothing egresses

Tried all three combinations — SPECIAL with DGLORT `0xff00`, SPECIAL with our own `0x03ed`, NORMAL
with `0x03ed`. Every one queues and completes; `lane1` never sets Transmitting; the test host's
`rx_packets` does not move.

### Remaining unreplicated

Two of the seven tables are still untouched for our GLORT: **`NEXTHOP_TABLE`** (5 writes, e.g.
`0x160015 = 0x03ef80a2` — the GLORT paired with a MAC, so this is the routed-next-hop binding) and
**`L2L_SWEEPER`** (12 writes, `0x03ef3333` — MAC aging). Neither is obviously required for a
special-delivery inject, which is why they were left, but "not obviously required" has been wrong
repeatedly in this session and they are the only replay-derived items left.

## Where this stands

Verified against a working reference and still not forwarding: link, SGLORT, GLORT_CAM, GLORT_RAM,
L2F destination mask, parser frame type, EVID1/IVID1/EVID2/IVID2. Seven prerequisites, each
confirmed by readback, none sufficient.

The honest conclusion has not changed and is now better supported: **CPU-inject to a port EOS never
configured needs state we cannot yet enumerate**, and the two candidates left from the replay
(NEXTHOP, SWEEPER) plus the undecoded L2AR are where it must live.


---

## ★ The positive control, which should have been run first

Injecting to **Et1** with the same tool, same boot, same moment:

```
lane0 before inject:  0x00000cc0
lane0 after inject:   0x00000ac0     <- bit 9, Transmitting, SET
lane1 (port 41):      0x000008c0     <- never sets Transmitting
```

**The inject path works.** `fm6000_txinline` reaches the wire on Et1 and does not on port 41, in
the same configuration seconds apart. That validates every negative result above: the failures are
about port 41 specifically, not about the injection mechanism, the DMA rings, or the tag.

This control was cheap and available from the beginning, and running it earlier would have saved
several rounds of "did that even do anything". A negative result is only worth the positive control
that frames it.

## Where the port-41 egress question now stands

Verified identical to a working reference and still not transmitting:

- link up, `lane1 = 0x8c0`, MAC receiving (`0xcc0` under host traffic)
- SGLORT `0x03ed` in `PARSER_INIT_FIELDS[41]`
- `GLORT_CAM[8]` → `GLORT_RAM[8]` → `DMaskBaseIdx 16` → `L2F[16].w1 = 0x200` = port 41
- `EVID1`/`IVID1`/`EVID2`/`IVID2` for our GLORT, matching Et1's pattern
- parser frame type: all three F64 types now expressible, e27 live
- tried DGLORT `0xff00`, `0x03ed`, `0xf000`; ftype normal and special; e29 shape

⚠ Also learned along the way: **`0x03ed` was never resolvable at all** — no `GLORT_CAM` entry
matches `0x03xx`, so those injections had no destination. Only `0xff00` and `0xf000` were ever
real tests.

Since the inject path demonstrably reaches the wire on one port and not the other, what remains
must be **per-port egress enablement** that Et1 has and port 41 does not — the scheduler/CM per-port
configuration, or a MOD egress-side per-port entry. Neither is in the GLORT-referencing set the
replay writes, which is why copying that set was not enough.

The two replay-derived tables still unreplicated (`NEXTHOP_TABLE`, `L2L_SWEEPER`) remain worth
doing, but the control makes a per-port egress enable the stronger hypothesis: a destination mask
that names a port the egress pipeline has not been told to serve is exactly what "queued, completed,
never transmitted" looks like.


## Systematic per-port sweep — what is and is not different

With the positive control established, the question narrowed to "what per-port state does Et1 have
that port 41 lacks". The header lists every per-port table (`ENTRIES = 76`), so this is a bounded
sweep rather than a hunt. Reading index 40 against index 41 on the live chip:

**Identical** (not the blocker): `ESCHED_CFG_1/2/3` (all `0x00ffffff`, and identical for port 20
too), `ESCHED_DRR_CFG`, `LAG_PORT_TABLE`, `CM_BSG_MAP`, `CM_TC_PC_MAP`, `CM_PC_RXMP_MAP`,
`CM_PAUSE_CFG`, `CM_ESCHED_STATE`, `ERL_CFG_IFG`, `MOD_MAP_DATA_W16E/W12A/W8A`, `MOD_TX_PORT_TAG`,
`MOD_DST_PORT_TAG` (`0x130`), `MOD_MIN_LENGTH` (`0x40`), `STATS_AR_TX_PORT_MAP`.

That rules out the egress scheduler and MOD's per-port configuration outright — both were strong
candidates for "queued, completed, never transmitted".

**Differed:**

```
LBS_CAM                  p40 0x0001fffe   p41 0x0003fffc
MAPPER_SRC_PORT_TABLE.w1 p40 0x00000049   p41 0x00000021
MCAST_LOOPBACK_SUPPRESS  p40 0xffff0001   p41 0xffff0003
SAF_MATRIX               p40 w0 0x00000007 w1 0x00000000
                         p41 w0 0x0010000f w1 0x00000100
```

Copied port 40's `SAF_MATRIX`, `MCAST_LOOPBACK_SUPPRESS` and `LBS_CAM` onto port 41, verified by
readback, and injected again: **still no Transmitting bit, still nothing at the test host.**

`MAPPER_SRC_PORT_TABLE` was deliberately left — it is ingress-side (source-port classification) and
cannot explain an egress failure, and it is checklist item B3 in its own right.

## Honest status

The elimination is now broad and the control is solid: injection reaches the wire on Et1 and not on
port 41, with link up, destination mask naming port 41, VID tables, GLORT resolution, frame type,
scheduler, and MOD per-port state all verified equal or correct.

What has *not* been examined is the one block this project has never decoded — **L2AR** — and the
two replay tables still unreplicated (`NEXTHOP_TABLE`, `L2L_SWEEPER`). Everything cheap and
enumerable has been checked. That is a reasonable place to stop and take A2 seriously, rather than
keep sampling registers.


---

## ★ Why none of it worked: GLORT is the wrong mechanism for an access port

Asked the question that should have come first — **what DGLORT does EOS itself use to reach Et3?**
— by walking every `GLORT_CAM` entry on a live EOS, following `GLORT_RAM[i].DMaskBaseIdx` into
`L2F_TABLE_256`, and checking which masks name port 41. Both plausible indexings of
`DMaskBaseIdx` were tried (`i1*4+i0` and `i1=flat`).

```
CAM entries whose mask includes port 41: 0
```

**None.** On a system where Et3 is `connected` and working, there is no GLORT that resolves to
port 41.

So EOS does not reach Et3 via a GLORT, and neither can we. `Et3` is a **switched VLAN-1 access
port**; frames reach it through the L2 destination lookup or a VLAN flood. The
`GLORT_CAM → GLORT_RAM → L2F` chain is the path for **routed and CPU-directed** traffic — which is
exactly why Et1 (`routed`, GLORT `0x03ef`) works through it and port 41 never will.

That single fact explains the whole sequence of failures above. Every prerequisite we satisfied —
SGLORT, GLORT_CAM entry, `DMaskBaseIdx`, `L2F` mask naming port 41, VID tables, frame type, SAF,
LBS, loopback-suppress — was correct *for a mechanism that does not apply here*. The masks EOS has
containing port 41 (`{0,41}` and `{0,3,20,40,41}`) are reached by the L2 lookup, not by any DGLORT.

### What to do instead

Two mechanisms actually deliver to a switched port, and both are testable:

1. **Flood.** Inject a broadcast frame into the VLAN and let it flood to every member port.
   `fm6000_txinline` already supports this — argv[6] is a `bcast` flag — so it is a one-command
   experiment, and the L2F masks EOS holds are exactly flood masks.
2. **L2 lookup hit.** Install a MAC→port-41 entry in the L2 table and inject a frame addressed to
   it.

Both are ordinary switching, and neither needs the GLORT machinery this section spent its time on.

### The lesson

The positive control proved injection reached the wire on Et1 and not port 41, and I read that as
"port 41 is missing configuration". The other reading — *Et1 and port 41 are reached by different
mechanisms* — was never tested, and it was the true one. A control that distinguishes two ports
does not tell you they differ only in degree.


---

## Broadcast inject does not flood — tested, with the control in the same run

The cheap test the mechanism argument implied: inject a broadcast and let the VLAN flood deliver
it. Port 3 enabled, and port 41 added to the three L2F flood masks EOS uses (full-entry writes,
verified by readback).

```
frame: ff ff ff ff ff ff | 44 4c a8 31 5d ab | 01 00 f0 00 ff 00 00 00 | 08 00

A  bcast, DGLORT 0xf000   lane0 0xcc0 (no TX)   lane1 0x8c0 (no TX)
B  bcast, DGLORT 0x03ef   lane0 0xac0 TRANSMIT  lane1 0x8c0 (no TX)
C  bcast, DGLORT 0x0000   lane0 0x8c0 (no TX)   lane1 0x8c0 (no TX)
```

**B is the control and it passes** — the same broadcast frame egresses Et1 when the tag names Et1.
So the inject path works, the frame is well formed, and the DMAC is genuinely broadcast.

**A CPU-injected frame is delivered by its DGLORT, not by a MAC lookup.** A broadcast destination
MAC changes nothing; the tag decides. That is consistent with the ISL tag's purpose — it exists
precisely to bypass the lookup — and it disproves the flood hypothesis for injected traffic.

Case C is the `e29` shape (DGLORT low 12 bits zero) and produces no egress anywhere, so "no
destination in the tag" does not fall back to a lookup either.

### What that leaves

Flooding to port 41 would have to be triggered by a frame that goes through the **L2 lookup**, and
a CPU-injected tagged frame never does. The remaining route to observing egress on port 3 is
therefore traffic that *ingresses from the wire* — i.e. a frame arriving on Et1 whose destination
resolves to port 41 — which needs the MAC table programmed, not the inject path.

That is ordinary L2 switching and it is the same A2/L2AR territory this document keeps arriving at,
but from a clearer direction: what is needed is a MAC→port-41 entry, not more GLORT plumbing.


## MAC-table route: structure known, entry placement blocked

`FM6000_L2L_MAC_TABLE` (`0x280000`, 4096 buckets x 16 ways x 4 words):

```
MAC[47:0]  FID1[59:48]  FID2[71:60]  Prec[73:72]  GLORT[95:80]  TAG[107:96]  DATA[115:108]
```

So the L2 lookup's destination **is a GLORT** — the lookup feeds back into the same
`GLORT_CAM -> L2F` chain, just reached from the wire instead of from the CPU. That is coherent with
everything above and means a MAC entry pointing at a GLORT that resolves to port 41 would work.

Two obstacles, both practical rather than conceptual:

1. **Placement needs the hash.** Entries are hash-bucketed; writing one requires the L2 hash
   function (`HASH_BASE 0xb000`, `fm6000_hashinit`), which is not decoded.
2. **Learning would place it for us, but port 41 cannot learn.** Learning requires the port to be
   in a VLAN with learning enabled — the forwarding-domain membership this whole document is about.
   Scanning the first 4 buckets after sustained host traffic found **no entries at all**, and
   `fmdump` cannot sparsely scan the remaining 4,092 (stride `0x4000`, 64 words used per bucket).

So the MAC route is circular with the original problem: it needs the VLAN membership that would
also have made flooding work.
