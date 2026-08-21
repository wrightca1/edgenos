# L3AR: the verified address map, and why the old decode was incomplete

**2026-08-20.** Work toward authoring L3AR slices 1-4 (`docs/BLOB-REMOVAL-PLAN.md`, Goal A).
The headline is not a generator -- it is that the premise the previous decode rested on was
wrong, and the map that replaces it is now verified.

## ⚠⚠ RETRACTION: "the action is a flag rewrite, and that is all it is"

`l3ar_decode.py` said this in its docstring, and every plan built on it. It is **false**.
It survived because the decoder only ever read two of the **five** RAM banks.

Datasheet Table 5-31 lists, besides `SetFlags`: `SetAlu13CmdProfile`, `SetALU46CmdProfile`,
`SetL2LookupCmdProfile`, `SetDestMaskCmdProfile`, `SetTrapHeaderCmd`, `SetHashKeyProfile`,
and 21 `MuxOutput_*` actions -- 6 sequential and 21 output mux actions in total (5.10.1).
Their operands live in RAM3/RAM4/RAM5 and index 19 separate profile tables.

**Slice 1 is the proof.** All 32 of its rules carry `Mask=0x3ffffff, Value=0`. Datasheet
5.10.5 gives the operation exactly:

    ACTION_FLAGS' = ACTION_FLAGS & Mask | Value

so that is a **no-op**, not a clear. By the old reading slice 1 did nothing at all. Yet every
one of its 32 rules carries a nonzero RAM5 word. Anything authored from RAM1/RAM2 alone would
have silently dropped every action in the stage.

## ⚠ Provenance correction

The docstring credited the geometry to "the register header". The header does **not** define
L3AR -- `EOS-SOURCES.md:77` already recorded that the `0x10000` macros are absent from it,
which is why the block was originally reverse-engineered by clustering writes.

The map below comes from a register descriptor table in `libFocalpointSDK.so` (`.rodata`,
56-byte stride, entries shaped `{name_ptr, ..., base_addr, 0, words_per_entry, 0, 0}`). Facts
about the silicon, in our own words; nothing copied. The SDK is the authority for what the
header omits (`EOS-SOURCES.md:273`).

## The map

| register | base | words/entry | notes |
|---|---|---|---|
| `CAM` | `0x10000` | 4 | x4 segments = the 0x10 rule stride, 256-bit key |
| `SLICE_CFG` / `KEY_CFG` / `ACTION_CFG` | `0x11000` / `0x11001` / `0x11008` | 1 | |
| `RAM1` | `0x11200` | 2 | SetFlags LO mask/value |
| `RAM2` | `0x11400` | 2 | SetFlags HI mask/value |
| **`RAM3`** | **`0x11600`** | **1** | **slice stride 0x20, not 0x40** |
| `RAM4` | `0x11800` | 2 | |
| `RAM5` | `0x11a00` | 2 | |
| 19 profile tables | `0x11c00`-`0x120df` | 1-4 | 32 profiles each |
| `TRAP_HEADER_RULE` / `_DATA` | `0x12100` / `0x12120` | 1 / 11 | |
| `IP` / `IM` | `0x12140` / `0x12148` | 1 | |

Profile tables, in address order: `DGLORT`, `SGLORT`, `W8ABCD`, `W8E`, `W8F`, `MA1_MAC`,
`MA2_MAC`, `VID`, `MA_FID`, `CSGLORT`, `W16ABC`, `W16DEF`, `W16GH`, `HASH_ROT`, `ALU13_OP`,
`ALU46_OP`, `POL1_IDX`, `POL2_IDX`, `POL3_IDX`, `QOS`.

**RAM3 being 1 word wide is load-bearing.** Read at the other banks' 0x40 stride it silently
returns slices 2-3's content under slice 1's label -- which is exactly what happened here
before the widths were checked. Two independent confirmations that 1 word is right: RAM3
slice 4 stops after **25** words, matching the 25-rule slice in
`fm6000MicrocodeRuleNames.txt` (32/32/32/32/25 = 153 of the datasheet's 160); and the profile
tables' declared widths tile their address range exactly (`DGLORT` 0x11c00 + 32x2 words ends
where `SGLORT` begins at 0x11c40; `W8ABCD` 3 words x 32 ends at `W8E`'s 0x11d00).

## What EOS actually puts in each slice

Nonzero rules per bank, from the executed replay via `l3ar_decode.py --actions`.

⚠ The slice-4 row and its description below were **wrong until alpha34**: `--actions` read only
word 0 of RAM4, hiding `MuxOutput_QOS` (bit 49) and `QOS_PROFILE` (50-54) entirely, and
truncating `ALU46_OP_PROFILE` (29-33). Slice 4 showed as "RAM4 13/25, ALU46 only" when it is
really QoS classification across all 25 rules. Slices 2 and 3 were read with the same defect,
so **their rows below may also understate RAM4 and RAM5** -- re-check before authoring them.

| slice | rules | distinct keys | RAM1/2 flags | RAM3 | RAM4 | RAM5 |
|---|---|---|---|---|---|---|
| 0 | 32 | 32 | 14 rewrites | 32 | 32 | 32 |
| 1 | 32 | **7** | no-op x32 | 0 | 0 | 32 |
| 2 | 32 | 32 | 11 rewrites | 28 | 6 | 15 |
| 3 | 32 | 32 | 6 rewrites | 11 | 32 | 0 |
| 4 | 25 | 25 | 1 rewrite | 0 | **25** | 0 |

**Slice 1 holds about 7 real rules, not 32.** Its 32 rules resolve to only 7 distinct keys;
the datasheet calls the 32 rules of a slice one *precedence set* ("Number of rules per
precedence set: 32", 5.10.1), so exactly one rule wins and duplicate-key rules can never
fire. The remaining 25 slots are filled but unreachable. Consistent with that, slice 1's RAM5
words are `(k<<12)|0x040` where `k>>1` runs over `0..31` exactly once -- a permutation with a
valid bit, the signature of a table-init loop rather than hand-authored rules.

## The action field layout — established

The SDK carries a **field** descriptor table beside the register one: 12-byte stride,
`{name_ptr, bit_offset, width}`. It gives the operands of the 6 sequential and 21 output mux
actions:

| bank | width | fields |
|---|---|---|
| RAM3 | bits 0-31 | `SetTrapHeaderCmd`, `TRAP_HEADER_ENABLE/IDX`, `SetL2LookupCmdProfile`, `L2L_CMD_PROFILE`, `MuxOutput_MA1_MAC/MA2_MAC/VID/MA_FID` + their profiles |
| RAM4 | bits 0-54 | `SetHashProfile`, `HASH_PROFILE`, `HASH_ROT`, `SetAlu13/46CmdProfile`, `ALU13/46_CMD_PROFILE`, `MuxOutput_ALU13/46_OP`, `POL1/2/3_IDX`, `QOS` |
| RAM5 | bits 0-58 | `MuxOutput_DGLORT/SGLORT/CSGLORT`, `SetDestMaskCmdProfile`, `DMASK_CMD_PROFILE`, `MuxOutput_W8ABCD/W8E/W8F/W16ABC/W16DEF/W16GH` + their profiles |

**Each list tiles its register exactly** — RAM3 covers 0-31, RAM4 covers 0-54, RAM5 covers
0-58, with no gap and no overlap. That is the check that the grouping is right: a wrong
assignment does not tile. Two RAM5 fields were *predicted from gaps* in the tiling and then
confirmed against the table at exactly the predicted positions (`SetDestMaskCmdProfile` bit
18 w1, `W8ABCD_PROFILE` bit 24 w5). Widths independently match the datasheet's stated operand
sizes (`ALU13_CMD_PROFILE` 5 bits, `L2L_CMD_PROFILE` 4, `DMASK_CMD_PROFILE` 4, every
MuxOutput profile 5).

The descriptor entries also state the **strides** directly: RAM3 `0x20`, RAM5 `0x40`,
CAM rule `0x10` / slice `0x200` — confirming the RAM3 correction above from the SDK rather
than from inference.

## What each slice does

Decoded from the executed replay with `l3ar_decode.py --actions`:

| slice | function |
|---|---|
| 0 | **forwarding** — L2 lookup key construction (`L2L_CMD_PROFILE`, MA1/MA2_MAC, VID, MA_FID), `ALU46`, and DGLORT / DMASK / W8ABCD / W8E muxing. All 32 rules populate all three banks. |
| 1 | **SGLORT + csGlort assignment** — every rule is `MuxOutput_SGLORT` + `MuxOutput_CSGLORT` with a distinct `CSGLORT_PROFILE`. Nothing else: no flag rewrite, no RAM3, no RAM4. |
| 2 | **VID assignment and trap** — `MuxOutput_VID` across most rules, `SetTrapHeaderCmd` on rules 2-3, some L2 lookup and csGlort. |
| 3 | **the ALUs** — `SetAlu13CmdProfile`/`SetAlu46CmdProfile` with operand profiles on all 32 rules; RAM5 entirely unused. |
| 4 | **QoS classification** — every rule but 0 sets `MuxOutput_QOS` with a distinct profile, mapping the FFU's `W8B`/`W8C` nibbles onto QoS. The 13 rules carrying the port qualifier additionally select an `ALU46` operand. |

⚠ **This makes the old "csGlort assignment, policers, storm control and L3 QoS" line partly
right for the wrong reason.** Slice 1 *is* csGlort assignment -- that is now established by
decoding it, not asserted. But it was uncited when acted on, the other three functions are
not what slices 2-4 do (VID/trap, ALUs, ALU46), and the policer and QoS profile fields live
in RAM4 spread across slices rather than in a slice of their own. alpha30 deleted all four.

⚠ **A retraction of something written earlier the same day.** This document first read slice
1's RAM5 words as "a permutation with a valid bit, the signature of a table-init loop, not of
hand-authored rules". That was wrong. Decoded with the real field layout the "valid bit" is
`MuxOutput_CSGLORT` and the "permutation" is `CSGLORT_PROFILE` taking each of its 32 values
once -- 32 genuine rules. Corroborated by `CSGLORT_PROFILE_TABLE` being the one profile table
written completely (64/64 words). The lesson is the same one as the flag-rewrite retraction:
a pattern read off raw words, without the field layout, invites a story.

## The slice-1 question — resolved

Earlier this document flagged as unresolved that slice 1's rules 2-27 share one key while all
32 decode as live, so at most one of 26 can fire. **Decoding `CSGLORT_PROFILE_TABLE` settles
it: it does not matter which one fires.**

All 32 csGlort profiles are near-identical -- `Value=0x0000, Mask=0xffff, Select=0`, a
pass-through. Exactly five differ: profile 5 (`Select=2`) and profiles 10, 15, 22, 28
(`Mask=0xffe0`, clearing the low 5 bits). And the five slice-1 rules that have *distinct keys*
are precisely the five that point at those profiles:

| rule | key | CSGLORT_PROFILE | profile content |
|---|---|---|---|
| 1 | distinct | 5 | `Select=2` |
| 28 | distinct | 22 | `Mask=0xffe0` |
| 29 | distinct | 28 | `Mask=0xffe0` |
| 30 | distinct | 10 | `Mask=0xffe0` |
| 31 | distinct | 15 | `Mask=0xffe0` |
| 0 (universal default), 2-27 | shared | 11, 25, 8, 2, 18, ... | all pass-through |

So the 26 duplicate-key rules are functionally equivalent to each other and to the default:
whichever precedence picks, csGlort passes through unchanged. The unique profile index per
slot is a slot-numbering convention, not 26 different behaviours.

**Consequence for authoring: slice 1 needs 6 rules, not 32** -- the universal-match default
plus the five that select a functionally distinct profile.

This also retires the "init-loop fill" reading twice over. The permutation is real rule
content, *and* the rules it indexes are mostly inert -- both halves of the earlier guess were
wrong in opposite directions, which is what happens when a pattern is interpreted without the
table it indexes.

## Profile table semantics

The 19 profile tables share a shape: `Value_X`, optional `Mask_X`, and `Select_X` per channel,
applied as `X = (selected_source & Mask) | Value`. `Mask=0` therefore means a constant
assignment -- `DGLORT_PROFILE_TABLE` entry 2 is `Value=0xfffe, Mask=0`, entry 6 is
`Value=0xffff, Mask=0`, the reserved flood/drop GLORTs.

Every table's field extent matches the width the register descriptor declares, across all 19:
`ALU13_OP` reaches bit 119 (w=4), `QOS` bit 60 (w=2), `W8ABCD` bit 71 (w=3), `POL1_IDX` bit 15
(w=1), `CSGLORT` bit 34 (w=2). Nineteen independent agreements, no exceptions -- which is the
evidence that the field groups are matched to the right registers.

The same field table also covers L2AR's and MOD's action profile tables
(`MOD_DATA_W8{A..E}`, `MA_WRITEBACK_*`, `RX_STATS_IDX*`, `L2_VID*`, `DMASK_IDX`), so this is
not an L3AR-only result.

## Select_X source encodings — from the datasheet

Datasheet Table 5-37 and 5.10.7 give the mux fan-in for each output channel, which is what
`Select_X` indexes:

| Src# | DGLORT | SGLORT | CSGLORT (2-bit) |
|---|---|---|---|
| 0 | `ISL_DGLORT` (post-assoc) | `ISL_SGLORT` | `ISL_SGLORT` (canonical) |
| 1 | `FFU_DATA.W24[15:0]` | `FFU_DATA.W24[15:0]` | `L3_HASH` (SMAC + Hash(SIP) security) |
| 2 | `FFU_DATA.W16A` | `FFU_DATA.W16A` | `SGLORT` (output of the SGLORT transform) |
| 3 | `FFU_DATA.W16B` | `FFU_DATA.W16B` | |
| 4 | `NEXTHOP_DATA.W16A` | `NEXTHOP_DATA.W16D` | |
| 5 | `NEXTHOP_DATA.W16D` | | |

EOS's `DGLORT_PROFILE_TABLE` reads straight off this: profile 1 = `NEXTHOP_DATA.W16D` (the
routed next-hop), profile 3 = `NEXTHOP_DATA.W16A`, profile 5 = `FFU_DATA.W24`, and profiles
2/4/6/7 are `Mask=0` constants `0xfffe`/`0xfffb`/`0xffff`/`0xfffc` -- the reserved
flood/drop GLORTs.

### What slice 1 is, in words

`CSGLORT` is a **12-bit** "canonical SGLORT" field. Slice 1 derives it:

- **rules 0, 2-27** -- `Select=0, Mask=0xffff`: canonical SGLORT = `ISL_SGLORT` unchanged.
- **rules 28-31** -- `Select=0, Mask=0xffe0`: clears the low 5 bits, collapsing up to 32
  GLORTs onto one. That is **LAG canonicalisation** -- identify the logical source (the
  aggregate) rather than the physical member port, which is what a canonical source GLORT is
  for (source suppression on a LAG).
- **rule 1** -- `CSGLORT_PROFILE=5, Select=2`: take csGlort from the SGLORT transform's
  output, with `SGLORT_PROFILE=1` = `FFU_DATA.W16A` masked to 12 bits.

**The LAG reading is confirmed from the keys.** Rules 28-31 match four *disjoint* GLORT
blocks, each canonicalised by clearing the low 5 bits (32 members per aggregate):

| rule | match | range | block size |
|---|---|---|---|
| 31 | `ISL_SGLORT=0x1000/0xf800` | 0x1000-0x17ff | 2048 |
| 30 | `ISL_SGLORT=0x1800/0xfc00` | 0x1800-0x1bff | 1024 |
| 29 | `ISL_SGLORT=0x1c00/0xff00` | 0x1c00-0x1cff | 256 |
| 28 | `ISL_SGLORT=0x1d00/0xffc0` | 0x1d00-0x1d3f | 64 |

Rules 2-27 match `ISL_SGLORT=0x0` exactly, and pass through -- the same result as the default.

### Precedence: higher index wins

Rule 0 is a universal match. If the *lowest* index won, it would shadow rules 28-31 and
canonicalisation could never happen, so **higher index takes precedence and rule 0 is the
default**. This is inference from the table's structure rather than a documented statement,
but it is forced: no other ordering lets EOS's own slice 1 do anything. It is also
untestable from rules 28-31 alone, since those four are disjoint and never compete.

## Confirmed on live silicon

The map and field layout came from a stripped binary and a replay file, so they were read back
off the running 7150 (`10.1.1.77`, via the serial console):

| word | silicon | decoded with our layout |
|---|---|---|
| `0x11a7e` RAM5 slice1 rule31 | `0x0001F040` | `MuxOutput_SGLORT, MuxOutput_CSGLORT, CSGLORT_PROFILE=15` |
| `0x11e4a` CSGLORT[5] | `0xFFFF0000 0x00000002` | Value=0, Mask=0xffff, **Select=2** |
| `0x11e54` CSGLORT[10] | `0xFFE00000 0x00000000` | Value=0, **Mask=0xffe0**, Select=0 |
| `0x11c42` SGLORT[1] | `0x0FFF0000 0x00000002` | Value=0, Mask=0x0fff, Select=2 |

Every field lands where the SDK's table says it does, on real hardware. `gen_l3ar_slice1.py`'s
profile-table writes are byte-identical to these.

## Authoring: slice 1 is done

`asic/fm6000/tools/gen_l3ar_slice1.py` emits the whole of slice 1 -- **810 writes replacing
1,088 replay lines**, in **6 rules instead of 32**, plus one csGlort profile where EOS used
four identical ones. Its `--verify` compares *behaviours* rather than bytes (deliberately: the
rule count and profile numbering differ on purpose) and finds the same three csGlort
behaviours EOS produces.

⚠ **A byte-comparison caught a bug the functional verify could not.** The first version put
Key in CAM words 0-1 and KeyInvert in words 2-3. It is the other way round. `--verify` passed
anyway, because it only ever compares profile-table contents and never looks at the keys. What
exposed it was diffing our authored rules against EOS's own rules 28-31, whose keys we also
author: every rule differed by exactly a word 0<->2 swap. With the order fixed, all six live
rules are byte-identical to EOS's CAM words, and the decoder reads our emitted image back as
the intended 6 rules with the 26 never-match slots correctly seen as dead.

**Hardware-tested: alpha32, 3 of 3 boots identical.** Wired into `fm6000-fullseq.sh`, built,
and booted. Both ports clean-locked (`0940`, HiBer clear, `pcsRx=1`, retrain attempts 0),
executed writes 130,966 -> 130,688 (exactly the predicted -278), provenance 82% -> 83%. After
`edgenos-up.sh`: OSPF Full adjacency, 39 kernel routes, 14 routes programmed into silicon.
See `build/arista-7150/m1/RELEASE-0.3.0-alpha32.md`.

End-to-end transit **passes**: frames in on et2, captured out on et1 with the SMAC rewritten to
the router MAC, the DMAC to the next-hop, and TTL 64 -> 63 -- 5 of 5. The TAP counters show
et2 rx=0 throughout, which is positive evidence the forwarding was silicon and not the kernel
(TAP counters only count punted packets). `tools/eg.sh` and `tools/p5.sh`, which the harness
needs, were missing from the tree and have been recreated.

## ⚠ What is still not established

Addresses, widths, field layout, profile-table layout and the `Select_X` source encodings are
all established. What remains for slice 1 is confirming the CAM keys of rules 28-31 select
LAG source ports, as the section above flags. For the other slices, the multi-channel profile
tables (VID, MA_FID, W8ABCD, W16*, ALU13/46_OP, QOS) have their layouts recorded here but are
not yet wired into the decoder.

Authoring rules from a guessed layout is precisely the alpha30 failure: that change rested on
an uncited premise about what slices 1-4 contained, contradicted the datasheet, and killed the
dataplane. So `--actions` prints **words, not field names** -- rendering invented field names
over unverified bits is how the flag-rewrite-only claim survived as long as it did.

Also still true and still binding: the slices are serial application stages, so a slice we do
not author must have its never-match keys written explicitly (`Key=0, KeyInvert=0`) rather
than its writes omitted.

## Next

1. Decode the CAM keys of slice 1 rules 28-31 to confirm the LAG reading.
2. Author slice 1: 6 rules, one action word each, no flag rewrite, no RAM3, no RAM4. It is by
   some margin the cheapest slice, and it is now fully understood except for `Select=2` on
   rule 1.
3. ~~Then slice 4~~ — **DONE, alpha34**: authored, byte-verified 625/625, transit passes.
   Note it is *not* "ALU46 only, 13 live rules" as this document first said: RAM4's second word
   was never decoded, and slice 4 is really **QoS classification** across all 25 rules, with
   ALU46 operand selection on the 13 qualified ones.
4. **Slice 3 DONE, alpha35** — 725/725 live words identical, 3 dead rules (23, 24, 30) confirmed
   never-match on both sides. Note it also **rewrites flags** (six combinations), which the
   "6 rewrites" row above understated as incidental.
5. **Slice 2 DONE, alpha36** — 725/725 live words, 3 dead rules, twelve flag combinations.

★ **L3AR IS COMPLETE AS OF alpha38** — slices 1-4 (alpha32/36/35/34), plus slice 0's RAM3/4/5
and the 19 shared profile tables (alpha38, 450 writes). `fm6000_l3arinit` emitted slice 0's CAM
and RAM1/RAM2 only, a direct consequence of the retracted "flag rewrite and that is all it is"
premise; its missing 160 addresses of forwarding actions came from EOS's replay until alpha38.

★ **THE WHOLE L3AR BLOCK IS NOW AUTHORED.** All five slices are generated from our own source:
slice 0 by `fm6000_l3arinit`, and slices 1-4 by `gen_l3ar_slice{1,2,3,4}.py` in alpha32/36/35/34.
Provenance across the four went 107,828 (82.3%) to 110,863 (84.8%) of executed writes, with the
dataplane verified by the transit test on each.
