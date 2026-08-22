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

### The key model, and the Ethernet header solved

Those readings can be replaced by a derivation. The 64-bit CAM key is **big-endian**
(key byte 0 is the most significant) and is **not eight bytes of packet**. It is four
bytes of parser state followed by four bytes of packet:

    key byte 0..3  =  State3, State2, State1, State0
    key byte 4..7  =  packet bytes [4s .. 4s+3]

so the parse advances 4 bytes per slice, `hw0` takes `[4s, 4s+1]` and `hw1` takes
`[4s+2, 4s+3]`.

The state half is fixed by two facts that agree. `PARSER_INIT_STATE` is **all zeros
on all 76 ports**, and **every** slice-0 rule cares for key byte 3 `== 0x00` and for
nothing else in bytes 0–2 — "state0 is still the initial state" — while each sets a
*different* `StateValue0`, dispatching into different states for the slices that
follow.

The packet half is fixed by four rules that have to hold at once, and all four do:

    slice 0 rule 6   key bytes 4..7 = 01 80 c2 00
                     -> DMAC 01:80:c2 at packet bytes 0,1,2
    slice 3 rule 6   key bytes 4,5 = 08 00   -> EtherType at packet bytes 12,13
    slice 3 rule 9   key bytes 4,5 = 81 00   -> same position, C-VLAN
    slice 3 rule 5   key bytes 4,5 = 86 dd   -> same position, IPv6

and the same slice-3 rules independently care for the **high nibble of key byte 6**
with values `4` and `6` — the IP version, at packet byte 14, exactly where the L3
header begins.

⚠ **Correction.** An earlier version of this document described key bytes 0–3 as
packet bytes `[4s-4 .. 4s-1]`. That was wrong, and testing it against deeper rules is
what exposed it: it would have made the slice-3 rules care about the middle of the
source MAC, which is not what they are doing. The packet mapping below is unaffected.

With both halves known, the disassembler renders rules as conditions. Slice 0 is a
complete destination-MAC classifier, in precedence order:

    [  1] when st0=00  pkt any                          | hw0->r7 hw1->r6  st0=op1(0x03)
    [  2] when st0=00  pkt [0]=00 [1]=00 [2]=de [3]=ad  | ...              st0=op1(0x07)
    [  3] when st0=00  pkt [0]=00 [1]=00 [2]=00 [3]=00  | ...              st0=op1(0x06)
    [  4] when st0=00  pkt [0]&01=01                    | ...              st0=op1(0x04)
    [  5] when st0=00  pkt [0]=01 [1]=1b [2]=19 [3]=00  | ...              st0=op1(0x0b)
    [  6] when st0=00  pkt [0]=01 [1]=80 [2]=c2 [3]=00  | ...              st0=op1(0x05)
    [  7] when st0=00  pkt [0]=ff [1]=ff [2]=ff [3]=ff  | ...              st0=op1(0x02)

Read in order: the PTP peer-delay group address, the IEEE reserved group address, an
all-zero MAC, a `00:00:de:ad` sentinel, **the multicast bit alone** (`[0]&01`),
broadcast, and a default. Every one of them captures the same two halfwords to
`r7`/`r6` and differs only in the state it leaves behind. That is a classifier, and
it is now readable as one.

Slice 3 is the EtherType dispatch, guarded by the state slice 0 set:

    [  5] when st3&20=20 st0&f0=10  pkt [12]=86 [13]=dd [14]&f0=60 | flags=0x300 shift=2
    [  6] when st3&20=20 st0&f0=10  pkt [12]=08 [13]=00 [14]&f0=40 | flags=0x100 shift=2
    [  7] when st3&20=20 st0&f0=10  pkt [12]=08 [13]=06            | flags=0x4000000
    [  8] when st1&08=08 st0&f0=10  pkt [12]=88 [13]=a8            | st1=op1(0xf1) flags=0x8000
    [  9] when st1&08=08 st0&f0=10  pkt [12]=81 [13]=00            | st1=op1(0xf1) flags=0x8000

IPv6 and IPv4 each check the EtherType **and** the version nibble in the byte that
follows it; ARP checks only the EtherType; both VLAN tags share a state guard and a
flag.

`parser_disasm.py --offsets` resolves the entire Ethernet header, in order:

    byte 0   DMAC[0:1]    r7
    byte 2   DMAC[2:3]    r6
    byte 4   DMAC[4:5]    r5
    byte 6   SMAC[0:1]    r14
    byte 8   SMAC[2:3]    r13, r10
    byte 10  SMAC[4:5]    r12
    byte 12  EtherType    r15

Three independent lines agree on this. Slice 0 matches destination-MAC prefixes and
writes `r7`/`r6`. `r15` is written by the IPv4, IPv6 and ARP rules in near-equal
numbers, which is the EtherType demultiplex point. And the geometric model, derived
without reference to either, places `r7 r6 r5 r14 r13 r12 r15` at bytes
0, 2, 4, 6, 8, 10, 12 — contiguous and in the right order.

⚠ **Registers written past the Ethernet header are not resolved.** Their offset
depends on how deeply the frame is encapsulated, and a single "most common" offset
across the whole program conflates the untagged, VLAN, QinQ, MPLS and tunnel paths.
`r22`/`r23` and `r24`/`r25` are 32-bit fields extracted deep in the parse by more
rules than anything else, which is what source and destination IP addresses would
look like — but that remains inference. Naming them needs a path-aware walk of the
state machine, following `ShiftNextSlice` along one encapsulation at a time. That is
the next step and it is not done.

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
