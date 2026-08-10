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
