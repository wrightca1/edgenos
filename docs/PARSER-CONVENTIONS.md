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

## ★ The state machine, traced

`parser_decode.py --states` walks the machine from state 0 at slice 0, applying each rule's
StateOp0..3 to compute the next state. Transitions whose result depends on frame bytes the rule
does not pin (`StateOp` 2 and 3 read FRAME_DATA) are **dropped rather than guessed**, so a trace
reaches fewer states than exist — 26 states and 137 transitions, against 60–79 states per slice
in the raw match data. What it recovers is exact; it is simply partial.

### The four state bytes have distinct roles

| byte | rules constraining it | distinct values | reading |
|---|---:|---:|---|
| **STATE8[0]** | **2,114 (100%)** | 63 | the primary parse state — every single rule keys on it |
| STATE8[1] | 533 (25%) | 15 | auxiliary context |
| STATE8[2] | 679 (32%) | 12 | auxiliary context |
| STATE8[3] | 1,016 (48%) | 14 | auxiliary context / flags |

That every rule constrains `STATE8[0]` and only some constrain the rest is the structural fact a
generated program has to respect: byte 0 is the state variable, bytes 1–3 qualify it.

### Protocol transitions are visible

Tag stacking reads out directly, e.g.

```
slice3 0x00007f10 --VLAN C-tag--> slice4 0x0000f110 --VLAN C-tag--> slice5 0x0000f1ff
```

with `STATE8[1]` carrying the tag-depth context (`0x7f` → `0xf1` → …) while `STATE8[0]` stays
`0x10`.

### ★ What the STATE8[0] values mean

`--state-map` labels each state by what its rules extract: a state whose rules deposit the DMAC is
a state parsing the DMAC. Only rules pinning `STATE8[0]` exactly are counted, so attribution is
unambiguous. 55 of the 63 values are pinned exactly by at least one rule, and they read as a walk
through the packet:

| state | slices | extracts | reading |
|---:|---|---|---|
| `0x01` | 5–15 | L2_DMAC[47:32], [31:16] | first Ethernet word |
| `0x02` | 1–16 | L2_DMAC[15:0], L2_SMAC[47:32] | second Ethernet word |
| `0x11` | 15–20 | L2_TYPE (matches IPv6, FCoE) | EtherType position |
| `0x20` | 4–20 | L3_FLOW/L3_PRI, L3_LENGTH | IP header start |
| `0x30` | 4–20 | L3_FLOW/L3_PRI, L3_FLOW[15:0] | IPv6 flow label |
| `0x22` | 6–22 | L3_TTL / L3_PROT | IP TTL/protocol |
| `0x31` | 5–21 | L3_LENGTH, L3_TTL/L3_PROT | IP header body |
| `0x23` | 7–23 | L3_SIP/DIP[31:16], [15:0] | IP addresses |
| `0x40` | 9–27 | L4_SRC, L4_DST | L4 ports |
| `0x50` | 9–27 | L3_TTL/L3_PROT, L4_SRC | deepest L3/L4 |
| `0x3a` | 4–23 | — (matches **IPv4**) | IPv4 EtherType decision |

A state is not a single packet offset — most span many slices, because the same logical position is
reached at different depths depending on how many tags precede it. That is the whole point of an
unrolled parser, and it is why a generated program must emit the same rule at every slice where the
state is reachable, not once.

⚠ Not every state is labelled. Several high-traffic ones (`0x24` with 160 rules, `0x39`, `0x36`)
extract only into unmapped generic channels (`ch22`, `ch23`, `ch30`, `ch31`), which Table 5-5 does
not name. Those are FIELD16-class channels used for whatever the program wants; their meaning is a
choice EOS made, not a hardware fact.

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

## ⚠⚠ THE ACTION LAYOUT IS SETTLED — and both earlier answers here were wrong

The register header defines every PARSER_RAM field's exact bit position
(`FM6000_PARSER_RAM_l_/h_/b_*`). That is authoritative and ends the guessing:

```
SetFlags       0-37    Byte0-3Enable   54-57   StateOp2/Value2   80-89
Halfword0Dest 38-43    Halfword0/1Add  58-59   StateOp3/Value3   90-99
Halfword1Dest 44-49    StateOp0/Value0 60-69   StateFrameRot   100-101
Halfword0Rot  50-51    StateOp1/Value1 70-79   LegalPadding    102-103
Halfword1Rot  52-53                            TerminateAllowed    104
                                               Terminate           105
                                               ShiftNextSlice  106-108
```

**Two wrong answers preceded it, and both were reached honestly:**

1. *Table 5-3 order packed LSB-first.* Put `Halfword0Dest` at bit 80 → channels
   `{0,1,2,3,4,5,8,60,62}`, never the 5/6/7 and 12/13/14 that Table 5-5 fixes for DMAC/SMAC, and
   60/62 do not exist in a table stopping at 43. Refuted.
2. *Scanning offsets for a 6-bit field hitting those channels.* Gave a unique hit at **bit 45**
   with a compelling slice progression — DMAC high-to-low, then SMAC, then EtherType, deeper each
   slice. **Also wrong.** Bit 38 was in the candidate list and was discarded for hitting 6 of 7
   documented channels instead of 7 of 7.

The lesson worth keeping: a semantic scan over 2,117 samples can produce a unique, plausible and
wrong answer. Check the register header before inferring a layout.

### What the authoritative layout shows

| | |
|---|---|
| entries writing FIELDS | **1,455 of 2,117** (the broken layout said 131) |
| `Terminate` | **used — 344 entries** (the broken layout said never) |
| `TerminateAllowed` | 518 entries |
| `LegalPadding`, `StateOp3` | always 0 |
| channels written | all within the documented 0–43 range |

Channels seen at `Halfword0Dest`: L2_DMAC[15:0] and [47:32], L2_SMAC[31:16], L2_TYPE, L3_TTL/PROT,
L3_LENGTH, L3_FLOW, L3_SIP/DIP, L4_SRC, L2_VID1, ISL_SGLORT and the generic FIELD16 channels.

### Why the round-trip test caught none of this

`gen_parser --verify` reproduces all 2,117 entries bit-identically and still passes with the wrong
offsets. **A decode→encode round-trip proves only that the packing is self-consistent.** Shifted
field boundaries re-encode to exactly the same bits. It validates an encoder; it can never validate
an interpretation. Only an external fact can, and here that was Table 5-5.

The earlier corroboration — "every field lands inside its documented range" — was weak evidence
treated as confirmation. Several of these fields are narrow enough that a wrong offset still yields
plausible-looking values.

Both wrong layouts round-tripped 2,117/2,117 entries perfectly. The layout is now taken from the
register header rather than inferred, so `action_render()` no longer marks anything untrusted.

## What is still needed before authoring

| convention | state |
|---|---|
| key split (state / frame) | **derived** |
| entry state = `0x0` | **derived** |
| state-machine shape per slice | **derived** (shape, not meaning) |
| FIELDS channels written | **solved** — 1,455 entries, all channels in range |
| which FIELDS channel carries which header | **solved** — Table 5-5, fixed in hardware |
| full action field layout | **solved** — exact bit positions from the register header |
| *meaning* of each state value | not derived — which state means "seen one VLAN tag", etc. |
| slice budget per protocol | not derived |

The FIELDS binding — which this document previously called the binding constraint — turned out not
to be a constraint at all: it is fixed in hardware and documented in Table 5-5. Worth recording why
the earlier reasoning was wrong. L3AR does not read FIELDS directly; its inputs (Table 5-30) are
mapper-derived IDs — `L2_DMAC_ID3`, `L3_DIP_ID3`, `L2_TYPE_ID2`. The parser writes FIELDS, the
MAPPER associates those into small IDs, and L3AR keys on the IDs. So the parser-side contract is
Table 5-5 and nothing more.

What now gates authorship is narrower and more tractable:

1. **The meaning of state values** — which state encodes "one VLAN tag seen", etc. This is the
   last parser-side unknown.
2. **The MAPPER's configuration**, since it decides how FIELDS become the IDs L3AR matches on.

Both the encoding and the output contract are now settled, so authoring a parser is no longer
blocked on format questions.

## ★ Hardware validation (2026-08-09)

The first piece of this work to meet silicon. `parser_program.py` generated a CAM entry, and it was
written to the real FM6000 on the 7150 (`0000:02:00.0`) and read back:

```
slice 0, entry 127 (0x1001fc-0x1001ff)
  baseline    00000000 00000000 00000000 00000000
  written     7ffff7ff ffffffeb 7fff0800 ffff0014   <- gen_parser.encode_cam() output
  read back   7ffff7ff ffffffeb 7fff0800 ffff0014   <- byte-identical
  restored    00000000 00000000 00000000 00000000
  PIN 0x1c021 0x00000208 before and after
```

Chosen for zero risk: entry 127 already read all-zero, which is the *never-match* encoding, and the
written entry carried a never-match bit at 31 (outside its care mask) so it could not fire whatever
the traffic. The slot was restored afterwards.

**What this proves:** the `PARSER_CAM(slice, entry, word)` address arithmetic is right against real
hardware, the `words[1:0]=KeyInvert / words[3:2]=Key` ordering is right, and our encoder's bit
patterns survive a write/read round trip through the chip. None of that could be established by
self-consistency, however many round-trips passed.

**What it does not prove:** anything about behaviour. The entry was deliberately unable to match.
Whether a generated parser actually parses is a different question needing L3 extraction, a cold
boot, and a stock-replay control at the same cadence.

## Reproducing

```
python3 asic/fm6000/tools/parser_decode.py --image <fm6000Microcode.raw> --slice 4
```
