# L2 Action Resolution — structure, and what authoring it would take

`fm6000_l2arseq.c` is the largest transcribed file left in the tree: **29,110
writes**, and after alpha61/alpha63 retired two of its siblings it is 29,110 of the
52,702 pairs that are transcription rather than authorship. This is what is inside
it, read rather than guessed.

## It is not a sequence, it is a table written safely

29,110 writes over 15,201 addresses is **1.9 writes per address**. Compare
`fm6000_eplseq.c`: 22,051 writes over 1,027 addresses, **21.5 each**, which is link
training and genuinely a protocol. L2AR is not that.

    writes per address:  1 -> 4,606 addrs
                         2 -> 10,249 addrs
                       3+ ->   346 addrs

and of the 10,249 written twice, **9,900 are `00000000` followed by the value**.

That is the ternary-CAM safe-update idiom, and it is a *rule*, not data: zero both
`Key` and `KeyInvert` first, which is the **never-match** state, so a half-written
entry can never match live traffic; then write the real value. The 4,606 write-once
addresses are exactly what `fm6000_l2arinit` already covers.

So L2AR's 29,110 writes are ~15,201 values plus one mechanical rule applied to most
of them. The values are the work; the sequence is a loop.

## Where the values come from — three files, one story

`ucode_l2.raw` carries an L2AR block of 12,467 addresses. Every one of them is also
in `l2arseq`, **none is exclusive to it**, and 12,045 have the same final value. The
422 that differ are decisive:

- on all 422, the ucode file's value equals the **first** of `l2arseq`'s two writes
- on the sampled ones, a working chip holds **`l2arseq`'s** value, never the ucode's

So `ucode_l2.raw` supplies the initial L2AR content and `l2arseq` supplies initial
*plus* a 422-entry refinement. That is also, from a third direction, exactly why the
retired `fm6000_l2arpre.c` was wrong: it had captured only phase one.

`l2arseq` additionally writes 2,734 addresses the ucode file does not — 2,376 of
them `L2AR_CAM` — so its content cannot simply be sourced from that file.

## The table: 8 slices x 64 rules

From the SDK register map:

    L2AR_CAM   [6 x 64 x 8]  w=4  stride 0x20  outer 0x800   @0x140000
    L2AR_RAM   [64 x 8]      w=2  stride 0x80                @0x145400
    L2AR_SLICE_CFG   ChainedPrecedence[8] @0, SliceDisable[8] @8

Eight slices of sixty-four rules. Each rule is a **384-bit ternary key** held as six
chunks of `Key[64] @64` / `KeyInvert[64] @0`, and a two-word action. `SliceDisable`
being 8 bits confirms the slice count independently.

`asic/fm6000/tools/l2ar_disasm.py` reads it. Of the 512 rule slots:

| | count | |
|---|---:|---|
| real rules | **404** | 79% |
| never-match, explicitly zeroed | 99 | 19% |
| universal default (all-ones) | 9 | 2% |

**Every one of the 512 is written.** None is left alone. That is the same lesson
L3AR taught: `Key=0/KeyInvert=0` is the never-match state and has to be *written* —
an unwritten slot is not a disabled slot, it holds whatever was there before.

Per slice: slice 7 is fully populated (64 real rules), slice 2 and 5 nearly so
(63, 62), and slice 1 is mostly disabled (24 real, 40 zeroed).

## The action side is fully legible

`L2AR_RAM`'s fields all come from the SDK:

    FLAGS_TAG[8]  DMT_PROFILE[5]  TransformDestMask  DMT_NEXT_STAGE
    SetCpuCode  SetTrapHeader  SetMirror_0..3
    MuxOutput_{DGLORT,VID,QOS,DMASK_IDX,MA_WRITEBACK,W4,W16AB,W16CDEF,
               W8ABCDE,STATS_IDX5AB,STATS_IDX5C,STATS_IDX12A,
               STATS_IDX12B,STATS_IDX16A,STATS_IDX16B}

Each `MuxOutput_X` enables that output and selects an entry from the matching
`L2AR_X_PROFILE_TABLE` (32 entries each, all present in the register map). So a
rule reads as "raise these flags, take the DMT profile, and drive these outputs from
these profiles" — and a real one disassembles as:

    [ 1] act=DMT_PROFILE=10 TransformDestMask SetTrapHeader
                 MuxOutput_MA_WRITEBACK MuxOutput_DGLORT MuxOutput_VID

`SetTrapHeader` and `SetCpuCode` are the CPU-punt controls, which is why this block
is where "does anything reach the CPU" is decided.

## ⚠ What is still missing: the key layout — and it is not the parser's output

The 384-bit key has **no field table in the SDK** and no key-format register in the
L2AR block. The working assumption was that it is fed from the parser's
extracted-field register file, which `parser_walk.py` has since named through the
IPv4 header (`r7/r6/r5` DMAC, `r14/r13/r12` SMAC, `r15` EtherType, `r21/r20` source
IP, `r23/r22` destination IP, `r24` L4 source port).

**That assumption is wrong, and the test that broke it is worth recording.** All 404
rules were searched for lab constants whose values are known exactly — this switch's
router MAC `44:4c:a8:31:5d:ab`, the port GLORTs `03ee` and `03ef`, and the EtherTypes
`0800` and `0806` — at every byte offset, requiring the bytes to be fully cared-for.

**Zero hits, for every constant.**

The distribution of cared-for bytes says the same thing. Across all 404 rules the
care mask concentrates in the **low 12 bytes** of the 384-bit key, where roughly 100
rules apiece care, against a handful in the high bytes:

    byte  36  37  38  39  40  41  42  43  44  45  46  47
    rules 128 99 135  98  98 100 101 100  98 103 100 106

So L2AR does not match on packet fields. It matches on **action-resolution
metadata** — the results of the L2 lookup and the flags raised by earlier stages —
which is consistent with `FLAGS_TAG` in its action word and with `L2AR_FLAGS_CAM`
existing as a separate structure alongside the main CAM. The name was the clue:
this is L2 *action resolution*, not L2 lookup.

Naming those 96 bits means tracing what the L2 lookup stage and the parser's
`SetFlags` (38 bits) deposit, not mapping packet offsets. That is the real next step
and it has not been done.

## The FFU is the stage that keys on packet fields

`FFU_SLICE_SCENARIO_CFG` has `ByteMux_0..3` (6 bits each) and `Top4Mux[5]`, so unlike
L2AR the FFU **selects** its key sources. Reading the 151 populated scenarios and
interpreting each selector as a parser register number gives groupings that look
right for a classifier:

    slice 1  scen 0-2   r60(?)  r24(L4 sport)  r18(IP total len)  r18(IP total len)
    slice 2  scen 0-2   r21(SIP[0:1])  r21(SIP[0:1])  r8(?)  r58(?)

⚠ **But the selector index space is not established, and there are two reasons to
doubt the direct reading.** Selectors 53, 58 and 60 appear, and the parser only ever
writes r1..r42 — so either the space is not register numbers, or the FFU can also
reach metadata the parser does not produce. And selectors repeat across adjacent byte
positions (`r21 r21`, `r18 r18`, and four-way repeats like `r1 r1 r1 r1`), which fits
a **halfword** register supplying two consecutive key bytes better than it fits four
independent byte picks.

Both readings are consistent with the data available here. This is left unresolved
rather than guessed, because the same shortcut — a plausible mapping that fit every
case checked — has already had to be corrected twice on the parser side.

## Honest scope

This does not remove `fm6000_l2arseq.c`. It establishes that:

1. the file is a **table plus one mechanical safe-update rule**, not a protocol —
   so unlike `eplseq` it is not disqualified from being authored;
2. the table is **404 real rules in 8 slices**, not 29,110 opaque pairs;
3. the action half of every rule is already fully named;
4. the key half is **not** packet fields and needs the L2 lookup's outputs and the
   parser's 38 `SetFlags` bits traced instead — established by searching all 404
   rules for known lab constants and finding none.

That is the same position L3AR was in before `l3arslice1`, which went on to replace
1,088 replay lines with 6 authored rules where EOS shipped 32.

## Reproducing

    python3 asic/fm6000/tools/l2ar_disasm.py <l2ar writes> --summary
    python3 asic/fm6000/tools/l2ar_disasm.py <l2ar writes> --slice 0
