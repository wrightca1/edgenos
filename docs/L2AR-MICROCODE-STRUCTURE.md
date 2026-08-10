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

**L3AR does not share this geometry.** `0x158000`–`0x159fff` gives 111 runs of mixed lengths
(2, 52, 84, 100) against 153 named rules. Its layout is a separate problem.

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

## The key composition (Table 5-71), and where the fields sit

The datasheet lists the key fields but not their bit positions. The positions
follow from the data: **100 rules match seg0's full 64 bits together with
seg1[11:0]** — exactly 76 — and seg0 carries 6,409 care-bits of which 6,400 are
those matches. So `DMASK_A` (76 bits, the first entry in Table 5-71) occupies key
bits [75:0], and the table's order then lays out the rest:

| field | width | key bits | segment |
|---|---:|---|---|
| DMASK_A | 76 | 0–75 | seg0 + seg1[11:0] |
| SMASK | 76 | 76–151 | seg1[63:12] + seg2[23:0] |
| ACTION_FLAGS | 76 | 152–227 | seg2[63:24] + seg3[35:0] |
| ACTION_DATA.W8F | 8 | 228–235 | seg3 |
| POL{1,2}_TAG{1,2}_TOP, POL3_TAG_TOP | 4 each | 236–255 | seg3 |
| DROP_CODE | 8 | 256–263 | seg4 |
| L2F_ISTATE | 13 | 264–276 | seg4 |
| DGLORT_TAG | 1 | 277 | seg4 |
| ALU13_Z / ALU46_Z | 16 each | 278–309 | seg4 |
| MA1/MA2_FID2_IVL | 1 each | 310–311 | seg4 |
| *(~19 bits unaccounted — padding or unlisted fields)* | | 312–330 | |
| MA1_LOOKUP / HPV / TAG | 1/4/12 | 331–347 | seg5 |
| MA2_LOOKUP / HPV | 1/4 | 348–352 | seg5 |
| **MA2_MPV** | 4 | **353–356** | seg5 — **measured** |
| MA2_TAG, L2L_ETAG1/2, L2L_ITAG1/2 | 12 each | 357–416 | seg5+ |
| DGLORT, SGLORT | 16 each | 417–448 | |
| L2_DMAC_ID3, L2_SMAC_ID3, L2_TYPE_ID2 | 5/5/4 | 449–462 | |
| ISL_USER | 8 | 463–470 | |

### ⚠⚠ The ordering hypothesis is REFUTED — do not author against that table

Tested, and it fails. `MA2_MPV` is 4 bits and the datasheet says it is "used for
entry write-back control (**including learning**)", so the rules Arista names
`…Learn…` should match it. They do match a clean 4-bit group — **seg5 bits
33–36**, in 11–17 rules each — but the ordering hypothesis predicts `MA2_MPV` at
seg5 bits **14–17**. A 19-bit discrepancy.

Rules *not* named Learn match seg5 bits 0–3, 25–27 and 63 as well, so 33–36 is
specific to learning rather than a generally-hot region.

Working backwards from `MA2_MPV` at key bit 353 places the continuation-page
fields 19 bits later than a straight concatenation predicts, i.e. there are ~19
bits of padding or unlisted fields between the two pages of Table 5-71. That is
not something to guess at.

**Two anchors are now measured**, and only two:

| field | key bits | how |
|---|---|---|
| `DMASK_A` | 0–75 | 100 rules match seg0[63:0]+seg1[11:0] = exactly its 76-bit width |
| `MA2_MPV` | ~353–356 | the `…Learn…` rules match seg5[33:36], and it is the 4-bit learning-control field |

Every other row in the table below is **ordering-derived and now known to be
unreliable**. Confirm each field the way these two were before authoring against
it. This is the fourth time in this work that a plausible ordering assumption
has been wrong — twice for the parser action layout, once for CAM match
priority, now here.

`DMASK_A` also has **bitwise-OR matching semantics** per the datasheet — if any
set bit matches, the whole field matches. That is unlike every other field here
and unlike anything in the parser, and a generator that treats it as a normal
ternary compare will be wrong.

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
