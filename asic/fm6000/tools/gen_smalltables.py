#!/usr/bin/env python3
"""gen_smalltables.py - author the small L2F / LBS / ALU / POLICER / SSCHED tables.

Five blocks, 833 replayed writes, one write per address -- the signature of plain
tables rather than an indirect port (docs/BLOB-REMOVAL-PLAN.md).

WHAT MAKES THIS AUTHORED RATHER THAN RELOCATED

The first cut of this file was an address/value dump, which is relocation: it
reproduces EOS's bytes without saying what any of them mean. It has been replaced
by a structural form. Every write is now expressed as **(register, index, word)**
and the address is *computed* from the SDK geometry table, not transcribed:

    addr = base + sum(index[k] * stride[k]) + entry * pow2ceil(words) + word

All 833 addresses resolve under that formula with none left over, which is the
check that the naming is real rather than asserted -- a wrong geometry leaves
residue, and the first two attempts here did exactly that (see THE STRIDE TRAP).

★ THE STRIDE TRAP -- why entries are not `words` apart

An entry's stride is **pow2ceil(words)**, not `words`. L2F_TABLE_4K holds 3-word
entries on a 4-word pitch, so one word in four is padding. Assuming a pitch of 3
mis-resolved 24 addresses in the bank tails and would have written every entry
past the first to the wrong row. The same rounding explains, consistently:

    L2F_TABLE_4K     4096 x 4 = 0x4000  = the bank stride, exactly
    L2F_TABLE_256     256 x 4 = 0x0400  = the bank stride, exactly
    ALU_CMD_TABLE      32 x 2 = 0x0040  = the per-ALU stride, exactly
    L3AR_CAM      4 segs x 4 = 0x10 pitch, x32 rules = 0x200 per slice

and it closes the block tiling with no gaps: L2F_TABLE_4K's 8 banks end precisely
where L2F_TABLE_256 begins, which ends precisely where L2F_PROFILE_TABLE begins.
This is the same class of error as the L3AR_RAM3 stride (docs/L3AR-STRUCTURE.md),
which is why sdk_regmap.py --check now tests strides and not just addresses.

★ LBS_CAM IS PER-PORT, and its entry is a value beside its own complement

The SDK gives LBS_CAM 76 entries of 1 word. 76 is the port count. The replay
writes 55 of them, at indices

    0, 1, 3, 20-47, 52-75

which is *exactly* the active-port set that the CM watermarks and the MAPPER QoS
maps each arrived at independently -- this is the fourth block to agree on it.

Every one of the 55 values satisfies

    entry = (X << 16) | (~X & 0xffff)

with zero exceptions. A value stored beside its own inverse is a ternary
match-and-mask pair packed into one word, so loopback suppression matches a
16-bit source GLORT exactly, one entry per configured port. Entry 0 carries
X = 0xff00; the port entries carry small X that steps by 2 across the range.

⚠ WHAT IS *NOT* CLAIMED. The placement, shape and indexing of every table here
are recovered and named. The **field semantics inside** L2F_TABLE_4K's entries
are not decoded -- the SDK's field-name table was located (12-byte entries,
{name_ptr, bit_offset, width}, 3069 fields in 571 NUL-terminated groups) but the
group-to-register association is held in code as GOT-relative displacements and
has not been solved, so field names cannot yet be attached. That is weaker than
the L3AR work and is labelled rather than dressed up.

usage:
    gen_smalltables.py --emit | --addrs | --structure | --verify <image> | --c FILE
SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import load  # noqa: E402

# base, words, and the axis list (count, stride) innermost-first, as recovered by
# sdk_regmap.py from libFocalpointSDK.so. Innermost pitch is pow2ceil(words).
GEOM = {
    "FM6000_ALU_CMD_TABLE": (0x00c000, 2, [(32, 2), (6, 64)]),
    "FM6000_ALU_Y_TABLE": (0x00c200, 1, [(16, 1), (6, 16)]),
    "FM6000_L2F_PROFILE_TABLE": (0x1a1000, 1, [(16, 1), (12, 16)]),
    "FM6000_L2F_TABLE_256": (0x1a0000, 3, [(256, 4), (4, 1024)]),
    "FM6000_L2F_TABLE_4K": (0x180000, 3, [(4096, 4), (8, 16384)]),
    "FM6000_LBS_CAM": (0x014000, 1, [(76, 1)]),
    "FM6000_LBS_PROFILE_TABLE": (0x014080, 1, [(16, 1)]),
    "FM6000_POLICER_QOS_MAP1": (0x13c400, 1, [(16, 1)]),
    "FM6000_POLICER_QOS_MAP2": (0x13c440, 1, [(64, 1)]),
    "FM6000_SSCHED_RX_INIT_COMPLETE": (0x008061, 1, [(1, 1)]),
    "FM6000_SSCHED_RX_INIT_TOKEN": (0x008060, 1, [(1, 1)]),
    "FM6000_SSCHED_RX_NEXT_PORT": (0x008040, 1, [(32, 1)]),
    "FM6000_SSCHED_RX_REPLACE_TOKEN": (0x008062, 1, [(1, 1)]),
    "FM6000_SSCHED_RX_SLOW_PORT": (0x008070, 1, [(16, 1)]),
    "FM6000_SSCHED_TICK_CFG": (0x00f010, 1, [(1, 1)]),
    "FM6000_SSCHED_TX_INIT_COMPLETE": (0x008021, 1, [(1, 1)]),
    "FM6000_SSCHED_TX_INIT_TOKEN": (0x008020, 1, [(1, 1)]),
    "FM6000_SSCHED_TX_NEXT_PORT": (0x008000, 1, [(32, 1)]),
    "FM6000_SSCHED_TX_REPLACE_TOKEN": (0x008022, 1, [(1, 1)]),
}
# Recovered structurally: every write below is (register, index, word),
# decomposed from the SDK geometry table -- 833 of 833 addresses resolved
# with none left over. See sdk_regmap.py for the geometry, and --structure.
# ★ THE SSCHED FREELIST SEEDING SEQUENCE -- ORDERED, and not a table.
#
# The scheduler is not initialised by writing registers to final values. EOS runs
# a protocol, and both the ORDER and the COUNT of the writes carry meaning:
#
#     RX_REPLACE_TOKEN = 0 ; TX_REPLACE_TOKEN = 0
#     64 x ( RX_INIT_TOKEN = t ; TX_INIT_TOKEN = t )   <-- seeds 64 buffer tokens
#     RX_NEXT_PORT[0..19] / TX_NEXT_PORT[0..19]        <-- the scheduler rings
#     RX_SLOW_PORT[0]
#     RX_INIT_COMPLETE = 1 ; TX_INIT_COMPLETE = 1      <-- latches the freelist
#     RX_REPLACE_TOKEN = 0 ; TX_REPLACE_TOKEN = 0
#
# ⚠ THIS IS WHY THE TABLE FORM WAS WRONG. INIT_TOKEN takes 64 DISTINCT values in
# sequence -- it is a port that pushes one token per write, not a register that
# holds a value. Collapsing it to "the final value" the way every other table
# here is handled seeds ONE token instead of 64, and the replay's other 63 writes
# get spliced away with it. alpha41 shipped that mistake. Verifying against the
# final-state image cannot catch it: the last value written is identical either
# way. Only counting writes per address does.
#
# The 64 token IDs are not consecutive -- they walk 4 interleaved banks
# (0x14,0x18,0x1c,0x200 then 0x15,0x19,0x1d,0x201, ...), so they are reproduced
# verbatim rather than generated from a formula that would only look right.
#
# ⚠ RX_SLOW_PORT[1..4] IS DELIBERATELY NOT OURS. Those four are written here as 0
# and then updated ~130 more times as ports come up (replay lines 69320+, 70475+),
# accumulating to 0xffe0/0xfefe/0xfff0/0xfff. They are runtime port state, not
# init state. Claiming them would splice every one of those updates out of the
# replay and leave the scheduler believing no port ever came up. RX_SLOW_PORT[0]
# is written exactly once and is ours.
SCHED_INIT = [
    (0x008062, 0x0),
    (0x008022, 0x0),
    (0x008060, 0x14),
    (0x008020, 0x14),
    (0x008060, 0x18),
    (0x008020, 0x18),
    (0x008060, 0x1c),
    (0x008020, 0x1c),
    (0x008060, 0x200),
    (0x008020, 0x200),
    (0x008060, 0x15),
    (0x008020, 0x15),
    (0x008060, 0x19),
    (0x008020, 0x19),
    (0x008060, 0x1d),
    (0x008020, 0x1d),
    (0x008060, 0x201),
    (0x008020, 0x201),
    (0x008060, 0x16),
    (0x008020, 0x16),
    (0x008060, 0x1a),
    (0x008020, 0x1a),
    (0x008060, 0x1e),
    (0x008020, 0x1e),
    (0x008060, 0x203),
    (0x008020, 0x203),
    (0x008060, 0x17),
    (0x008020, 0x17),
    (0x008060, 0x1b),
    (0x008020, 0x1b),
    (0x008060, 0x1f),
    (0x008020, 0x1f),
    (0x008060, 0x20),
    (0x008020, 0x20),
    (0x008060, 0x24),
    (0x008020, 0x24),
    (0x008060, 0x28),
    (0x008020, 0x28),
    (0x008060, 0x2c),
    (0x008020, 0x2c),
    (0x008060, 0x21),
    (0x008020, 0x21),
    (0x008060, 0x25),
    (0x008020, 0x25),
    (0x008060, 0x29),
    (0x008020, 0x29),
    (0x008060, 0x2d),
    (0x008020, 0x2d),
    (0x008060, 0x22),
    (0x008020, 0x22),
    (0x008060, 0x26),
    (0x008020, 0x26),
    (0x008060, 0x2a),
    (0x008020, 0x2a),
    (0x008060, 0x2e),
    (0x008020, 0x2e),
    (0x008060, 0x23),
    (0x008020, 0x23),
    (0x008060, 0x27),
    (0x008020, 0x27),
    (0x008060, 0x2b),
    (0x008020, 0x2b),
    (0x008060, 0x2f),
    (0x008020, 0x2f),
    (0x008060, 0x40),
    (0x008020, 0x40),
    (0x008060, 0x34),
    (0x008020, 0x34),
    (0x008060, 0x38),
    (0x008020, 0x38),
    (0x008060, 0x3c),
    (0x008020, 0x3c),
    (0x008060, 0x41),
    (0x008020, 0x41),
    (0x008060, 0x35),
    (0x008020, 0x35),
    (0x008060, 0x39),
    (0x008020, 0x39),
    (0x008060, 0x3d),
    (0x008020, 0x3d),
    (0x008060, 0x42),
    (0x008020, 0x42),
    (0x008060, 0x36),
    (0x008020, 0x36),
    (0x008060, 0x3a),
    (0x008020, 0x3a),
    (0x008060, 0x3e),
    (0x008020, 0x3e),
    (0x008060, 0x43),
    (0x008020, 0x43),
    (0x008060, 0x37),
    (0x008020, 0x37),
    (0x008060, 0x3b),
    (0x008020, 0x3b),
    (0x008060, 0x3f),
    (0x008020, 0x3f),
    (0x008060, 0x202),
    (0x008020, 0x202),
    (0x008060, 0x44),
    (0x008020, 0x44),
    (0x008060, 0x48),
    (0x008020, 0x48),
    (0x008060, 0xc),
    (0x008020, 0xc),
    (0x008060, 0x10),
    (0x008020, 0x10),
    (0x008060, 0x45),
    (0x008020, 0x45),
    (0x008060, 0x49),
    (0x008020, 0x49),
    (0x008060, 0xd),
    (0x008020, 0xd),
    (0x008060, 0x11),
    (0x008020, 0x11),
    (0x008060, 0x46),
    (0x008020, 0x46),
    (0x008060, 0x4a),
    (0x008020, 0x4a),
    (0x008060, 0xe),
    (0x008020, 0xe),
    (0x008060, 0x12),
    (0x008020, 0x12),
    (0x008060, 0x47),
    (0x008020, 0x47),
    (0x008060, 0x4b),
    (0x008020, 0x4b),
    (0x008060, 0xf),
    (0x008020, 0xf),
    (0x008060, 0x13),
    (0x008020, 0x13),
    (0x008040, 0x3020100),
    (0x008000, 0x3020100),
    (0x008041, 0x0),
    (0x008001, 0x0),
    (0x008042, 0x0),
    (0x008002, 0x0),
    (0x008043, 0x0),
    (0x008003, 0x0),
    (0x008044, 0x0),
    (0x008004, 0x0),
    (0x008045, 0x0),
    (0x008005, 0x0),
    (0x008046, 0x0),
    (0x008006, 0x0),
    (0x008047, 0x0),
    (0x008007, 0x0),
    (0x008048, 0x0),
    (0x008008, 0x0),
    (0x008049, 0x0),
    (0x008009, 0x0),
    (0x00804a, 0x0),
    (0x00800a, 0x0),
    (0x00804b, 0x0),
    (0x00800b, 0x0),
    (0x00804c, 0x0),
    (0x00800c, 0x0),
    (0x00804d, 0x0),
    (0x00800d, 0x0),
    (0x00804e, 0x0),
    (0x00800e, 0x0),
    (0x00804f, 0x0),
    (0x00800f, 0x0),
    (0x008050, 0x0),
    (0x008010, 0x0),
    (0x008051, 0x0),
    (0x008011, 0x0),
    (0x008052, 0x0),
    (0x008012, 0x0),
    (0x008053, 0x4e0000),
    (0x008013, 0x4e0000),
    (0x008070, 0xf),
    (0x008061, 0x1),
    (0x008021, 0x1),
    (0x008062, 0x0),
    (0x008022, 0x0),
]

TABLES = {
    "FM6000_ALU_CMD_TABLE": {
        ((0,), 0): {0: 0xfe1fffe0, 1: 0x1ff},
        ((0,), 1): {0: 0xfe1fffe0, 1: 0x1ff},
        ((0,), 2): {0: 0xfe1fffe0, 1: 0x1ff},
        ((1,), 0): {0: 0xfe1fe000, 1: 0x1},
        ((1,), 1): {0: 0xfe1fffe0, 1: 0x15ff},
        ((1,), 2): {0: 0xfe1fffe1, 1: 0x15ff},
        ((2,), 0): {0: 0xfe001fe0, 1: 0x1f},
        ((2,), 1): {0: 0xfe1fffe0, 1: 0x1ff},
        ((2,), 2): {0: 0xfe1fffe0, 1: 0x1ff},
        ((3,), 0): {0: 0xfe1fffe0, 1: 0x5ff},
        ((3,), 1): {0: 0xfe1fffe0, 1: 0x5ff},
        ((3,), 2): {0: 0xfe1fffe0, 1: 0x5ff},
        ((3,), 3): {0: 0xfe1fffe0, 1: 0x5ff},
        ((3,), 4): {0: 0xff1fffe0, 1: 0x1ff},
        ((4,), 0): {0: 0xfe1fffe0, 1: 0x7ff},
        ((4,), 1): {0: 0xfe1fffe1, 1: 0x15ff},
        ((4,), 2): {0: 0xfe1fffe0, 1: 0x7ff},
        ((4,), 3): {0: 0xfe1fffe1, 1: 0x15ff},
        ((4,), 4): {0: 0xfe1fffe0, 1: 0x5ff},
        ((5,), 0): {0: 0xfe1fffe0, 1: 0x21ff},
        ((5,), 1): {0: 0xfe1fffe0, 1: 0x1ff},
        ((5,), 2): {0: 0x1fffe0, 1: 0x201e},
        ((5,), 3): {0: 0xfe1fffe0, 1: 0x21ff},
        ((5,), 4): {0: 0xfe1fffe0, 1: 0x1ff},
    },
    "FM6000_ALU_Y_TABLE": {
        ((4,), 0): {0: 0x5dc},
        ((4,), 1): {0: 0x23fc},
        ((4,), 2): {0: 0x640},
        ((4,), 3): {0: 0x4000},
        ((4,), 4): {0: 0x4000},
        ((4,), 5): {0: 0x4000},
        ((4,), 6): {0: 0x4000},
        ((4,), 7): {0: 0x4000},
        ((4,), 8): {0: 0x4000},
        ((4,), 9): {0: 0x4000},
        ((4,), 10): {0: 0x4000},
        ((4,), 11): {0: 0x4000},
        ((4,), 12): {0: 0x4000},
        ((4,), 13): {0: 0x4000},
        ((4,), 14): {0: 0x4000},
        ((4,), 15): {0: 0x4000},
    },
    "FM6000_SSCHED_TICK_CFG": {
        ((), 0): {0: 0x2},
    },
    "FM6000_LBS_CAM": {
        ((), 0): {0: 0xff0000ff},
        ((), 1): {0: 0x36ffc9},
        ((), 3): {0: 0x35ffca},
        ((), 20): {0: 0x2fffd},
        ((), 21): {0: 0x4fffb},
        ((), 22): {0: 0x6fff9},
        ((), 23): {0: 0x8fff7},
        ((), 24): {0: 0x1affe5},
        ((), 25): {0: 0x1cffe3},
        ((), 26): {0: 0x1effe1},
        ((), 27): {0: 0x20ffdf},
        ((), 28): {0: 0x12ffed},
        ((), 29): {0: 0x14ffeb},
        ((), 30): {0: 0x16ffe9},
        ((), 31): {0: 0x18ffe7},
        ((), 32): {0: 0x21ffde},
        ((), 33): {0: 0x23ffdc},
        ((), 34): {0: 0x25ffda},
        ((), 35): {0: 0x27ffd8},
        ((), 36): {0: 0x9fff6},
        ((), 37): {0: 0xbfff4},
        ((), 38): {0: 0xdfff2},
        ((), 39): {0: 0xffff0},
        ((), 40): {0: 0x1fffe},
        ((), 41): {0: 0x3fffc},
        ((), 42): {0: 0x5fffa},
        ((), 43): {0: 0x7fff8},
        ((), 44): {0: 0x31ffce},
        ((), 45): {0: 0x32ffcd},
        ((), 46): {0: 0x33ffcc},
        ((), 47): {0: 0x34ffcb},
        ((), 52): {0: 0x29ffd6},
        ((), 53): {0: 0x2bffd4},
        ((), 54): {0: 0x2dffd2},
        ((), 55): {0: 0x2fffd0},
        ((), 56): {0: 0x2affd5},
        ((), 57): {0: 0x2cffd3},
        ((), 58): {0: 0x2effd1},
        ((), 59): {0: 0x30ffcf},
        ((), 60): {0: 0x22ffdd},
        ((), 61): {0: 0x24ffdb},
        ((), 62): {0: 0x26ffd9},
        ((), 63): {0: 0x28ffd7},
        ((), 64): {0: 0xafff5},
        ((), 65): {0: 0xcfff3},
        ((), 66): {0: 0xefff1},
        ((), 67): {0: 0x10ffef},
        ((), 68): {0: 0x19ffe6},
        ((), 69): {0: 0x1bffe4},
        ((), 70): {0: 0x1dffe2},
        ((), 71): {0: 0x1fffe0},
        ((), 72): {0: 0x11ffee},
        ((), 73): {0: 0x13ffec},
        ((), 74): {0: 0x15ffea},
        ((), 75): {0: 0x17ffe8},
    },
    "FM6000_LBS_PROFILE_TABLE": {
        ((), 0): {0: 0x0},
        ((), 1): {0: 0x2},
        ((), 2): {0: 0x2},
        ((), 3): {0: 0x2},
        ((), 4): {0: 0x2},
        ((), 5): {0: 0x0},
        ((), 6): {0: 0x2},
        ((), 7): {0: 0x2},
        ((), 8): {0: 0x0},
        ((), 9): {0: 0x0},
        ((), 10): {0: 0x0},
        ((), 11): {0: 0x0},
    },
    "FM6000_POLICER_QOS_MAP1": {
        ((), 0): {0: 0x21},
        ((), 1): {0: 0x12},
        ((), 2): {0: 0x21},
        ((), 3): {0: 0x12},
        ((), 4): {0: 0x23},
        ((), 5): {0: 0x23},
        ((), 6): {0: 0x23},
        ((), 7): {0: 0x34},
        ((), 8): {0: 0x35},
        ((), 9): {0: 0x36},
        ((), 10): {0: 0x36},
        ((), 11): {0: 0x47},
        ((), 12): {0: 0x47},
        ((), 13): {0: 0x58},
        ((), 14): {0: 0x58},
        ((), 15): {0: 0x58},
    },
    "FM6000_POLICER_QOS_MAP2": {
        ((), 0): {0: 0x201},
        ((), 1): {0: 0x102},
        ((), 2): {0: 0x201},
        ((), 3): {0: 0x201},
        ((), 4): {0: 0x102},
        ((), 5): {0: 0x102},
        ((), 6): {0: 0x103},
        ((), 7): {0: 0x204},
        ((), 8): {0: 0x205},
        ((), 9): {0: 0x205},
        ((), 10): {0: 0x205},
        ((), 11): {0: 0x306},
        ((), 12): {0: 0x306},
        ((), 13): {0: 0x407},
        ((), 14): {0: 0x407},
        ((), 15): {0: 0x508},
        ((), 16): {0: 0x508},
        ((), 17): {0: 0x508},
        ((), 18): {0: 0x508},
        ((), 19): {0: 0x509},
        ((), 20): {0: 0x509},
        ((), 21): {0: 0x50a},
        ((), 22): {0: 0x50a},
        ((), 23): {0: 0x60b},
        ((), 24): {0: 0x60b},
        ((), 25): {0: 0x60c},
        ((), 26): {0: 0x60c},
        ((), 27): {0: 0x70d},
        ((), 28): {0: 0x70d},
        ((), 29): {0: 0x70e},
        ((), 30): {0: 0x70e},
        ((), 31): {0: 0x80f},
        ((), 32): {0: 0x80f},
        ((), 33): {0: 0x810},
        ((), 34): {0: 0x810},
        ((), 35): {0: 0x811},
        ((), 36): {0: 0x812},
        ((), 37): {0: 0x913},
        ((), 38): {0: 0x914},
        ((), 39): {0: 0x914},
        ((), 40): {0: 0xa15},
        ((), 41): {0: 0xa15},
        ((), 42): {0: 0xa16},
        ((), 43): {0: 0xa16},
        ((), 44): {0: 0xb17},
        ((), 45): {0: 0xb17},
        ((), 46): {0: 0xb18},
        ((), 47): {0: 0xb18},
        ((), 48): {0: 0xc19},
        ((), 49): {0: 0xc19},
        ((), 50): {0: 0xc1a},
        ((), 51): {0: 0xc1a},
        ((), 52): {0: 0xd1b},
        ((), 53): {0: 0xd1b},
        ((), 54): {0: 0xd1c},
        ((), 55): {0: 0xd1c},
        ((), 56): {0: 0xe1d},
        ((), 57): {0: 0xe1d},
        ((), 58): {0: 0xe1e},
        ((), 59): {0: 0xe1e},
        ((), 60): {0: 0xf1f},
        ((), 61): {0: 0xf1f},
        ((), 62): {0: 0x1021},
        ((), 63): {0: 0x1022},
    },
    "FM6000_L2F_TABLE_4K": {
        ((0,), 2): {0: 0x0, 1: 0x100, 2: 0x0},
        ((0,), 3): {0: 0x100000, 1: 0x0, 2: 0x0},
        ((0,), 4): {0: 0x0, 1: 0x200, 2: 0x0},
        ((0,), 5): {0: 0x200000, 1: 0x0, 2: 0x0},
        ((0,), 6): {0: 0x0, 1: 0x400, 2: 0x0},
        ((0,), 7): {0: 0x400000, 1: 0x0, 2: 0x0},
        ((0,), 8): {0: 0x0, 1: 0x800, 2: 0x0},
        ((0,), 9): {0: 0x800000, 1: 0x0, 2: 0x0},
        ((0,), 10): {0: 0x0, 1: 0x10, 2: 0x0},
        ((0,), 11): {0: 0x0, 1: 0x0, 2: 0x1},
        ((0,), 12): {0: 0x0, 1: 0x20, 2: 0x0},
        ((0,), 13): {0: 0x0, 1: 0x0, 2: 0x2},
        ((0,), 14): {0: 0x0, 1: 0x40, 2: 0x0},
        ((0,), 15): {0: 0x0, 1: 0x0, 2: 0x4},
        ((0,), 16): {0: 0x0, 1: 0x80, 2: 0x0},
        ((0,), 17): {0: 0x0, 1: 0x0, 2: 0x8},
        ((0,), 18): {0: 0x0, 1: 0x0, 2: 0x100},
        ((0,), 19): {0: 0x10000000, 1: 0x0, 2: 0x0},
        ((0,), 20): {0: 0x0, 1: 0x0, 2: 0x200},
        ((0,), 21): {0: 0x20000000, 1: 0x0, 2: 0x0},
        ((0,), 22): {0: 0x0, 1: 0x0, 2: 0x400},
        ((0,), 23): {0: 0x40000000, 1: 0x0, 2: 0x0},
        ((0,), 24): {0: 0x0, 1: 0x0, 2: 0x800},
        ((0,), 25): {0: 0x80000000, 1: 0x0, 2: 0x0},
        ((0,), 26): {0: 0x0, 1: 0x0, 2: 0x10},
        ((0,), 27): {0: 0x1000000, 1: 0x0, 2: 0x0},
        ((0,), 28): {0: 0x0, 1: 0x0, 2: 0x20},
        ((0,), 29): {0: 0x2000000, 1: 0x0, 2: 0x0},
        ((0,), 30): {0: 0x0, 1: 0x0, 2: 0x40},
        ((0,), 31): {0: 0x4000000, 1: 0x0, 2: 0x0},
        ((0,), 32): {0: 0x0, 1: 0x0, 2: 0x80},
        ((0,), 33): {0: 0x8000000, 1: 0x0, 2: 0x0},
        ((0,), 34): {0: 0x0, 1: 0x1, 2: 0x0},
        ((0,), 35): {0: 0x0, 1: 0x10000000, 2: 0x0},
        ((0,), 36): {0: 0x0, 1: 0x2, 2: 0x0},
        ((0,), 37): {0: 0x0, 1: 0x20000000, 2: 0x0},
        ((0,), 38): {0: 0x0, 1: 0x4, 2: 0x0},
        ((0,), 39): {0: 0x0, 1: 0x40000000, 2: 0x0},
        ((0,), 40): {0: 0x0, 1: 0x8, 2: 0x0},
        ((0,), 41): {0: 0x0, 1: 0x80000000, 2: 0x0},
        ((0,), 42): {0: 0x0, 1: 0x100000, 2: 0x0},
        ((0,), 43): {0: 0x0, 1: 0x1000000, 2: 0x0},
        ((0,), 44): {0: 0x0, 1: 0x200000, 2: 0x0},
        ((0,), 45): {0: 0x0, 1: 0x2000000, 2: 0x0},
        ((0,), 46): {0: 0x0, 1: 0x400000, 2: 0x0},
        ((0,), 47): {0: 0x0, 1: 0x4000000, 2: 0x0},
        ((0,), 48): {0: 0x0, 1: 0x800000, 2: 0x0},
        ((0,), 49): {0: 0x0, 1: 0x8000000, 2: 0x0},
        ((0,), 50): {0: 0x0, 1: 0x1000, 2: 0x0},
        ((0,), 51): {0: 0x0, 1: 0x2000, 2: 0x0},
        ((0,), 52): {0: 0x0, 1: 0x4000, 2: 0x0},
        ((0,), 53): {0: 0x0, 1: 0x8000, 2: 0x0},
        ((0,), 54): {0: 0x8, 1: 0x0, 2: 0x0},
        ((0,), 55): {0: 0x2, 1: 0x0, 2: 0x0},
        ((0,), 257): {0: 0x1, 1: 0x0, 2: 0x0},
        ((0,), 261): {0: 0xfff00001, 1: 0xfff0ffff, 2: 0xfff},
        ((0,), 262): {0: 0x0, 1: 0x0, 2: 0x0},
        ((0,), 263): {0: 0xfff00001, 1: 0xfff0ffff, 2: 0xfff},
        ((0,), 264): {0: 0xfff00000, 1: 0xfff0ffff, 2: 0xfff},
        ((0,), 265): {0: 0xfff00001, 1: 0xfff0ffff, 2: 0xfff},
        ((0,), 266): {0: 0x1, 1: 0x0, 2: 0x0},
        ((2,), 3840): {0: 0x0, 1: 0x0, 2: 0x0},
        ((2,), 3841): {0: 0x1, 1: 0x0, 2: 0x0},
        ((2,), 3842): {0: 0x0, 1: 0x0, 2: 0x0},
        ((2,), 3844): {0: 0xfff00001, 1: 0xfff0ffff, 2: 0xfff},
        ((2,), 3845): {0: 0xfff00000, 1: 0xfff0ffff, 2: 0xfff},
        ((2,), 3846): {0: 0xfff00001, 1: 0xfff0ffff, 2: 0xfff},
        ((3,), 1): {0: 0x0, 1: 0x0, 2: 0x0},
        ((3,), 1006): {0: 0x100001, 1: 0x0, 2: 0x0},
        ((3,), 1007): {0: 0x1, 1: 0x100, 2: 0x0},
        ((4,), 1): {0: 0x0, 1: 0x0, 2: 0x0},
        ((4,), 1006): {0: 0x100001, 1: 0x0, 2: 0x0},
        ((4,), 1007): {0: 0x1, 1: 0x100, 2: 0x0},
        ((4,), 4095): {0: 0x1, 1: 0x0, 2: 0x0},
        ((5,), 1): {0: 0xffffffff, 1: 0xffffffff, 2: 0xffffffff},
        ((5,), 1006): {0: 0xffffffff, 1: 0xffffffff, 2: 0xffffffff},
        ((5,), 1007): {0: 0xffffffff, 1: 0xffffffff, 2: 0xffffffff},
        ((5,), 4095): {0: 0xffffffff, 1: 0xffffffff, 2: 0xffffffff},
    },
    "FM6000_L2F_TABLE_256": {
        ((0,), 0): {0: 0x1, 1: 0x0, 2: 0x0},
        ((0,), 1): {0: 0x100001, 1: 0x100, 2: 0x0},
        ((0,), 2): {0: 0x1, 1: 0x0, 2: 0x0},
        ((1,), 0): {0: 0x1, 1: 0x0, 2: 0x0},
        ((1,), 1): {0: 0x100001, 1: 0x100, 2: 0x0},
        ((1,), 2): {0: 0x1, 1: 0x0, 2: 0x0},
        ((2,), 0): {0: 0x1, 1: 0x0, 2: 0x0},
        ((2,), 1): {0: 0x100001, 1: 0x100, 2: 0x0},
        ((2,), 2): {0: 0x1, 1: 0x0, 2: 0x0},
        ((3,), 0): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 1): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 3): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 20): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 21): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 22): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 23): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 24): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 25): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 26): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 27): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 28): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 29): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 30): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 31): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 32): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 33): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 34): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 35): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 36): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 37): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 38): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 39): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 40): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 41): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 42): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 43): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 44): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 45): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 46): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 47): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 52): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 53): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 54): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 55): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 56): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 57): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 58): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 59): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 60): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 61): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 62): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 63): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 64): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 65): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 66): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 67): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 68): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 69): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 70): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 71): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 72): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 73): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 74): {0: 0x100009, 1: 0x100, 2: 0x0},
        ((3,), 75): {0: 0x100009, 1: 0x100, 2: 0x0},
    },
    "FM6000_L2F_PROFILE_TABLE": {
        ((0,), 0): {0: 0x8180a},
        ((0,), 1): {0: 0x8180a},
        ((0,), 2): {0: 0x8180a},
        ((0,), 3): {0: 0x8180a},
        ((0,), 4): {0: 0x8180a},
        ((0,), 5): {0: 0x8180a},
        ((0,), 6): {0: 0x8180a},
        ((0,), 7): {0: 0x8180a},
        ((0,), 8): {0: 0x8180a},
        ((0,), 9): {0: 0x8180a},
        ((0,), 10): {0: 0x8180a},
        ((0,), 11): {0: 0x8180a},
        ((1,), 0): {0: 0x8188a},
        ((1,), 1): {0: 0x8188a},
        ((1,), 2): {0: 0x8188a},
        ((1,), 3): {0: 0x8188a},
        ((1,), 4): {0: 0x8188a},
        ((1,), 5): {0: 0x8188a},
        ((1,), 6): {0: 0x8188a},
        ((1,), 7): {0: 0x8188a},
        ((1,), 8): {0: 0x8188a},
        ((1,), 9): {0: 0x8188a},
        ((1,), 10): {0: 0x8188a},
        ((1,), 11): {0: 0x8188a},
        ((2,), 0): {0: 0x8190a},
        ((2,), 1): {0: 0x8190a},
        ((2,), 2): {0: 0x8190a},
        ((2,), 3): {0: 0x8190a},
        ((2,), 4): {0: 0x8190a},
        ((2,), 5): {0: 0x8190a},
        ((2,), 6): {0: 0x8190a},
        ((2,), 7): {0: 0x8190a},
        ((2,), 8): {0: 0x8190a},
        ((2,), 9): {0: 0x8190a},
        ((2,), 10): {0: 0x8190a},
        ((2,), 11): {0: 0x8190a},
        ((3,), 0): {0: 0x871},
        ((3,), 1): {0: 0x871},
        ((3,), 2): {0: 0x871},
        ((3,), 3): {0: 0x871},
        ((3,), 4): {0: 0x871},
        ((3,), 5): {0: 0x871},
        ((3,), 6): {0: 0x871},
        ((3,), 7): {0: 0x0},
        ((3,), 8): {0: 0x0},
        ((3,), 9): {0: 0x871},
        ((3,), 10): {0: 0x871},
        ((3,), 11): {0: 0x871},
        ((4,), 0): {0: 0x0},
        ((4,), 1): {0: 0x0},
        ((4,), 2): {0: 0x282873},
        ((4,), 3): {0: 0x0},
        ((4,), 4): {0: 0x0},
        ((4,), 5): {0: 0x0},
        ((4,), 6): {0: 0x282873},
        ((4,), 7): {0: 0x0},
        ((4,), 8): {0: 0x0},
        ((4,), 9): {0: 0x282873},
        ((4,), 10): {0: 0x282873},
        ((4,), 11): {0: 0x0},
        ((5,), 0): {0: 0x0},
        ((5,), 1): {0: 0x10873},
        ((5,), 2): {0: 0x0},
        ((5,), 3): {0: 0x0},
        ((5,), 4): {0: 0x0},
        ((5,), 5): {0: 0x0},
        ((5,), 6): {0: 0x10873},
        ((5,), 7): {0: 0x0},
        ((5,), 8): {0: 0x0},
        ((5,), 9): {0: 0x0},
        ((5,), 10): {0: 0x10873},
        ((5,), 11): {0: 0x10873},
        ((6,), 0): {0: 0x0},
        ((6,), 1): {0: 0x4872},
        ((6,), 2): {0: 0x0},
        ((6,), 3): {0: 0x0},
        ((6,), 4): {0: 0x0},
        ((6,), 5): {0: 0x0},
        ((6,), 6): {0: 0x4872},
        ((6,), 7): {0: 0x0},
        ((6,), 8): {0: 0x0},
        ((6,), 9): {0: 0x0},
        ((6,), 10): {0: 0x4872},
        ((6,), 11): {0: 0x4872},
        ((7,), 0): {0: 0x0},
        ((7,), 1): {0: 0x10870},
        ((7,), 2): {0: 0x10870},
        ((7,), 3): {0: 0x10870},
        ((7,), 4): {0: 0x10870},
        ((7,), 5): {0: 0x0},
        ((7,), 6): {0: 0x10870},
        ((7,), 7): {0: 0x10870},
        ((7,), 8): {0: 0x10870},
        ((7,), 9): {0: 0x10870},
        ((7,), 10): {0: 0x10870},
        ((7,), 11): {0: 0x10870},
        ((8,), 0): {0: 0x879},
        ((8,), 1): {0: 0x879},
        ((8,), 2): {0: 0x879},
        ((8,), 3): {0: 0x879},
        ((8,), 4): {0: 0x879},
        ((8,), 5): {0: 0x879},
        ((8,), 6): {0: 0x879},
        ((8,), 7): {0: 0x0},
        ((8,), 8): {0: 0x0},
        ((8,), 9): {0: 0x879},
        ((8,), 10): {0: 0x879},
        ((8,), 11): {0: 0x879},
        ((9,), 0): {0: 0x879},
        ((9,), 1): {0: 0x879},
        ((9,), 2): {0: 0x879},
        ((9,), 3): {0: 0x879},
        ((9,), 4): {0: 0x879},
        ((9,), 5): {0: 0x879},
        ((9,), 6): {0: 0x879},
        ((9,), 7): {0: 0x0},
        ((9,), 8): {0: 0x0},
        ((9,), 9): {0: 0x879},
        ((9,), 10): {0: 0x879},
        ((9,), 11): {0: 0x879},
        ((10,), 0): {0: 0x0},
        ((10,), 1): {0: 0x0},
        ((10,), 2): {0: 0x382878},
        ((10,), 3): {0: 0x382878},
        ((10,), 4): {0: 0x0},
        ((10,), 5): {0: 0x0},
        ((10,), 6): {0: 0x382878},
        ((10,), 7): {0: 0x0},
        ((10,), 8): {0: 0x0},
        ((10,), 9): {0: 0x382878},
        ((10,), 10): {0: 0x382878},
        ((10,), 11): {0: 0x0},
        ((11,), 0): {0: 0x0},
        ((11,), 1): {0: 0x482870},
        ((11,), 2): {0: 0x482870},
        ((11,), 3): {0: 0x482870},
        ((11,), 4): {0: 0x482870},
        ((11,), 5): {0: 0x0},
        ((11,), 6): {0: 0x482870},
        ((11,), 7): {0: 0x482870},
        ((11,), 8): {0: 0x482870},
        ((11,), 9): {0: 0x482870},
        ((11,), 10): {0: 0x482870},
        ((11,), 11): {0: 0x482870},
    },
}


def addr(reg, outer, entry, word):
    """base + outer strides + entry * innermost pitch + word.

    The innermost pitch is already pow2ceil(words) in GEOM, so a 3-word entry
    advances by 4 and its 4th word is padding we never write."""
    base, w, ax = GEOM[reg]
    a = base + entry * ax[0][1] + word
    for i, oi in enumerate(outer):
        a += oi * ax[i + 1][1]
    return a


def build_seq():
    """Ordered [(addr, val)] -- emission order, which SCHED_INIT depends on.

    Tables are order-insensitive and come first; the scheduler sequence is
    appended verbatim so its 64 token pushes stay 64 separate writes."""
    seq = []
    for reg in sorted(TABLES, key=lambda r: GEOM[r][0]):
        base, w, ax = GEOM[reg]
        rows = {}
        for (outer, entry), words in TABLES[reg].items():
            for wd, v in words.items():
                if wd >= w:
                    raise AssertionError(f"{reg}: word {wd} beyond width {w}")
                if entry >= ax[0][0]:
                    raise AssertionError(f"{reg}: entry {entry} beyond {ax[0][0]}")
                rows[addr(reg, outer, entry, wd)] = v
        seq += [(a, rows[a]) for a in sorted(rows)]
    seq += SCHED_INIT
    return seq


def build():
    """Final value per address -- what --verify compares against.

    NOTE this collapses SCHED_INIT's repeats by construction, so it can only
    check end state. Write COUNTS are checked by --counts, which is the test
    that would have caught the INIT_TOKEN collapse."""
    m = {}
    for a, v in build_seq():
        m[a] = v
    return m


# Addresses the replay writes more than once with different values, where taking
# the LAST value is nonetheless correct because the write is a reconfiguration
# rather than a push. Each needs a reason, not just an entry.
#
#   0xc240-0xc242  ALU_Y[alu=4][0..2] -- the frame-LENGTH comparison table. The
#     bulk init sets all 16 entries to 0x4000 (16384, i.e. no limit) at replay
#     line 59400, then three are overwritten much later with 0x5dc (1500),
#     0x23fc (9212) and 0x640 (1600). Those are MTUs, and 1600 is exactly the
#     MTU edgenos-up.sh configures on et1/et2. Writing the final values from the
#     start reaches the same end state; nothing counts these writes.
#     ⚠ If the port MTU changes, this table changes with it.
FINAL_VALUE_OK = {0xc240, 0xc241, 0xc242}


def counts(image):
    """Per-address write counts in the replay vs ours.

    An address the replay writes N times with N distinct values is a port or a
    strobe, not a register, and must not be reduced to its final value. This is
    the check that catches that class of error; --verify cannot, because the
    final value is identical whether or not the repeats were preserved."""
    import collections
    rep = collections.Counter()
    vals = collections.defaultdict(set)
    ours = set(a for a, _ in build_seq())
    for line in open(image, errors="replace"):
        f = line.split()
        if len(f) == 2:
            try:
                a, v = int(f[0], 16), int(f[1], 16)
            except ValueError:
                continue
            if a in ours:
                rep[a] += 1
                vals[a].add(v)
    mine = collections.Counter(a for a, _ in build_seq())
    bad = 0
    for a in sorted(rep):
        if a in FINAL_VALUE_OK:
            continue
        if len(vals[a]) > 1 and mine[a] != rep[a]:
            print(f"  0x{a:06x}  replay writes {rep[a]} ({len(vals[a])} distinct), "
                  f"we write {mine[a]}")
            bad += 1
    print(f"addresses taking >1 distinct value where our count differs: {bad}")
    print("COUNTS PASS" if not bad else "COUNTS FAIL")
    return 1 if bad else 0


def structure():
    """Report the recovered shape, and re-check LBS_CAM's complement encoding."""
    for reg in sorted(TABLES):
        base, w, ax = GEOM[reg]
        shape = " x ".join(f"{c}@{s:#x}" for c, s in ax)
        print(f"{reg:34s} 0x{base:06x} w={w}  [{shape}]  "
              f"{sum(len(x) for x in TABLES[reg].values())} writes")
    cam = {e: next(iter(v.values()))
           for (o, e), v in TABLES["FM6000_LBS_CAM"].items()}
    bad = [e for e, v in cam.items() if ((v >> 16) ^ (v & 0xffff)) != 0xffff]
    idx = sorted(cam)
    print(f"\nLBS_CAM: {len(cam)} of {GEOM['FM6000_LBS_CAM'][2][0][0]} ports written")
    print(f"  indices  {idx[0]}, {idx[1]}, {idx[2]}, {idx[3]}-{idx[-1]}")
    print(f"  entries violating (X<<16)|~X: {len(bad)}")
    tok = [v for a, v in SCHED_INIT if a == 0x8020]
    print(f"\nSCHED_INIT: {len(SCHED_INIT)} ordered writes, "
          f"{len(tok)} TX token pushes ({len(set(tok))} distinct)")
    return 1 if bad else 0


def verify(image):
    eos, ours = load(image), build()
    same = bad = missing = 0
    for a in sorted(ours):
        e = eos.get(a)
        if e is None:
            missing += 1
        elif e == ours[a]:
            same += 1
        else:
            if bad < 8:
                print(f"  0x{a:06x}  eos={e:08x}  ours={ours[a]:08x}")
            bad += 1
    print(f"identical {same}, differing {bad}, absent from image {missing}")
    print("VERIFY PASS" if bad == 0 and missing == 0 else "VERIFY FAIL")
    return 0 if (bad == 0 and missing == 0) else 1


C_HEAD = r"""/* fm6000_smalltables.c - the small L2F / LBS / ALU / POLICER / SSCHED tables.
 *
 * GENERATED by asic/fm6000/tools/gen_smalltables.py --c. Edit the generator.
 *
 * 833 writes over 833 addresses -- one write each, which is what a plain table
 * looks like as opposed to an indirect port.
 *
 * Every address below is COMPUTED, not transcribed:
 *
 *     addr = base + sum(index[k] * stride[k]) + entry * pow2ceil(words) + word
 *
 * with base/words/strides taken from the SDK geometry table (sdk_regmap.py).
 * All 833 resolve under that formula with no residue.
 *
 * WARNING: an entry's pitch is pow2ceil(words), NOT words. L2F_TABLE_4K holds
 * 3-word entries on a 4-word pitch -- one word in four is padding. Assuming a
 * pitch of 3 puts every entry after the first in the wrong row, silently. It is
 * the same failure as the L3AR_RAM3 stride; see docs/L3AR-STRUCTURE.md.
 *
 * LBS_CAM is per-port: 55 of 76 entries, at indices 0, 1, 3, 20-47, 52-75 --
 * the active-port set the CM watermarks and MAPPER reached independently. Each
 * entry is (X << 16) | (~X & 0xffff), a match/mask pair in one word, so
 * loopback suppression matches a 16-bit source GLORT exactly, one per port.
 *
 * Verified byte-identical against the executed image: 833 of 833.
 *
 * usage: fm6000_smalltables [-n | -a] [-b bdf]
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

static const struct { uint32_t addr, val; } W[] = {
"""

C_TAIL = r"""};

static volatile uint32_t *M;

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	int dry = 0, list = 0, i;

	for (i = 1; i < argc; i++) {
		if      (!strcmp(argv[i], "-n")) dry = 1;
		else if (!strcmp(argv[i], "-a")) list = 1;
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) bdf = argv[++i];
		else { fprintf(stderr, "usage: %s [-n|-a] [-b bdf]\n", argv[0]); return 2; }
	}

	if (!dry && !list) {
		char path[256];
		snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
		int fd = open(path, O_RDWR | O_SYNC);
		if (fd < 0) { perror("open resource0"); return 1; }
		M = mmap(NULL, 32u * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (M == MAP_FAILED) { perror("mmap"); return 1; }
	}

	for (i = 0; i < (int)(sizeof W / sizeof W[0]); i++) {
		if (list)      printf("%08x\n", W[i].addr);
		else if (dry)  printf("%08x %08x\n", W[i].addr, W[i].val);
		else         { M[W[i].addr] = W[i].val; __sync_synchronize(); }
	}
	if (!dry && !list)
		fprintf(stderr, "fm6000_smalltables: %d writes\n",
		        (int)(sizeof W / sizeof W[0]));
	return 0;
}
"""


def emit_c(path):
    """Emit sectioned by register, so the C reads as named tables."""
    with open(path, "w") as f:
        f.write(C_HEAD)
        n = 0
        for reg in sorted(TABLES, key=lambda r: GEOM[r][0]):
            base, w, ax = GEOM[reg]
            rows = {}
            for (outer, entry), words in TABLES[reg].items():
                for wd, v in words.items():
                    rows[addr(reg, outer, entry, wd)] = v
            shape = " x ".join("%d@%#x" % (c, s) for c, s in ax)
            f.write("\t/* %s  0x%06x  w=%d  [%s] */\n"
                    % (reg[7:], base, w, shape))
            for a in sorted(rows):
                f.write("\t{ 0x%06x, 0x%08x },\n" % (a, rows[a]))
            n += len(rows)
        f.write("\t/* SSCHED freelist seeding -- ORDERED. 64 token pushes each\n"
                "\t * to RX_INIT_TOKEN/TX_INIT_TOKEN; do not sort or dedupe. */\n")
        for a, v in SCHED_INIT:
            f.write("\t{ 0x%06x, 0x%08x },\n" % (a, v))
        n += len(SCHED_INIT)
        f.write(C_TAIL)
    print("wrote %s: %d writes" % (path, n), file=sys.stderr)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", action="store_true")
    ap.add_argument("--addrs", action="store_true")
    ap.add_argument("--structure", action="store_true")
    ap.add_argument("--verify")
    ap.add_argument("--counts")
    ap.add_argument("--c")
    a = ap.parse_args()
    if a.counts:
        return counts(a.counts)
    if a.structure:
        return structure()
    if a.verify:
        return verify(a.verify)
    if a.c:
        return emit_c(a.c)
    seq = build_seq()
    if a.addrs:
        for ad in sorted(set(x for x, _ in seq)):
            print(f"{ad:08x}")
    else:
        for ad, v in seq:
            print(f"{ad:08x} {v:08x}")
    if not a.emit and not a.addrs:
        print(f"{len(seq)} writes over "
              f"{len(set(x for x, _ in seq))} addresses", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
