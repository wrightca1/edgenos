# Parser conventions: what the rest of the chip expects

`gen_parser.py` can emit valid CAM+RAM words — verified by round-tripping all 2,117 of EOS's
entries bit-identically. That is not enough to author a parser. A program also has to agree with
L2AR, L3AR and FFU about *conventions*: what the inter-slice state means, and which FIELDS channel
carries which extracted header. Those choices are implicit in EOS's program, and
`fm6000_parserinit.c` currently satisfies them only by being a copy of it.

This is what has been derived so far. Everything here is measured from the decode.

## ★ The 64-bit key splits state from frame data

Datasheet 5.5.1 says each slice keys on the next 4-byte frame word plus the previous slice's
32-bit state. The split is:

```
key[63:32]  STATE       4 x STATE8 bytes, transformed by StateOp0..3
key[31:0]   FRAME_DATA  the slice's 4-byte parsing window
```

Confirmed on slice 4 entry 3, the IPv4 rule: `care = 0x00ff00ff_ffffffff` — the frame half fully
cared with `0x00010800` (EtherType 0x0800 in the low halfword), the state half partially cared
at `0x3a`.

Care masks bear the split out. On the **state** half the dominant mask is `0x000000ff` (586 of
2,117 entries), i.e. most rules test only STATE8[0] — exactly the per-byte model Table 5-3
describes. On the **frame** half the dominant mask is `0x00000000` (1,265 entries): **most rules
do not look at frame data at all** and are pure state transitions.

## The state machine is legible

Distinct state values matched, per slice:

| slice | states | shape |
|---|---:|---|
| 0 | 1 | only `0x0` — the entry state |
| 1 | 8 | `0x2`–`0xb` |
| 2 | 5 | |
| 3–8 | 27–43 | fan-out as tags and headers accumulate |
| 9–23 | 60–79 | the wide middle |
| 24–27 | 33–34 | a distinct late set (`0x2b50`, `0x2c50`, `0x3250`, `0x3350`, `0x3c50`) |

Slice 0 keying only on state `0x0` is the anchor: **parsing starts from state zero**, and any
program we author has to as well.

## ⚠ The FIELDS output surface is small

Only **131 of 2,117 entries write anything to FIELDS** — `Byte0-3Enable` is `0000` on 1,986 of
them. Filtering to entries that actually enable a byte, the channels used are:

| channel | writes | slices | rotations | +CHECKSUM |
|---:|---:|---|---|---:|
| 0 | 34 | 1–23 (16 slices) | 0, 3 | 0 |
| 5 | 15 | 9–23 (15) | 0 | 0 |
| 15 | 96 | 8–23 (16) | 0 | 0 |
| 60 | 96 | 8–23 (16) | 0 | 0 |

So the parser's job is overwhelmingly to walk headers, advance state and set flags. Extraction
into FIELDS is targeted and narrow.

⚠ **Unresolved discrepancy.** `Halfword0Dest` takes 9 distinct values across all entries
(`{0,1,2,3,4,5,8,60,62}`) and `Halfword1Dest` 3 (`{0,1,15}`), but only 4 of those channels ever
have a byte enabled. Channels 1, 2, 3, 4, 8, 62 are addressed by rules that write nothing. Either
the dest field is left set as a don't-care when unused, or the byte-enable reading is incomplete.
**Do not treat the 4-channel map as final until that is resolved** — if enables are being misread,
the real output map is wider and a generated parser would under-populate FIELDS.

## What is still needed before authoring

| convention | state |
|---|---|
| key split (state / frame) | **derived** |
| entry state = `0x0` | **derived** |
| state-machine shape per slice | **derived** (shape, not meaning) |
| FIELDS channels written | **derived, with the caveat above** |
| *meaning* of each state value | not derived — which state means "seen one VLAN tag", etc. |
| which FIELDS channel L2AR/FFU read for what | **not derived** — the binding constraint |
| slice budget per protocol | not derived |

The last two are what actually gate authorship. We can emit a parser that walks Ethernet/VLAN/IPv4
correctly and still forward nothing, if it deposits the destination MAC in a channel the FFU is not
reading. That binding lives on the *consumer* side — L2AR's input keys (datasheet Table 5-30) and
the FFU key-composition registers — not in the parser tables, so the next work is there rather than
here.

## Reproducing

```
python3 asic/fm6000/tools/parser_decode.py --image <fm6000Microcode.raw> --slice 4
```
