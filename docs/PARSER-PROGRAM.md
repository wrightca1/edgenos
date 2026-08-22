# The FM6000 packet parser program

`ucode_l2.raw` and `ucode_tail.raw` were the last two files in the tree described
as opaque vendor microcode. They are not opaque. This documents what they are.

## They are register writes, not an instruction stream

Both files are the **same `ADDR VALUE` ASCII text as the replay set**:

    00123100 ffffffff
    00123101 ffffffff

| file | writes | unique addrs | write-once |
|---|---|---|---|
| `ucode_l2.raw` | 30,321 | 30,321 | **100%** |
| `ucode_tail.raw` | 9,146 | 9,094 | 99% |

100% write-once matters. The register replay's residual is 99.8% *repeated* writes
to addresses already written — a protocol, which is why it resists being authored.
These files are the exact opposite: pure table content, no embedded sequence. What
they configure is state, and state can be written from intent.

Decoded against the SDK register map, they are not one thing but several tables:

    ucode_l2.raw    L2AR_CAM 9,912 · PARSER_CAM 8,580 · PARSER_RAM 8,580
                    L2AR_CAM_DMASK 864 · L2AR_RAM 826 · PARSER_INIT_FIELDS 304
    ucode_tail.raw  MOD_CAM 2,792 · L3AR_CAM 2,448 · MOD_VALUE_RAM 658
                    STATS_AR_IDX_CAM 616 · MOD_COMMAND_RAM 369 · L3AR_RAM1/2/4 306 each

So: the packet parser, the L2 and L3 lookup rules, and the header-rewrite engine.
These are the same blocks we already author generators for elsewhere.

## The parser is a 28-slice x 128-entry TCAM state machine

From the SDK register map, `PARSER_CAM` and `PARSER_RAM` share one geometry —
`[128 x 28]`, 4 words per entry, slice stride `0x400`. The two interleave inside
each slice:

    slice s:  CAM entries at 0x100000 + s*0x400 + e*4   (128 x 4 words)
              RAM entries at 0x100200 + s*0x400 + e*4   (128 x 4 words)

`PARSER_CAM` is a 128-bit ternary key — `Key[64] @64`, `KeyInvert[64] @0`, the same
word order as `L3AR_CAM`. A bit is **don't care** where both are set, which is why an
all-ones entry is the universal default rule rather than an empty slot, and
**never-match** where both are clear. The cared-for bits are where the two disagree.

`PARSER_RAM` is the 110-bit action, and the SDK names every field:

    SetFlags[38] @0                         flags raised on a match
    Halfword0Dest[6] @38  Rot[2] @50  Add @58    extract a halfword to a register
    Halfword1Dest[6] @44  Rot[2] @52  Add @59
    Byte0..3Enable @54..57                  which bytes of it are valid
    StateOp0..3[2] + StateValue0..3[8] @60..99    four parallel state updates
    StateFrameRot[2] @100   LegalPadding[2] @102
    TerminateAllowed @104   Terminate @105
    ShiftNextSlice[3] @106                  how far to advance for the next slice

That is a conventional programmable parser: match a 64-bit window, extract two
halfwords into named registers, update four state variables, raise flags, shift, and
either continue or terminate.

⚠ The 2-bit `StateOp` encoding is **not** named in the SDK's field table. The
disassembler prints it numerically (`op0`..`op3`) rather than guessing at meanings.

## The program is legible

`asic/fm6000/tools/parser_disasm.py` disassembles it. All 28 slices are used, with
**2,145 of 3,584 possible entries populated (60%)**.

    occupancy: 0:8 1:12 2:12 3:45 4:50 5:60 6:59 7:64 8:74 9:97 10:112 11:113
               12:106 13:109 14:117 15:116 16:111 17:104 18:103 19:103 20:103
               21:94 22:89 23:85 24:52 25:49 26:49 27:49

Slice 0 is the entry point and matches on destination MAC:

    [  0] key=ffffffffffffffff care=0000000000000000 | -                    <- default
    [  5] key=ffffff00011b1900 care=000000ffffffffff | st0=op1(0x0b)        <- 01:1b:19:00 PTP peer-delay
    [  6] key=ffffff000180c200 care=000000ffffffffff | st0=op1(0x05)        <- 01:80:c2:00 IEEE reserved

Slice 3 matches EtherType, and the constants are plainly readable:

    [  5] key=...86dd6fff care=...fffff000 | flags=0x300 shift=2   <- IPv6, version nibble 6
    [  6] key=...08004fff care=...fffff000 | flags=0x100 shift=2   <- IPv4, version nibble 4
    [  7] key=...0806ffff care=...ffff0000 | flags=0x4000000       <- ARP
    [  4] key=...8906ffff care=...ffff0000 | flags=0x200           <- FCoE
    [  8] key=...88a8ffff care=...ffff0000 | flags=0x8000          <- 802.1ad S-VLAN
    [  9] key=...8100ffff care=...ffff0000 | flags=0x8000          <- 802.1Q C-VLAN
    [  3] key=...88f7ffff care=...ffff0000 | st0=op1(0x3b)         <- PTP / 1588

Scanning every entry for 16-bit fields that are *fully* cared-for gives the protocol
inventory:

| EtherType | slices |
|---|---|
| IPv4, IPv6, ARP, FCoE | 18 each |
| transparent bridging (0x6558, GRE/NVGRE) | 15 |
| PTP / 1588 | 12 |
| 802.1Q C-VLAN, 802.1ad S-VLAN | 10 each |
| MAC control (PAUSE) | 2 |

The same EtherType recurring across slices 3–10 is the parser handling it at
different header depths — unwrapped, behind one VLAN tag, behind two, and so on.

⚠ A matching scan over **8-bit** fields will also report IP protocol numbers (TCP,
UDP, ICMP...), but those hits are individually unreliable: any fully-cared byte that
happens to equal 6 reads as "TCP". The 16-bit EtherType hits above are trustworthy
because a full 16-bit field must be cared-for to register. Do not quote the 8-bit
inventory as fact without confirming an entry's context.


## The parser feeds a 42-register extracted-field file

`Halfword0Dest` and `Halfword1Dest` are 6-bit destination register numbers, and
across the whole program the destinations actually used are **r1..r42, contiguous**.
That register file is the parser's output and the input to every downstream lookup
stage — L2AR, L3AR and the FFU all match on it rather than on raw packet bytes.

The counts show it is not 42 independent scalars. Twelve adjacent pairs are written
by an equal number of rules — `r3/r4`, `r6/r7`, `r12/r13`, `r20/r21`, `r22/r23`,
`r24/r25`, `r28/r29`, `r30/r31`, `r32/r33`, `r34/r35`, `r36/r37`, `r38/r39` — and a
pair of halfwords is a 32-bit field, which is what an IPv4 address or half a MAC
looks like.

`parser_disasm.py --regs` profiles the file: for each register, how many rules write
it, at which parse depths, and what EtherType those rules were matching.

    reg   rules  slices   EtherType context of the rules that write it
    r1    43     3-11     S-VLAN x18, C-VLAN x18
    r2    20     3-12     S-VLAN x10, C-VLAN x10
    r6    69     0-15     -
    r7    69     0-15     -
    r15   124    3-20     IPv4 x22, IPv6 x20, ARP x20
    r22   174    8-23     -
    r23   174    8-23     -
    r24   190    9-27     -
    r25   190    9-27     -
    r42   26     3-11     S-VLAN x9, C-VLAN x9

Three readings are well supported:

- **`r6`/`r7` are the destination MAC.** They are the only registers written from
  **slice 0**, and every slice-0 rule writes exactly `hw0->r7 hw1->r6`. Slice 0 is
  the first 64-bit window of the frame, and its keys match destination-MAC prefixes
  (`01:80:c2:00` IEEE reserved, `01:1b:19:00` PTP peer-delay). The stage that
  examines DMAC is the stage that extracts it.
- **`r1`, `r2` and `r42` are VLAN tag fields.** They are written almost exclusively
  by rules matching `8100`/`88a8`, and only at shallow depths (slices 3–12).
- **`r15` is the L3 dispatch field.** It is written by the rules matching IPv4, IPv6
  and ARP in near-equal numbers (22/20/20) — the EtherType demultiplex point.

⚠ Everything below that is inference from position and pairing, not established.
`r22`/`r23` and `r24`/`r25` are 32-bit fields extracted deep in the parse (slices
8–23 and 9–27) by more rules than anything else, which is what source and destination
IP addresses would look like — but nothing here proves it. They are labelled as
unknown until a rule is traced end to end or the chip is observed classifying a
crafted frame.

### The consumer side is muxed, and readable

`FFU_SLICE_SCENARIO_CFG` carries `ByteMux_0..3` (6 bits each) and `Top4Mux[5]`, so
the FFU builds each scenario's key by **selecting sources into key positions** rather
than using a fixed layout. Across the 151 populated scenario entries only **16
distinct selectors** are ever used: r1, r2, r3, r8, r17, r18, r21, r24, r32, r33,
r34, r35, r40, r53, r58, r60.

⚠ Three of those — r53, r58, r60 — are **outside** the r1..r42 range the parser
writes, so the mux source space is not only parser destinations; it must also reach
metadata the parser does not produce. The selector space is 6 bits (64 sources) and
the mapping from selector number to source is not established.

`L2AR` has no such register: its key layout appears fixed in hardware, so it cannot
be read out the way the FFU's can.

## What this changes

This does not remove a blob. It establishes that the largest remaining unexamined
file is **not** a compiled instruction stream needing a toolchain — it is 2,145 rules
in a documented match/action format, with the field layout coming from the SDK's own
tables and nothing guessed.

It is therefore authorable in the same way `l3arslice1` was: read the rules, decide
what forwarding behaviour they express, and write a generator that expresses it. That
is a large piece of work and is not started.

## Reproducing

    python3 asic/fm6000/tools/parser_disasm.py /mnt/flash/ucode_l2.raw --summary
    python3 asic/fm6000/tools/parser_disasm.py /mnt/flash/ucode_l2.raw --slice 3

Field offsets come from `sdk_fieldmap.py`, which reads the SDK's field-name table at
runtime. As with every tool here, the Intel register header and the SDK are read
where they already are; nothing from them is copied into this tree.
