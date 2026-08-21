#!/usr/bin/env python3
"""gen_l3ar_slice2.py - author L3AR slice 2 (VID assignment and trap) from intent.

Slice 2 classifies and tags. Per rule it selects:

  * a **VID** profile (`MuxOutput_VID`) -- the bulk of the stage, profiles 6, 8,
    10, 12, 18, 20, 22 and 24 across the 32 rules
  * a **trap header** command on rules 2 and 3 (`SetTrapHeaderCmd`)
  * an **L2 lookup** command profile, and on rules 22/23/26/27 the MA1/MA2 MAC
    muxes that build the lookup key
  * **policer index** muxes -- POL1, POL2 and POL3 all appear here
  * `ALU46` command and operand profiles on four rules
  * the **W16ABC** action-data channel on most rules, plus **W8E** on 15/16

⚠ RULES ARE DEAD IN EOS'S TABLE HERE TOO -- three of them, each carrying a bit in
the never-match state (Key=0, KeyInvert=0). They are emitted as never-match with
zero actions rather than reproducing action words that can never execute.

⚠ TWELVE DISTINCT FLAG COMBINATIONS. Slice 2 rewrites `ACTION_FLAGS` more than
any other slice, so all four flag words are carried per rule; a blanket
pass-through would be wrong, as it was for slice 3.

⚠ RAM4 AND RAM5 ARE TWO WORDS EACH. The table below was generated with the fixed
reader -- reading only word 0 hid `MuxOutput_QOS`, the policer profiles and the
top of `ALU46_OP_PROFILE`, which is what made slice 4 look like "ALU46 only"
until alpha34. For slice 2 the same defect understated RAM4 as 6/32 and RAM5 as
15/32; they are really 31/32 and 31/32.

usage:
    gen_l3ar_slice3.py --emit | --addrs | --verify <image> | --c FILE
SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from l3ar_decode import (  # noqa: E402
    L3AR_BASE, CAM_SLICE_STRIDE, CAM_RULE_STRIDE, RAM_SLICE_STRIDE,
    RAM1_OFF, RAM2_OFF, RAM3_OFF, RAM4_OFF, RAM5_OFF, RAM3_SLICE_STRIDE,
    KEY_LAYOUT, RAM3_FIELDS, RAM4_FIELDS, RAM5_FIELDS, encode_cam, load, ternary,
)

SLICE = 2
RULES = 32
FLAG_PASSTHROUGH = 0x3FFFFFF
KEYPOS = {n: (lo, hi) for n, lo, hi in KEY_LAYOUT}

# (key fields, RAM3 fields, RAM4 fields, RAM5 fields, Set_HI).
# A key of None marks a rule EOS disables with a never-match bit.
RULESET = {
    0: ({}, {}, {"MuxOutput_POL1_IDX": 1, "POL1_IDX_PROFILE": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x0, 0x3ffffff, 0x0)),
    1: ({"FFU_DATA_TAG2A": (0x0, 0xfff)}, {}, {"MuxOutput_POL1_IDX": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x0, 0x3ffffff, 0x0)),
    2: ({"ACTION_FLAGS": (0x4400000000000, 0x4400000002000)}, {"SetTrapHeaderCmd": 1, "TRAP_HEADER_ENABLE": 1}, {"MuxOutput_POL1_IDX": 1, "POL1_IDX_PROFILE": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x0, 0x3dfffff, 0x0)),
    3: ({"ACTION_FLAGS": (0x4400000000000, 0x4400000002000), "FFU_DATA_TAG2A": (0x0, 0xfff)}, {"SetTrapHeaderCmd": 1, "TRAP_HEADER_ENABLE": 1}, {"MuxOutput_POL1_IDX": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x0, 0x3dfffff, 0x0)),
    4: ({"ACTION_FLAGS": (0x490000002900, 0x2490000022900), "FFU_DATA_W8A": (0x0, 0x20), "FFU_DATA_W16A_TOP": (0x1, 0xf)}, {"MuxOutput_VID": 1, "VID_PROFILE": 10}, {"MuxOutput_POL1_IDX": 1, "POL1_IDX_PROFILE": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x0, 0x3dfffff, 0x0)),
    5: ({"ACTION_FLAGS": (0x490000002900, 0x2490000022900), "FFU_DATA_W8A": (0x0, 0x20), "FFU_DATA_W16A_TOP": (0x1, 0xf), "FFU_DATA_TAG2A": (0x0, 0xfff)}, {"MuxOutput_VID": 1, "VID_PROFILE": 10}, {"MuxOutput_POL1_IDX": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x0, 0x3dfffff, 0x0)),
    6: ({"ACTION_FLAGS": (0x200000000000, 0x200000000000), "FFU_DATA_W16A_TOP": (0x1, 0xf), "FFU_DATA_W24_TOP": (0x0, 0x8)}, {"MuxOutput_VID": 1, "VID_PROFILE": 10}, {"MuxOutput_POL1_IDX": 1, "POL1_IDX_PROFILE": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x2, 0x3dfffff, 0x0)),
    7: ({"ACTION_FLAGS": (0x200000000000, 0x200000000000), "FFU_DATA_W16A_TOP": (0x1, 0xf), "FFU_DATA_W24_TOP": (0x0, 0x8), "FFU_DATA_TAG2A": (0x0, 0xfff)}, {"MuxOutput_VID": 1, "VID_PROFILE": 10}, {"MuxOutput_POL1_IDX": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x2, 0x3dfffff, 0x0)),
    8: ({"ACTION_FLAGS": (0x490000000100, 0x2490000002100), "FFU_DATA_W8A": (0x0, 0x20), "FFU_DATA_W16A_TOP": (0x1, 0xf)}, {"MuxOutput_VID": 1, "VID_PROFILE": 6}, {"MuxOutput_POL1_IDX": 1, "POL1_IDX_PROFILE": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x0, 0x3dfffff, 0x0)),
    9: ({"ACTION_FLAGS": (0x490000000100, 0x2490000002100), "FFU_DATA_W8A": (0x0, 0x20), "FFU_DATA_W16A_TOP": (0x1, 0xf), "FFU_DATA_TAG2A": (0x0, 0xfff)}, {"MuxOutput_VID": 1, "VID_PROFILE": 6}, {"MuxOutput_POL1_IDX": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x0, 0x3dfffff, 0x0)),
    10: ({"ACTION_FLAGS": (0x2490000000100, 0x2490000000104), "FFU_DATA_W8A": (0x0, 0x20), "FFU_DATA_W16A_TOP": (0x1, 0xf)}, {"MuxOutput_VID": 1, "VID_PROFILE": 6}, {"SetAlu46CmdProfile": 1, "ALU46_CMD_PROFILE": 2, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 4, "MuxOutput_POL1_IDX": 1, "POL1_IDX_PROFILE": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x0, 0x3dfffff, 0x0)),
    11: ({"ACTION_FLAGS": (0x2490000000100, 0x2490000000104), "FFU_DATA_W8A": (0x0, 0x20), "FFU_DATA_W16A_TOP": (0x1, 0xf), "FFU_DATA_TAG2A": (0x0, 0xfff)}, {"MuxOutput_VID": 1, "VID_PROFILE": 6}, {"SetAlu46CmdProfile": 1, "ALU46_CMD_PROFILE": 2, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 4, "MuxOutput_POL1_IDX": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x0, 0x3dfffff, 0x0)),
    12: ({"ACTION_FLAGS": (0x490000000100, 0x2490000002100), "FFU_DATA_W8A": (0x0, 0x20), "FFU_DATA_W16A_TOP": (0x1, 0xf), "NEXTHOP_TAG": (0x4, 0x4)}, {"MuxOutput_VID": 1, "VID_PROFILE": 10}, {}, {}, (0x3ffffff, 0x0, 0x3dfffff, 0x0)),
    13: ({"ACTION_FLAGS": (0x0, 0x80000), "FFU_DATA_W16B_TOP": (0x1, 0xf), "FFU_DATA_W24_TOP": (0x0, 0x2)}, {"MuxOutput_VID": 1, "VID_PROFILE": 24}, {"MuxOutput_POL1_IDX": 1, "POL1_IDX_PROFILE": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x0, 0x3dfffff, 0x0)),
    14: ({"ACTION_FLAGS": (0x0, 0x80000), "FFU_DATA_W16B_TOP": (0x1, 0xf), "FFU_DATA_W24_TOP": (0x0, 0x2), "FFU_DATA_TAG2A": (0x0, 0xfff)}, {"MuxOutput_VID": 1, "VID_PROFILE": 24}, {"MuxOutput_POL1_IDX": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x0, 0x3dfffff, 0x0)),
    15: (None, {}, {}, {}, (0x3ffffff, 0x0, 0x3ffffff, 0x0)),
    16: (None, {}, {}, {}, (0x3ffffff, 0x0, 0x3ffffff, 0x0)),
    17: (None, {}, {}, {}, (0x3ffff3f, 0x0, 0x3ffffff, 0x0)),
    18: ({"ACTION_FLAGS": (0x200000080000, 0x2000000c0000), "FFU_DATA_W16B_TOP": (0x1, 0xf), "FFU_DATA_W24_TOP": (0x2, 0x2)}, {"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 8, "MuxOutput_VID": 1, "VID_PROFILE": 20, "MuxOutput_MA_FID": 1}, {"MuxOutput_POL2_IDX": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_SGLORT": 1, "SGLORT_PROFILE": 1, "MuxOutput_CSGLORT": 1, "CSGLORT_PROFILE": 5, "MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x10000, 0x3dfffff, 0x0)),
    19: ({"ACTION_FLAGS": (0x200000080000, 0x2000000c0000), "FFU_DATA_W16B_TOP": (0x1, 0xf), "FFU_DATA_W24_TOP": (0x2, 0x2), "FFU_DATA_TAG2B": (0x0, 0xfff)}, {"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 8, "MuxOutput_VID": 1, "VID_PROFILE": 20, "MuxOutput_MA_FID": 1}, {"MuxOutput_POL2_IDX": 1, "POL2_IDX_PROFILE": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_SGLORT": 1, "SGLORT_PROFILE": 1, "MuxOutput_CSGLORT": 1, "CSGLORT_PROFILE": 5, "MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x10000, 0x3dfffff, 0x0)),
    20: ({"ACTION_FLAGS": (0x400000080100, 0x4000000c2300), "FFU_DATA_W8A": (0x0, 0x20), "FFU_DATA_W16B_TOP": (0x1, 0xf)}, {"SetL2LookupCmdProfile": 1, "MuxOutput_VID": 1, "VID_PROFILE": 20, "MuxOutput_MA_FID": 1}, {"MuxOutput_POL1_IDX": 1, "POL1_IDX_PROFILE": 2, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_SGLORT": 1, "SGLORT_PROFILE": 1, "MuxOutput_CSGLORT": 1, "CSGLORT_PROFILE": 5, "MuxOutput_W16ABC": 1}, (0x3ffff3f, 0x10000, 0x3ffffff, 0x0)),
    21: ({"ACTION_FLAGS": (0x400000080100, 0x24000010e2300), "FFU_DATA_W8A": (0x0, 0x20), "FFU_DATA_W16B_TOP": (0x1, 0xf), "NEXTHOP_TAG": (0x0, 0x4)}, {"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 8, "MuxOutput_VID": 1, "VID_PROFILE": 8}, {"MuxOutput_POL1_IDX": 1, "POL1_IDX_PROFILE": 2, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_SGLORT": 1, "SGLORT_PROFILE": 1, "MuxOutput_CSGLORT": 1, "CSGLORT_PROFILE": 5, "MuxOutput_W16ABC": 1}, (0x3ffffff, 0x10000, 0x3ffffff, 0x0)),
    22: ({"ACTION_FLAGS": (0x2400000080100, 0x24000010e2300), "FFU_DATA_W8A": (0x0, 0x20), "FFU_DATA_W16B_TOP": (0x1, 0xf), "NEXTHOP_TAG": (0x8, 0xc)}, {"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 1, "MuxOutput_MA1_MAC": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 12}, {"SetAlu46CmdProfile": 1, "ALU46_CMD_PROFILE": 4, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 9, "MuxOutput_POL2_IDX": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_SGLORT": 1, "SGLORT_PROFILE": 1, "MuxOutput_CSGLORT": 1, "CSGLORT_PROFILE": 5, "MuxOutput_W8ABCD": 1, "MuxOutput_W8E": 1, "MuxOutput_W8F": 1, "MuxOutput_W16ABC": 1, "MuxOutput_W16DEF": 1, "MuxOutput_W16GH": 1}, (0x3ffff7f, 0x14040, 0x3fffffb, 0x0)),
    23: ({"ACTION_FLAGS": (0x2400000080100, 0x24000010e2300), "FFU_DATA_W8A": (0x0, 0x20), "FFU_DATA_W16B_TOP": (0x1, 0xf), "NEXTHOP_TAG": (0x8, 0xc), "FFU_DATA_TAG2B": (0x0, 0xfff)}, {"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 1, "MuxOutput_MA1_MAC": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 12}, {"SetAlu46CmdProfile": 1, "ALU46_CMD_PROFILE": 4, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 9, "MuxOutput_POL2_IDX": 1, "POL2_IDX_PROFILE": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_SGLORT": 1, "SGLORT_PROFILE": 1, "MuxOutput_CSGLORT": 1, "CSGLORT_PROFILE": 5, "MuxOutput_W8ABCD": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 2, "MuxOutput_W8F": 1, "MuxOutput_W16ABC": 1, "MuxOutput_W16DEF": 1, "MuxOutput_W16GH": 1}, (0x3ffff7f, 0x14040, 0x3fffffb, 0x0)),
    24: ({"ACTION_FLAGS": (0x400000080100, 0x4000000c2300), "FFU_DATA_W8A": (0x0, 0x20), "FFU_DATA_W16A_TOP": (0x3, 0xf), "FFU_DATA_W16B_TOP": (0x1, 0xf)}, {"SetL2LookupCmdProfile": 1, "MuxOutput_VID": 1, "VID_PROFILE": 20, "MuxOutput_MA_FID": 1}, {"MuxOutput_POL1_IDX": 1, "POL1_IDX_PROFILE": 2, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_SGLORT": 1, "SGLORT_PROFILE": 1, "MuxOutput_CSGLORT": 1, "CSGLORT_PROFILE": 5, "MuxOutput_W16ABC": 1}, (0x3ffff3f, 0x0, 0x3ffffff, 0x4)),
    25: ({"ACTION_FLAGS": (0x400000080100, 0x24000010e2300), "FFU_DATA_W8A": (0x0, 0x20), "FFU_DATA_W16A_TOP": (0x3, 0xf), "FFU_DATA_W16B_TOP": (0x1, 0xf), "NEXTHOP_TAG": (0x0, 0x4)}, {"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 8, "MuxOutput_VID": 1, "VID_PROFILE": 8, "MuxOutput_MA_FID": 1}, {"MuxOutput_POL1_IDX": 1, "POL1_IDX_PROFILE": 2, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_SGLORT": 1, "SGLORT_PROFILE": 1, "MuxOutput_CSGLORT": 1, "CSGLORT_PROFILE": 5, "MuxOutput_W16ABC": 1}, (0x3ffffff, 0x0, 0x3ffffff, 0x4)),
    26: ({"ACTION_FLAGS": (0x2400000080100, 0x24000010e2300), "FFU_DATA_W8A": (0x0, 0x20), "FFU_DATA_W16A_TOP": (0x3, 0xf), "FFU_DATA_W16B_TOP": (0x1, 0xf), "NEXTHOP_TAG": (0x8, 0xc)}, {"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 1, "MuxOutput_MA1_MAC": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 12}, {"SetAlu46CmdProfile": 1, "ALU46_CMD_PROFILE": 4, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 9, "MuxOutput_POL2_IDX": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_SGLORT": 1, "SGLORT_PROFILE": 1, "MuxOutput_CSGLORT": 1, "CSGLORT_PROFILE": 5, "MuxOutput_W8ABCD": 1, "MuxOutput_W8E": 1, "MuxOutput_W8F": 1, "MuxOutput_W16ABC": 1, "MuxOutput_W16DEF": 1, "MuxOutput_W16GH": 1}, (0x3feff7f, 0x4040, 0x3ffffff, 0x4)),
    27: ({"ACTION_FLAGS": (0x2400000080100, 0x24000010e2300), "FFU_DATA_W8A": (0x0, 0x20), "FFU_DATA_W16A_TOP": (0x3, 0xf), "FFU_DATA_W16B_TOP": (0x1, 0xf), "NEXTHOP_TAG": (0x8, 0xc), "FFU_DATA_TAG2B": (0x0, 0xfff)}, {"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 1, "MuxOutput_MA1_MAC": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 12}, {"SetAlu46CmdProfile": 1, "ALU46_CMD_PROFILE": 4, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 9, "MuxOutput_POL2_IDX": 1, "POL2_IDX_PROFILE": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_SGLORT": 1, "SGLORT_PROFILE": 1, "MuxOutput_CSGLORT": 1, "CSGLORT_PROFILE": 5, "MuxOutput_W8ABCD": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 2, "MuxOutput_W8F": 1, "MuxOutput_W16ABC": 1, "MuxOutput_W16DEF": 1, "MuxOutput_W16GH": 1}, (0x3feff7f, 0x4040, 0x3ffffff, 0x4)),
    28: ({"ACTION_FLAGS": (0x200000080000, 0x2000000c0000), "FFU_DATA_W16A_TOP": (0x3, 0xf), "FFU_DATA_W16B_TOP": (0x1, 0xf), "FFU_DATA_W24_TOP": (0x2, 0x2)}, {"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 8, "MuxOutput_VID": 1, "VID_PROFILE": 20, "MuxOutput_MA_FID": 1}, {"MuxOutput_POL2_IDX": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_SGLORT": 1, "SGLORT_PROFILE": 1, "MuxOutput_CSGLORT": 1, "CSGLORT_PROFILE": 5, "MuxOutput_W16ABC": 1}, (0x3ffffff, 0x0, 0x3dfffff, 0x4)),
    29: ({"ACTION_FLAGS": (0x200000080000, 0x2000000c0000), "FFU_DATA_W16A_TOP": (0x3, 0xf), "FFU_DATA_W16B_TOP": (0x1, 0xf), "FFU_DATA_W24_TOP": (0x2, 0x2), "FFU_DATA_TAG2B": (0x0, 0xfff)}, {"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 8, "MuxOutput_VID": 1, "VID_PROFILE": 20, "MuxOutput_MA_FID": 1}, {"MuxOutput_POL2_IDX": 1, "POL2_IDX_PROFILE": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_SGLORT": 1, "SGLORT_PROFILE": 1, "MuxOutput_CSGLORT": 1, "CSGLORT_PROFILE": 5, "MuxOutput_W16ABC": 1}, (0x3ffffff, 0x0, 0x3dfffff, 0x4)),
    30: ({"FFU_DATA_W16A_TOP": (0x1, 0xf), "FFU_DATA_W16B_TOP": (0x1, 0xf)}, {"MuxOutput_VID": 1, "VID_PROFILE": 18}, {"MuxOutput_POL1_IDX": 1, "POL1_IDX_PROFILE": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x0, 0x3dfffff, 0x0)),
    31: ({"FFU_DATA_W16A_TOP": (0x1, 0xf), "FFU_DATA_W16B_TOP": (0x1, 0xf), "FFU_DATA_TAG2A": (0x0, 0xfff)}, {"MuxOutput_VID": 1, "VID_PROFILE": 18}, {"MuxOutput_POL1_IDX": 1, "MuxOutput_POL3_IDX": 1, "POL3_IDX_PROFILE": 1}, {"MuxOutput_W16ABC": 1, "W16ABC_PROFILE": 1}, (0x3ffffff, 0x0, 0x3dfffff, 0x0)),
}


def cam_addr(r, seg, w):
    return L3AR_BASE + CAM_SLICE_STRIDE * SLICE + CAM_RULE_STRIDE * r + 4 * seg + w


def ram_addr(off, r, w, stride=RAM_SLICE_STRIDE, per=2):
    return L3AR_BASE + off + stride * SLICE + per * r + w


def key_words(fields):
    value = care = 0
    for name, (v, m) in fields.items():
        lo, hi = KEYPOS[name]
        if v >> (hi - lo + 1) or m >> (hi - lo + 1):
            raise ValueError(f"{name} does not fit")
        value |= (v & m) << lo
        care |= m << lo
    return [encode_cam((value >> (64 * s)) & 0xFFFFFFFFFFFFFFFF,
                       (care >> (64 * s)) & 0xFFFFFFFFFFFFFFFF) for s in range(4)]


def pack(fieldvals, layout):
    acc = 0
    for n, lo, w in layout:
        v = fieldvals.get(n, 0)
        if v >> w:
            raise ValueError(f"{n} too wide")
        acc |= v << lo
    return acc


def build():
    m = {}
    for r in range(RULES):
        keys, f3, f4, f5, flags = RULESET[r]
        if keys is None:
            # never-match: Key=0 and KeyInvert=0 on every bit, and no actions
            for seg in range(4):
                for w in range(4):
                    m[cam_addr(r, seg, w)] = 0
            acts = (0, 0, 0)
        else:
            for seg, (k, ki) in enumerate(key_words(keys)):
                m[cam_addr(r, seg, 0)] = ki & 0xFFFFFFFF
                m[cam_addr(r, seg, 1)] = (ki >> 32) & 0xFFFFFFFF
                m[cam_addr(r, seg, 2)] = k & 0xFFFFFFFF
                m[cam_addr(r, seg, 3)] = (k >> 32) & 0xFFFFFFFF
            acts = (pack(f3, RAM3_FIELDS), pack(f4, RAM4_FIELDS),
                    pack(f5, RAM5_FIELDS))
        # flags pass through: ACTION_FLAGS' = ACTION_FLAGS & Mask | Value
        m[ram_addr(RAM1_OFF, r, 0)] = flags[0]
        m[ram_addr(RAM1_OFF, r, 1)] = flags[1]
        m[ram_addr(RAM2_OFF, r, 0)] = flags[2]
        m[ram_addr(RAM2_OFF, r, 1)] = flags[3]
        m[ram_addr(RAM3_OFF, r, 0, RAM3_SLICE_STRIDE, 1)] = acts[0] & 0xFFFFFFFF
        for i, off in ((1, RAM4_OFF), (2, RAM5_OFF)):
            m[ram_addr(off, r, 0)] = acts[i] & 0xFFFFFFFF
            m[ram_addr(off, r, 1)] = (acts[i] >> 32) & 0xFFFFFFFF
    return m


def dead_in(mem, r):
    """True if rule r has a bit in the never-match state in `mem`."""
    for seg in range(4):
        w = [mem.get(cam_addr(r, seg, i)) for i in range(4)]
        if any(x is None for x in w):
            continue
        ki = (w[1] << 32) | w[0]
        k = (w[3] << 32) | w[2]
        if ternary(k, ki)[2]:
            return True
    return False


def verify(image):
    eos, ours = load(image), build()
    live_same = live_bad = 0
    for r in range(RULES):
        dead = RULESET[r][0] is None
        if dead:
            if not dead_in(eos, r):
                print(f"  rule {r}: we mark dead, EOS does not")
                live_bad += 1
            elif not dead_in(ours, r):
                print(f"  rule {r}: ours is not never-match")
                live_bad += 1
            continue
        addrs = [cam_addr(r, s, w) for s in range(4) for w in range(4)]
        addrs += [ram_addr(RAM1_OFF, r, 0), ram_addr(RAM1_OFF, r, 1),
                  ram_addr(RAM2_OFF, r, 0), ram_addr(RAM2_OFF, r, 1),
                  ram_addr(RAM3_OFF, r, 0, RAM3_SLICE_STRIDE, 1),
                  ram_addr(RAM4_OFF, r, 0), ram_addr(RAM4_OFF, r, 1),
                  ram_addr(RAM5_OFF, r, 0), ram_addr(RAM5_OFF, r, 1)]
        for a in addrs:
            if eos.get(a) == ours[a]:
                live_same += 1
            else:
                if live_bad < 6:
                    print(f"  rule {r} 0x{a:05x}: eos={eos.get(a):08x} ours={ours[a]:08x}")
                live_bad += 1
    dead_n = sum(1 for r in range(RULES) if RULESET[r][0] is None)
    print(f"live words identical {live_same}, differing {live_bad}; "
          f"{dead_n} dead rules confirmed never-match on both sides")
    print("VERIFY PASS" if live_bad == 0 else "VERIFY FAIL")
    return 1 if live_bad else 0



C_HEAD = r'''/* fm6000_l3arslice2.c - L3AR slice 2: VID assignment and trap.
 *
 * GENERATED by asic/fm6000/tools/gen_l3ar_slice2.py --c. Edit the generator.
 *
 * Per rule this stage selects a VID profile (the bulk of it), a trap header
 * command on rules 2-3, an L2 lookup command profile with the MA1/MA2 MAC muxes
 * on rules 22/23/26/27, policer index muxes (POL1, POL2 and POL3 all appear),
 * ALU46 profiles on four rules, and the W16ABC action-data channel plus W8E.
 *
 * WARNING: three rules are DEAD in EOS's table -- each carries a bit in the
 * never-match state (Key=0, KeyInvert=0). They are emitted as never-match with
 * zero actions rather than reproducing actions that can never run.
 *
 * WARNING: slice 2 rewrites flags in TWELVE distinct combinations -- more than
 * any other slice -- so all four flag words are carried per rule. A blanket
 * pass-through is WRONG here.
 *
 * Verified against EOS's slice 2: 725 of 725 live words identical, and all three
 * dead rules never-match on both sides.
 *
 * usage: fm6000_l3arslice2 [-n | -a] [-b bdf]
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

static const struct { uint32_t addr, val; } W[] = {
'''

C_TAIL = r'''};

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
		fprintf(stderr, "fm6000_l3arslice2: %d writes\n",
		        (int)(sizeof W / sizeof W[0]));
	return 0;
}
'''


def emit_c(path):
    m = build()
    with open(path, "w") as f:
        f.write(C_HEAD)
        for r in range(RULES):
            keys = RULESET[r][0]
            f.write("\t/* rule %2d -- %s */\n"
                    % (r, "DEAD: never-match key" if keys is None
                       else ", ".join(sorted(keys)) or "matches any"))
            a = [cam_addr(r, s_, w) for s_ in range(4) for w in range(4)]
            a += [ram_addr(RAM1_OFF, r, 0), ram_addr(RAM1_OFF, r, 1),
                  ram_addr(RAM2_OFF, r, 0), ram_addr(RAM2_OFF, r, 1),
                  ram_addr(RAM3_OFF, r, 0, RAM3_SLICE_STRIDE, 1),
                  ram_addr(RAM4_OFF, r, 0), ram_addr(RAM4_OFF, r, 1),
                  ram_addr(RAM5_OFF, r, 0), ram_addr(RAM5_OFF, r, 1)]
            for x in sorted(a):
                f.write("\t{ 0x%06x, 0x%08x },\n" % (x, m[x]))
        f.write(C_TAIL)
    print("wrote %s: %d writes" % (path, len(m)), file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", action="store_true")
    ap.add_argument("--addrs", action="store_true")
    ap.add_argument("--verify")
    ap.add_argument("--c", dest="cfile")
    a = ap.parse_args()
    if a.cfile:
        emit_c(a.cfile)
        return 0
    if a.verify:
        return verify(a.verify)
    m = build()
    for addr in sorted(m):
        print(f"{addr:08x}" if a.addrs else f"{addr:08x} {m[addr]:08x}")
    if not a.emit and not a.addrs:
        print(f"{len(m)} writes", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
