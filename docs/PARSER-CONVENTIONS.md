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

## ★ The FIELDS binding is a datasheet fact, not a convention to reverse

The thing this document was written to say was missing — which channel carries what — is fixed in
hardware and documented. **Table 5-5, Parser Fixed Mapping:**

| channel | carries | channel | carries |
|---:|---|---:|---|
| 0 | ISL_FTYPE/VTYPE/PRI/USER | 15 | **L2_TYPE (EtherType)** |
| 1 | L2_VID1 (+L2_VPRI1) | 16/17 | L3_FLOW, L3_PRI |
| 2 | L2_VID2 (+L2_VPRI2) | 18 | L3_LENGTH |
| 3 | ISL_SGLORT | 19 | L3_TTL / L3_PROT |
| 4 | ISL_DGLORT | 20,21,36–39,32,33 | L3_SIP / L3_DIP |
| **5,6,7** | **L2_DMAC[15:0],[31:16],[47:32]** | 24,25 | L4_SRC / L4_DST |
| **12,13,14** | **L2_SMAC** | 8–11,26,27,40–42 | FIELD16{A..I} generic |

So a generated parser does not have to guess where to put the DMAC. It has to put it in channels
5/6/7 because the hardware reads it there.

## ⚠⚠ CORRECTION: the earlier FIELDS map in this document was wrong

A previous revision reported only four channels written (0, 5, 15, 60) and flagged the discrepancy
as unresolved. It is now resolved, and the resolution is that **the action field offsets were
wrong**, not the byte-enable reading.

Table 5-5 is what caught it. Any real parser must write channels 5/6/7 and 12/13/14 — under the
old layout `Halfword0Dest` sat at bit 80 and produced `{0,1,2,3,4,5,8,60,62}`: never 6, 7, 12, 13
or 14, and channels 60/62 do not exist in a table that stops at 43. Scanning every bit offset for a
6-bit field that hits the documented channels yields exactly one candidate, **bit 45**, and it is
unambiguous:

```
ch7  L2_DMAC[47:32]  written in slices 1,3,5,6,7...
ch6  L2_DMAC[31:16]  slices 2,4,7,8...
ch5  L2_DMAC[15:0]   slices 6,8,9,10...
ch14 L2_SMAC[47:32]  slices 7,8,9...
ch15 L2_TYPE         slices 10,11,12...
```

That is an Ethernet header being walked in wire order, deeper with each slice. The corrected map
also shows the parser writing L2_VID1/2, ISL_SGLORT/DGLORT, L3_LENGTH, L3_TTL/PROT and SIP/DIP —
a far wider surface than the four channels previously reported.

### Why the round-trip test did not catch it

`gen_parser --verify` reproduces all 2,117 entries bit-identically and still passes with the wrong
offsets. **A decode→encode round-trip proves only that the packing is self-consistent.** Shifted
field boundaries re-encode to exactly the same bits. It validates an encoder; it can never validate
an interpretation. Only an external fact can, and here that was Table 5-5.

The earlier corroboration — "every field lands inside its documented range" — was weak evidence
treated as confirmation. Several of these fields are narrow enough that a wrong offset still yields
plausible-looking values.

**Only `Halfword0Dest` (bit 45) is externally confirmed.** The rest of the Table 5-3 layout is
unverified and `action_render()` marks its output UNTRUSTED. `Halfword1Dest` is not located: bit 65
gives the right kind of pairing with bit 45 — `(7,6)`, `(13,12)`, `(11,10)`, the two halves of one
frame word — but on only 2.2% of entries, which is not enough to claim.

## What is still needed before authoring

| convention | state |
|---|---|
| key split (state / frame) | **derived** |
| entry state = `0x0` | **derived** |
| state-machine shape per slice | **derived** (shape, not meaning) |
| FIELDS channels written | **corrected** — see the correction above; `Halfword0Dest` is bit 45 |
| which FIELDS channel carries which header | **SOLVED** — Table 5-5, fixed in hardware |
| rest of the Table 5-3 action layout | **unverified** — offsets refuted, needs re-derivation |
| *meaning* of each state value | not derived — which state means "seen one VLAN tag", etc. |
| slice budget per protocol | not derived |

The FIELDS binding — which this document previously called the binding constraint — turned out not
to be a constraint at all: it is fixed in hardware and documented in Table 5-5. Worth recording why
the earlier reasoning was wrong. L3AR does not read FIELDS directly; its inputs (Table 5-30) are
mapper-derived IDs — `L2_DMAC_ID3`, `L3_DIP_ID3`, `L2_TYPE_ID2`. The parser writes FIELDS, the
MAPPER associates those into small IDs, and L3AR keys on the IDs. So the parser-side contract is
Table 5-5 and nothing more.

What now gates authorship is narrower and more tractable:

1. **Re-derive the Table 5-3 action layout**, anchored on the one offset external evidence fixes
   (`Halfword0Dest` = bit 45). Without it we cannot emit correct actions at all.
2. **The meaning of state values** — which state encodes "one VLAN tag seen", etc.
3. **The MAPPER's configuration**, since it decides how FIELDS become the IDs L3AR matches on.

## Reproducing

```
python3 asic/fm6000/tools/parser_decode.py --image <fm6000Microcode.raw> --slice 4
```
