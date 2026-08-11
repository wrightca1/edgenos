# Diffing the live chip: EOS vs EdgeNOS

**2026-08-11.** After a long unsuccessful hunt for why EdgeNOS stopped forwarding, the productive
move was to stop reasoning from the replay and **compare the running chip under EOS (which
forwards) against the running chip under EdgeNOS (which does not)**. EOS is a known-good
configuration of this exact hardware, so whatever differs is the candidate set — and nothing else
is.

This is the same differential-oracle method that settled the parser and L3AR work, applied to
whole register regions instead of single tables.

## Method

`fmdump <start_hex> <count>` over two regions, taken under each OS and diffed offline:

```
/mnt/flash/fmdump 110000 65536     # CM  0x110000-0x11ffff
/mnt/flash/fmdump   e0000 16384    # EPL 0x0e0000-0x0e3fff
```

81,920 words each. Staging `fmdump` on `/mnt/flash` matters — the M1 rootfs is in RAM, so anything
in `/tmp` is gone after the reboot between the two captures.

⚠ **The EdgeNOS capture is not a pristine boot.** It was taken after I had re-run the full replay
*and* `fm6000_cmfill`, so some EdgeNOS-side values are mine, not the boot's — `0x112800` in
particular reads the raw fill `0xffffffff` where the replay had correctly written `0x0013003a`,
because my `cmfill` run overwrote it. Treat CM watermark differences with that in mind; the EPL
differences are unaffected.

## Result 1 — it killed my leading hypothesis immediately

I had spent considerable effort on CM `0x114000`, which the replay writes (`0x000000d6`) and the
chip reads back as zero, and which `fm6000_cmfill` fails to write.

```
0x114000: EOS=0x00000000   EdgeNOS=0x00000000
0x113810: EOS=0x00000000   EdgeNOS=0x00000000
```

**Zero on EOS too.** That region is zero on a system that forwards perfectly, so it never mattered.
An hour of "the replay can't program this memory" was real but irrelevant. One diff against a
working reference would have said so at the start.

## Result 2 — 1,746 words of uninitialised memory

Classifying all 4,373 differing words:

| signature | count | meaning |
|---|---:|---|
| EOS zero, EdgeNOS non-zero | 1,746 | **uninitialised on EdgeNOS** — this is the "no CRM fill" gap |
| EOS non-zero, EdgeNOS zero | 12 | mostly FIFO *status*, see below |
| both non-zero, differing | 2,615 | real configuration differences |

The uninitialised runs hold random values (`0xdbd15510`, `0x2761cffa`, `0x5e8cccfd`) where EOS
holds zeros — `0x111780`-`0x111917`, `0x112942`-`0x112afb` (442 words), `0x112b42`-`0x112b8b`.
That is exactly what `COLD90 PROBE MODE ... no CRM fill` means, now measured rather than inferred.

⚠ **Do not chase the 12 "missing" words.** Seven are `0xe3b08`-`0xe3b0e`, which the header names
`EPL_TX_FIFO_B/C/D_PTR_STATUS` and `EPL_RX_FIFO_*_PTR_STATUS` — read-only status. EOS's are
non-zero because traffic was flowing and EdgeNOS's are zero because it was not. That is a symptom
of the fault, not its cause, and it looks like a smoking gun until you read the register names.

## Result 3 — the EPL14 configuration deltas

EPL14 is Et1. Stripping status registers, the genuine configuration differences are small and
specific:

| register | EOS | EdgeNOS | delta |
|---|---|---|---|
| `0xe3b01` `EPL_CFG_A` | `0x7e1d7899` | `0x7e0d7899` | bit 20 |
| `0xe3b02` `EPL_CFG_B` | `0x00090033` | `0x00090003` | bits 4,5 |
| `0xe3b00` `EPL_IP` | `0x00000000` | `0x00000010` | bit 4 |
| `0xe3804` | `0x00002a00` | `0x00003b87` | lane-0 block |
| `0xe3821` | `0x00402003` | `0xffc0c79a` | lane-0 block |

### …and reading the field names killed that lead too, productively

Before writing anything, look up what those bits are:

```
EPL_CFG_A bit 20      = Active_1        (EPL port 1 active)
EPL_CFG_B bits 4-7    = Port1PcsSel     (PCS select for EPL port 1)
EPL_IP                = interrupt pending — status, not config
```

EPL "port 1" is **lane 1**, which `fm6000_serdes_ports.h` maps to front-panel **port 3**. EOS has
those bits set because the test host is plugged in; EdgeNOS does not because it never brought that
lane up.

`Active_0` and `Port0PcsSel` — the bits that govern **Et1**, lane 0 — are **identical on both
sides.** So Et1's entire EPL configuration matches a working EOS system, and the forwarding fault
is not in the EPL.

The same check disposed of the eight lane-0 block differences: the header names them `LINK_IP`,
`RX_SEQUENCE`, `MAC_CODE_ERROR_COUNTER`, `MAC_LINK_COUNTER`, `DISPARITY_ERROR_8B10B`,
`SERDES_STATUS`, `SERDES_IP`, `LANE_DEBUG` — **every one a counter or status register.** Writing
EOS's values into them would have "tested" nothing while looking like an experiment.

★ **But the lead converts into the port-3 recipe.** Bringing up front-panel port 3 needs exactly:

```
EPL_CFG_A (0xe3b01)  |= 1<<20        Active_1
EPL_CFG_B (0xe3b02)  Port1PcsSel = 0x3   (bits 4-7)
```

derived from a configuration that demonstrably works, rather than from `fm6000_serdes_enable`
guesswork. That is what A4 and B1 need for transit traffic.

### So where is the Et1 fault? — and it lands on a wall already mapped

By elimination the EPL is clean, the forwarding tables are 25/25 correct, and CM `0x114000` is zero
on both. What remained was the **1,746 words of uninitialised memory**. Tested it: generated a
715-word zero-fill from the EOS reference, applied it under EdgeNOS, and the writes **did not
stick** —

```
sample before: 0x111780=0xdb9954f2
fmload: 715 writes
sample after : 0x111780=0xdb9954f2      <- unchanged
```

★ **This is not a new problem. It is phase 76, rediscovered from the other end.**
`arista-reverse-engineering/notes/analysis/phase76-cold75-crm-fill-still-offbuses-writability-is-boot-state.md`
established it by disassembly:

> bank writability is a boot-state, not the descriptor/clock … On the golden/EOS chip the CRM fill
> WORKS. Our cold path never reaches that state. The difference is the part of `fm6000BootSwitch`
> BEFORE the fills that makes the banked memory writable.

That note also records that the CRM is *not* a bypass — a CRM write to a banked memory faults the
same way a direct write does — and that there is no CRM engine setup step being missed. So every
write failure hit today (`0x114000`, `0x111780`, the 715-word fill, `fm6000_cmfill`'s own readback)
is one symptom of one known cause: **the banked memories are not writable in the state our boot
leaves the chip in.**

The new information this diff adds is *which* memories are affected and by how much — 1,746 words
across `0x111780`-`0x111917`, `0x112942`-`0x112afb`, `0x112b42`-`0x112b8b` — and that EOS holds
them at zero.

### The practical consequence

Our boot **cannot warm-inherit** EOS's initialised chip, because it resets it on the way past:

```
[FM6UP] COLD87 SOFT_RESET=0x00000000 CAM0=0x37a74ed0 (MSB released; loading full microcode)
[FM6UP] COLD90 PROBE MODE: chip left ALIVE (microcode loaded, no CRM fill).
```

Rebooting EOS→EdgeNOS leaves the chip powered, so the memory subsystem *would* still be in EOS's
writable state — but `COLD87` releases SOFT_RESET itself and the fills never run. Phase 76's option
3 was "accept warm-inherit as the shipped path (it lets EOS run the full boot)", and that is
exactly the path this boot forecloses.

So the ranked options are unchanged from phase 76, and this session adds evidence for option 2:

1. Disassemble `fm6000BootSwitch` end-to-end for the memory-subsystem enable before the fills.
2. **Diff the memory-subsystem enable registers EOS-vs-cold** — which is precisely the method that
   worked here, now pointed at BM/SRBM/SBus/JSS state instead of CM/EPL.
3. Warm-inherit: skip `COLD87`'s SOFT_RESET so EdgeNOS keeps the chip state EOS established.

Option 3 is a small change to the boot path and is testable in one reboot.

## Result 4 — EOS has your test host's port up, and that is the port-3 reference

```
Et1  to-Edgecore5610-port6      connected  routed  10G  10GBASE-SR
Et2  to-Edgecore5610-port7-DAC  connected  routed  10G  10GBASE-CR
Et3                             connected  vlan 1  10G  10GBASE-SR
```

`0xe3880` — EPL14 **lane 1**, which `fm6000_serdes_ports.h` maps to front-panel port 3 — reads
`0x00000ac0` under EOS (RxLinkUp, HeartbeatOk, Transmitting, SerXmit) against `0x00000015` (three
link faults) under EdgeNOS.

So the port-3 bring-up we were about to attempt from first principles does not need to be derived
at all: **EOS's own lane-1 configuration is now captured**, and the whole lane-1 block
`0xe3880`-`0xe38ff` is in the dump. That is the reference for A4/B1's transit traffic, and it also
gives a second working example to check the lane-0 deltas against.

## What this changes

The next steps are concrete rather than speculative:

1. Boot EdgeNOS, write EOS's `EPL_CFG_A`/`EPL_CFG_B`/`EPL_IP` values for EPL14, retest Et1.
2. If that restores forwarding, apply the same to lane 1 and bring up port 3 from the captured
   config rather than from `fm6000_serdes_enable` guesswork.
3. Fill the uninitialised CM regions to zero to close the CRM gap — cheap, and now known to be a
   real difference rather than a suspicion.

**Provenance.** The dumps are register *state* read from hardware at runtime and are kept outside
the tree, like `fwd4.txt`. What is recorded here are the addresses, the register names from the
header, and the deltas — facts about the chip, not a copy of anyone's configuration.


---

# Result 5 — the actual root cause: two memfill runs were 4,096 words short

The CM/EPL diff above narrowed things but did not close them. Extending the same method to the
**forwarding tables** did, and it exposed a gap in my own earlier verification: I had checked
EdgeNOS's tables against **the replay** (25/25 correct) and never against **EOS**. Those are not
the same question — our generators can write valid-but-different values, and the replay does not
cover everything.

Dumped L3AR (`0x10000`), parser (`0x100000`) and MOD (`0x150000`), 147,456 words, under each OS:

| region | differing |
|---|---|
| L3AR | **3** of 16,384 — our generator is right |
| parser | 16,815 of 65,536 — expected, our parser is authored, not EOS's |
| MOD | 6,143 of 65,536 — **and these are not by design** |

The MOD differences are two clean runs with the uninitialised-memory signature:

```
0x153000-0x153ffe   3072 words   eos=0x00000000  edge=0xdedeed23
0x157000-0x157ffe   3071 words   eos=0x00000000  edge=0x5210d2b4
```

Neither range is touched by the replay — **0 of 4096 addresses in each appear in `fwd4.txt`** — so
nothing but a memory fill was ever going to initialise them. Every MOD region the replay *does*
cover matched EOS exactly, 0 differing words.

## The bug, in two numbers

`asic/fm6000/fm6000_memfill.c`:

```c
{0x150000, 12288, 0x00000000, "MOD"},   /* 0x150000+12288 = 0x153000 */
{0x154000, 12288, 0x00000000, "MOD"},   /* 0x154000+12288 = 0x157000 */
```

Both runs stop exactly one 4096-word bank early, leaving precisely the two ranges measured as
garbage. The file's own header note says why: *"Two doc-elided runs (#71-88 L2AR, #108-112 MOD) are
reconstructed from the doc's summary lines."* The MOD fills were reconstructed rather than read,
and the reconstruction assumed 3 banks where the hardware has 4. Fixed to `16384`, which covers
`0x150000-0x153fff` and `0x154000-0x157fff` exactly.

## Why this explains everything we saw

MOD is the **egress** modifier, and the fault was egress-only:

- frames left the TAP correctly formed, with the right MACs and IPs
- `CM_PORT_TX_DROP_COUNT` stayed 0 — nothing discarded them
- ingress was perfect: the peer's OSPF hellos and ND arrived normally
- the peer never answered, because what reached the wire had been mangled by a modifier running on
  uninitialised SRAM

It also explains why ARP resolved *once*, immediately after the MAC was corrected, and then lapsed:
whatever garbage MOD held happened to leave that one frame intact.

## Tested on hardware: the fix is correct, and it did NOT restore forwarding

Rebuilt the SWI with the corrected `fm6000_memfill` (unzip → gunzip/cpio the initrd → swap
`usr/bin/fm6000_memfill` → repack, `zip -X -0`; EOS has zip/unzip/cpio/gzip so it can be done on
the box) and booted it as `edgenos-memfix.swi`.

Every predicted effect landed:

| check | before | after |
|---|---|---|
| memfill total | 1,138,476 words | **1,146,668** — exactly +8,192 |
| `0x153000`, `0x157000` | garbage | **`0x00000000`** |
| reading MOD | **off-bused the chip** | safe, `PIN=0x208` throughout |
| MOD vs EOS | 6,143 differing | **0 differing of 65,536** |

MOD is now byte-identical to a working EOS system.

**And Et1 still does not forward — 100% loss, ARP FAILED.**

So the memfill gap was a real defect, is fixed, and was not the cause of the presenting symptom.
Worth keeping separate: the fix is validated by measurement (the fill count, the zeros, the
byte-exact match, the vanished off-bus hazard), not by the thing we were hoping it would cure.

★ It also explains the off-bus hazard mechanically: uninitialised SRAM carries invalid ECC, so
*reading* it faults the chip. That is why `memfill` is the "CRM **ECC**-fill", why the boot log
says `SKIP MOD 0x150000-0x15ffff = the off-bus block`, and why a single `fm6000reg 0x153000`
wedged the box earlier in this session. With the region filled, the same read is safe.

## Still open

Regions not yet diffed against EOS: **L2AR** (`0x120000`) and **FFU** (`0x300000`). The parser
differs 25.7% but that is expected — ours is authored, not EOS's. The CM uninitialised regions
remain, and still reject writes (phase 76's wall).

The next move is the same method one more time: dump L2AR and FFU under both and diff.
