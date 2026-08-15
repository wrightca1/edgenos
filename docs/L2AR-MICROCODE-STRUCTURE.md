# L2AR microcode: geometry solved, encoding partly open

Groundwork for regenerating the parser/L2AR/MOD microcode from semantics instead of shipping
Arista's tables. See `EOS-SOURCES.md` for where the inputs come from and `PROVENANCE.md` §2.5 for
why this matters.

## ★ The rule geometry is exact

`fm6000MicrocodeRuleNames.txt` declares **413 L2AR rules** across 8 slices
(46/24/64/54/51/64/46/64) and **153 L3AR rules** across 5 (32/32/32/32/25).

The L2AR rule region is `0x140000`–`0x143fff`, and it matches the rule count exactly:

```
413 contiguous runs, every one exactly 24 words long
stride 0x20 between runs (24 words used of a 32-word slot)
413 x 24 = 9,912 writes = 1680 + 2832 + 2760 + 2640   (pages 0x140000..0x143000)
first run 0x140000, last run 0x143fe0
```

One run per named rule, no remainder. The handful of non-`0x20` strides (`0x160`, `0x260`,
`0x520`) fall at slice boundaries.

⚠ **Pages are not slices.** Runs per page are 70/118/115/110, which matches no slice size. The
rules are laid out sequentially across the whole region and the slice boundaries fall wherever the
larger strides are. Do not assume page == slice.

The remaining L2AR pages — `0x144000` (864), `0x145000` (1110), `0x146000` (534), `0x147000` (53)
— have irregular run lengths and are **not** rules. They are L2AR configuration and need separate
treatment.

⚠⚠ **ADDRESS CORRECTION.** The claim that L3AR lives at `0x158000`–`0x159fff` is **wrong**, and
it was wrong from the first analysis. `FM6000_L3AR_BASE` is **`0x10000`**; `0x158000` is
`MOD_CAM`. So:

- the 111 mixed-length runs at `0x158000` are MOD, not L3AR — and MOD's real structure is
  32 profiles × 32 steps, which is why run-length analysis made no sense of it
- **L3AR is the `0x010000` page** that `REPLAY-TRIAGE.md` group 3 called "unnamed" and that
  `EOS-SOURCES.md` then reclassified as generic "microcode". It is 5,095 writes across
  `0x010000` (2,448), `0x011000` (1,577), `0x012000` (95), `0x014000` (12), `0x018000` (963).

Two documents carried the wrong address for months because the region was identified by clustering
writes rather than by reading `FM6000_L3AR_BASE`. Same lesson as the field layouts, at the level of
whole blocks.

## ★★ SOLVED: the geometry and the action layout came from the register header

`docs/PARSER-CONVENTIONS.md` records the same lesson twice; this is the third
time it paid. The header defines L2AR exactly as it defined `PARSER_RAM`:

```
L2AR_CAM(slice, rule, seg, word) = 0x140000 + 0x800*slice + 0x20*rule + 4*seg
    ENTRIES_2 = 8 slices, ENTRIES_1 = 64 rules, ENTRIES_0 = 6 segments, WIDTH 4
L2AR_RAM(slice, rule, word)      = 0x145400 + 0x80*slice + 2*rule
```

**A rule is 6 CAM segments × 4 words = 24 words, key 6 × 128 = 768 bits.** That
is what the 24-word runs at stride `0x20` below always were — six segments, not
one wide entry. The earlier conclusion that the encoding "resists statistical
attack" was correct and beside the point: it needed the header, not more data.

Action RAM fields, exact: `FLAGS_TAG[7:0]`, `DMT_PROFILE[12:8]`,
`TransformDestMask`, `DMT_NEXT_STAGE`, `SetCpuCode`, `SetTrapHeader`,
`SetMirror[23:20]`, and fifteen `MuxOutput_*` bits (24–38).

Validated two ways before use:

- **structural** — RAM entries come out at exactly 2× the rule count on all
  eight slices (92/48/128/108/102/128/92/128 against 46/24/64/54/51/64/46/64
  from the rule-name file). Intel's header and Arista's names agreeing.
- **semantic** — slice 0 rule 2 is named `allowAndLearnNewSmac` and its action
  decodes to `MuxOutput_MA_WRITEBACK`, MAC-table writeback. The name says learn,
  the action writes the MA table, and the two come from unrelated files.
  `denyNewSmacOnSecuredPort` likewise sets `SetTrapHeader`.

Decoder: `asic/fm6000/tools/l2ar_decode.py`. 407 of 413 rules decode; the
shortfall is rules whose key is entirely fill.

## ★★ The key layout, from the register header — authoritative

`FM6000_L2AR_CAM_KEYS` gives exact bit positions. 384 bits, exactly the 6
segments x 64 key bits a rule provides.

```
SMASK             0-75      ACTION_DATA_W8F   256-263
ACTION_FLAGS     76-151     L2_DMAC_ID3       264-268
L2F_ISTATE      152-164     L2_SMAC_ID3       269-273
DGLORT_TAG          165     L2_TYPE_ID2       274-277
MA1_TAG         166-177     POL1_TAG1_TOP     278-281
MA2_TAG         178-189     POL1_TAG2_TOP     282-285
reserved0       190-191     POL2_TAG1_TOP     286-289
L2L_ETAG1       192-203     POL2_TAG2_TOP     290-293
L2L_ETAG2       204-215     POL3_TAG_TOP      294-297
ALU13_Z         216-231     DGLORT            298-313
ALU46_Z         232-247     reserved1         314-319
ISL_USER        248-255     SGLORT            320-335
                            DROP_CODE         336-343
                            MA1_HPV           344-347
                            MA1_FID2_IVL          348
                            MA2_FID2_IVL          349
                            MA1_LOOKUP            350
                            MA2_LOOKUP            351
                            MA2_HPV           352-355
                            MA2_MPV           356-359
                            L2L_ITAG1         360-371
                            L2L_ITAG2         372-383
```

Segment *i* holds key bits `[64i .. 64i+63]`.

### This corrected both of my "measured" anchors

- **bits 0-75 are SMASK, not `DMASK_A`.** The 76-bit field was real and the
  measurement was sound; the *attribution* came from Table 5-71's ordering, and
  that ordering is wrong. Same error as before, one level down.
- **`DMASK_A` is not in this key at all.** It is matched by a separate structure,
  `FM6000_L2AR_CAM_DMASK` (KeyInvert[37:0], Key[101:64]) — which is exactly why
  the datasheet gives it bitwise-OR semantics unlike every other field. A
  generator that tried to place it in the main key would have been wrong in a way
  no amount of care-mask analysis would have revealed.

`MA2_MPV` at 356-359 is seg5 bits 36-39; the `…Learn…` rules match seg5 33-36,
i.e. the top of `MA2_HPV` plus the first bit of `MA2_MPV`. Both are learning
controls, so that is coherent.

### The rule this establishes

Five times now a datasheet table's field order has failed to predict silicon bit
order, and five times the register header has had the answer:

| | inferred | header said |
|---|---|---|
| parser action layout, attempt 1 | table order, LSB-first | refuted |
| parser action layout, attempt 2 | scan for a 6-bit field -> bit 45 | 38 |
| CAM match priority | first match wins | last match wins (measured) |
| L2AR key, attempt 1 | Table 5-71 order | refuted |
| L2AR key, attempt 2 | anchored on DMASK_A | it is SMASK; DMASK_A is elsewhere |

**Check the header first.** It has been right every time and cost minutes;
inference has been wrong every time and cost hours.

## The encoding is sparse ternary

Each rule is 24 words = 768 bits, overwhelmingly `0xffffffff` with a few bits cleared:

| word | distinct values across 413 rules | note |
|---|---:|---|
| 0 | 2 | `0xffffffff` x412 |
| 2, 3 | 4 | `0xffffffff` x312 / `0x00000000` x99 — a binary selector |
| 4, 6, 16, 18 | 35–40 | the most discriminating words |
| most others | 5–19 | `0xffffffff` dominant |

All-ones reads as don't-care and cleared bits as required match conditions — a ternary key, which
is consistent with L2AR being action *resolution*.

## ⚠ Semantics do NOT reduce to one bit per concept

The attractive hypothesis — each name token maps to a bit — **is false as stated**. Tested by
correlating tokens against cleared-bit sets:

| result | finding |
|---|---|
| globally, near-perfect | `Secured` → `w23b3` (1.00 in / 0.07 out), `BinFull` → `w23b1`, `PROVISIONAL` → `w21b1`+`w23b2` |
| globally, nothing | `Learn`, `deny`, `allow`, `Tunnel`, `Smac`, `New` |
| **per-slice, perfect discrimination** | **0 of 20 slice/token combinations** |

So some concepts do land on single bits in the action-ish region (words 21–23), but the general
case does not. The correlating bits cluster at the high end, which suggests words 21–23 carry
result/action flags while the low words carry the match key — a hypothesis, not a finding.

**Statistical inference from 413 samples will not recover this.** What is needed is the L2AR key
*field layout*: which bit ranges the key is composed from. That is a datasheet/register question
(the key-composition registers), not a data-mining one.

## Where regeneration stands

| piece | state |
|---|---|
| rule count, sizes, addresses | **solved** — 413 x 24 words, stride 0x20, exact |
| rule names | **have all 566** (413 L2AR + 153 L3AR) |
| 24-word encoding | **partial** — sparse ternary confirmed; a few semantic bits identified |
| L2AR config pages (`0x144000`+) | not started |
| L3AR geometry | not started — different layout |
| parser (`fm6000_parserinit.c`, 16,960 words) | not started; datasheet §5.5.1 + Table 5-3 document the TCAM/action-SRAM encoding, so this may be the easier one to do first |
| MOD | not started |

Honest read: the geometry work means we can now *place* generated rules correctly and verify
count-exactness against EOS. It does not yet let us author the 24 words. That needs the key field
layout, and until we have it, regeneration cannot proceed past scaffolding.

Next concrete step is the parser rather than L2AR — its encoding is published, `PROVENANCE.md` §4.1
already reports it "fully legible", and it is the single largest transcribed file
(`fm6000_parserinit.c`, 100% microcode).

## Reproducing

Rule geometry:
```
python3 - <<'EOF'
mc={}
for a,v in (l.split() for l in open('fm6000Microcode.raw') if len(l.split())==2):
    mc[int(a,16)]=int(v,16)
starts=sorted(a for a in mc if 0x140000<=a<0x144000 and (a-1) not in mc)
print(len(starts))            # 413
EOF
```

## 2026-08-15: A2's blocker decoded — the three action tables, and the CPU port

`FEATURE-COMPLETE-CHECKLIST.md` A2: *"Format fully known; blocked on a second decode, not on
encoding. Its actions index `DMT_PROFILE`, `SetCpuCode` and `SetMirror` — tables configured
elsewhere, so authoring 'trap to CPU' means knowing which CPU-code entry that is."*

**All three tables are fully specified in `fm6000_api_regs_int.h`.** The block was never a decode
problem; nobody had looked. Same lesson as the six wrong bit-order inferences, and it cost longer.

```
L2AR_ACTION_DMT      0x146000   3 banks x 32 entries x 3 words
                     ACTION_DROP_CODE [7:0]   CmdA [11:8]   CmdB [14:12]
                     ACTION_DMASK   [95:20]   -- 76-bit destination port mask
L2AR_ACTION_CPU_CODE 0x146200   128 entries, each four 8-bit codes CPU_CODE_0..3
L2AR_ACTION_MIRROR   0x146400   4 banks x 128, 4 bits each of
                     MIR_RX / MIR_TX / MIR_TRUNC / MIR_MAP_PRI
```

### ★ Port 0 is the CPU port — two independent signatures

Decoding EOS's populated DMT entries, `CmdA` is the verb and `ACTION_DMASK` the destination:

| CmdA | entries | DMASK port count | reading |
|---|---|---|---|
| 0 | 2 | 76 | forward, all ports |
| 1 | 17 | 0 or 1 | **drop** — no destination, `ACTION_DROP_CODE` carries the reason |
| 2 | 7 | 21 / 75 / 76 | **flood** |
| 3 | 6 | exactly 1, always **bit 0** | **redirect to a single port** |

Two things identify that port without needing a datasheet:

1. **Every flood mask that is not all-76 is missing exactly bit 0** — the four 75-port masks exclude
   `[0]` and nothing else. The port you exclude when flooding is the CPU.
2. **Every `CmdA=3` entry targets bit 0 alone**, and their `ACTION_DROP_CODE` values (`0x82`, `0x85`,
   `0x8a`) overlap the contents of `L2AR_ACTION_CPU_CODE`, while `CmdA=1` drop entries carry
   low codes (`0x03`, `0x06`, `0x10`-`0x1b`).

Consistent with the port numbering established independently from `PARSER_INIT_FIELDS`, where the
three cabled front ports are **20 (Et2), 40 (Et1), 41 (Et3)** — none of them port 0.

### What this unblocks

**"Trap to CPU" is a DMT entry with `CmdA = 3` and `ACTION_DMASK = 1<<0`**, with the CPU code in
`ACTION_DROP_CODE` of the same entry. "Drop" is `CmdA = 1` with an empty mask and a reason code.
That is exactly what A2 said it needed in order to author actions, so **A2 is no longer blocked on
the second decode** — it is down to writing the encoder.

⚠ **Not established.** The same 8-bit field is called `ACTION_DROP_CODE` and appears to serve as a
CPU code on `CmdA=3` entries; that dual role is inferred from the value overlap, not documented.
`CmdB` is `0` in every populated entry, so its meaning is untested. And the 21-port mask in bank 1
excludes all three known front ports, so it is some other port group — do not assume it is a VLAN
member list without checking.

The cheap confirmation for the CPU-port claim is live, not static: program a `CmdA=3` entry and see
whether the frame arrives at `portd`.

### ⚠ `L2AR_ACTION_MIRROR` is entirely unpopulated

Running `l2ar_action_decode.py --summary` over EOS's replay:

```
L2AR_ACTION_DMT       32 populated entries
L2AR_ACTION_CPU_CODE  20 populated of 128
L2AR_ACTION_MIRROR     0 populated
```

**Nothing configures mirroring on this box.** That matters beyond A2, because
`FEATURE-COMPLETE-CHECKLIST.md` A4 proposes unblocking itself by *"(a) TX-mirror to the CPU port,
which needs the mirror table and therefore lands in A2"*. There is no shipped mirror configuration
to decode, copy or learn from — that route would mean authoring the table from the register header
alone and validating it on hardware, which is strictly more work than the alternatives A4 lists.

Since A4's real blocker turned out to be out of date anyway (there are two forwarding ports and the
peer has `tcpdump`), the mirror route can be dropped rather than pursued.

Also from the same run, for whoever authors CPU codes: `L2AR_ACTION_CPU_CODE` holds 20 populated
entries of 128, each packing four 8-bit codes, with values clustering in `0x01`-`0x23` and
`0x82`-`0x8a`. The high-bit group is the one that overlaps the codes on trap-to-CPU DMT entries.
