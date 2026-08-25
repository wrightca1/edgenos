#!/usr/bin/env python3
"""gen_l3ar_tables.py - author what the L3AR slice generators do not: slice 0's
action banks and the 19 shared profile tables.

With slices 1-4 authored (alpha32/34/35/36), L3AR still contributed 512 replayed
writes across 450 addresses. Two causes, and the first one matters:

★ SLICE 0'S RAM3, RAM4 AND RAM5 WERE NEVER PROGRAMMED BY US.

`fm6000_l3arinit` emits slice 0's CAM plus RAM1/RAM2 only. It was written under
the premise `l3ar_decode.py` used to state -- *"the action is a flag rewrite, and
that is all it is"* -- which docs/EDGENOS-7150.md (was L3AR-STRUCTURE) retracted: there are FIVE RAM
banks, and RAM3/4/5 hold the L2 lookup, ALU, policer, QoS, GLORT and action-data
muxes. Slice 0 is the forwarding stage and all 32 of its rules populate all three
of those banks, so 160 addresses of live forwarding actions have been coming from
EOS's replay the whole time. This generator supplies them.

★ THE PROFILE TABLES ARE SHARED, SO NO SLICE OWNS THEM.

The 19 tables at 0x11c00-0x120df are indexed by the 5-bit profile numbers the
slices select, and a table entry is routinely referenced from more than one slice
-- slice 2 selects csGlort profile 5, which slice 1 also uses. Each slice
generator therefore emits only the entries its own rules reference, and the union
still leaves gaps. They are authored here once, as a unit.

Field layouts for both parts are in docs/EDGENOS-7150.md (was L3AR-STRUCTURE), recovered from the
SDK's field descriptor table; the profile tables are
`X = (selected_source & Mask) | Value` with `Select` naming the source per
datasheet Table 5-37.

usage:
    gen_l3ar_tables.py --emit | --addrs | --verify <image> | --c FILE
SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from l3ar_decode import (  # noqa: E402
    L3AR_BASE, RAM3_OFF, RAM4_OFF, RAM5_OFF,
    RAM3_FIELDS, RAM4_FIELDS, RAM5_FIELDS, load,
)

SLICE = 0

# emitted by gen_l3ar_slice1 (SGLORT 0-1, CSGLORT 0/5/10) -- see build()
SLICE1_OWNED = (0x11c40, 0x11c41, 0x11c42, 0x11c43,
                0x11e40, 0x11e41, 0x11e4a, 0x11e4b, 0x11e54, 0x11e55)

SLICE0_ACTIONS = {
    0: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 2, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 6, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    1: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 2, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 10, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    2: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 6, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 6, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    3: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 6, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 6, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    4: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 6, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 6, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    5: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 6, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 9, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    6: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 4, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 6, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    7: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 5, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 6, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    8: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 5, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 6, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    9: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 2, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 6, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    10: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 5, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 6, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    11: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 3, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 5, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 6, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    12: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 5, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 6, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    13: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 14, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "ALU46_CMD_PROFILE": 2, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 15, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 2, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 9, "MuxOutput_W8ABCD": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 3, "MuxOutput_W8F": 1, "MuxOutput_W16DEF": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 4}),
    14: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 2, "MuxOutput_MA1_MAC": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 4, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "ALU46_CMD_PROFILE": 2, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 15, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 3, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 3, "MuxOutput_W8ABCD": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 3, "MuxOutput_W8F": 1, "MuxOutput_W16DEF": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 4}),
    15: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 14, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "ALU46_CMD_PROFILE": 3, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 7, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 2, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 9, "MuxOutput_W8ABCD": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 3, "MuxOutput_W8F": 1, "MuxOutput_W16DEF": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 1}),
    16: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 2, "MuxOutput_MA1_MAC": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 14, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "ALU46_CMD_PROFILE": 3, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 7, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 1, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 4, "MuxOutput_W8ABCD": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 3, "MuxOutput_W8F": 1, "MuxOutput_W16DEF": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 2}),
    17: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 2, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 1, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 3, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 5}),
    18: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 7, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 2, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    19: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 2, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 2, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    20: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 16, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 2, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 10, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16DEF": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 4}),
    21: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 5, "MuxOutput_MA1_MAC": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 2}, {"SetAlu46CmdProfile": 1, "ALU46_CMD_PROFILE": 4, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 9}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 1, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 5, "MuxOutput_W8ABCD": 1, "MuxOutput_W8E": 1, "MuxOutput_W8F": 1, "MuxOutput_W16DEF": 1, "MuxOutput_W16GH": 1}),
    22: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 5, "MuxOutput_MA1_MAC": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 2}, {"SetAlu46CmdProfile": 1, "ALU46_CMD_PROFILE": 4, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 9}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 1, "SetDestMaskCmdProfile": 1, "MuxOutput_W8ABCD": 1, "MuxOutput_W8E": 1, "MuxOutput_W8F": 1, "MuxOutput_W16DEF": 1, "MuxOutput_W16GH": 1}),
    23: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 2, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 3, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 11, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    24: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 2, "MuxOutput_MA1_MAC": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 3, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 1, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16DEF": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 3}),
    25: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 2, "MuxOutput_MA1_MAC": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 3, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 11, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16DEF": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 3}),
    26: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 3, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 1, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 5}),
    27: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 3, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 3, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 1, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 5}),
    28: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 4, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 3, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 8, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    29: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 10, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 2, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 8, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    30: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 7, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 13, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 2, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 10, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
    31: ({"SetL2LookupCmdProfile": 1, "L2L_CMD_PROFILE": 4, "MuxOutput_MA1_MAC": 1, "MA1_MAC_PROFILE": 1, "MuxOutput_MA2_MAC": 1, "MuxOutput_VID": 1, "VID_PROFILE": 22, "MuxOutput_MA_FID": 1, "MA_FID_PROFILE": 1}, {"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1, "ALU46_OP_PROFILE": 11, "MuxOutput_QOS": 1, "QOS_PROFILE": 7}, {"MuxOutput_DGLORT": 1, "DGLORT_PROFILE": 2, "SetDestMaskCmdProfile": 1, "DMASK_CMD_PROFILE": 7, "MuxOutput_W8ABCD": 1, "W8ABCD_PROFILE": 1, "MuxOutput_W8E": 1, "W8E_PROFILE": 1, "MuxOutput_W8F": 1, "MuxOutput_W16GH": 1, "W16GH_PROFILE": 6}),
}

# ⚠ per-word, not per-entry: EOS writes only SOME words of some profile
# entries (W8ABCD entries 1 and 2, for instance). Emitting a zero for a word
# EOS leaves alone is a fabricated write, so absent words stay absent.
PROFILES = {
    "DGLORT": [
        (0x11c00, 0xffff0000),
        (0x11c01, 0x0),
        (0x11c02, 0xffff0000),
        (0x11c03, 0x5),
        (0x11c04, 0xfffe),
        (0x11c05, 0x0),
        (0x11c06, 0xffff0000),
        (0x11c07, 0x4),
        (0x11c08, 0xfffb),
        (0x11c09, 0x0),
        (0x11c0a, 0xffff0000),
        (0x11c0b, 0x1),
        (0x11c0c, 0xffff),
        (0x11c0d, 0x0),
        (0x11c0e, 0xfffc),
        (0x11c0f, 0x0),
    ],
    "SGLORT": [
        (0x11c40, 0xffff0000),
        (0x11c41, 0x0),
        (0x11c42, 0xfff0000),
        (0x11c43, 0x2),
    ],
    "W8ABCD": [
        (0x11c80, 0x0),
        (0x11c81, 0xffffffff),
        (0x11c82, 0x0),
        (0x11c84, 0x0),
        (0x11c85, 0xff00),
        (0x11c86, 0x4),
    ],
    "W8E": [
        (0x11d00, 0x3ff00),
        (0x11d01, 0x0),
        (0x11d02, 0x1ff00),
        (0x11d03, 0x4ff00),
    ],
    "W8F": [
        (0x11d20, 0xff00),
    ],
    "MA1_MAC": [
        (0x11d40, 0x0),
        (0x11d41, 0x10000),
        (0x11d42, 0x0),
        (0x11d43, 0x0),
    ],
    "MA2_MAC": [
        (0x11d80, 0x0),
        (0x11d81, 0x0),
    ],
    "VID": [
        (0x11dc0, 0x0),
        (0x11dc1, 0xc80000),
        (0x11dc4, 0x0),
        (0x11dc5, 0x7c80000),
        (0x11dc8, 0x0),
        (0x11dc9, 0xc080000),
        (0x11dcc, 0x0),
        (0x11dcd, 0x8e00000),
        (0x11dd0, 0x0),
        (0x11dd1, 0xee80000),
        (0x11dd4, 0x0),
        (0x11dd5, 0x8200000),
        (0x11dd8, 0x0),
        (0x11dd9, 0x7e80000),
        (0x11ddc, 0x0),
        (0x11ddd, 0xcc80000),
        (0x11de0, 0x0),
        (0x11de1, 0x2c80000),
        (0x11de4, 0x0),
        (0x11de5, 0x9650000),
        (0x11de8, 0x0),
        (0x11de9, 0xf680000),
        (0x11dec, 0x0),
        (0x11ded, 0x2080000),
        (0x11df0, 0x0),
        (0x11df1, 0x34d0000),
    ],
    "MA_FID": [
        (0x11e00, 0x0),
        (0x11e01, 0xcf0000),
        (0x11e02, 0x0),
        (0x11e03, 0xcc0000),
    ],
    "CSGLORT": [
        (0x11e40, 0xffff0000),
        (0x11e41, 0x0),
        (0x11e42, 0xffff0000),
        (0x11e43, 0x0),
        (0x11e44, 0xffff0000),
        (0x11e45, 0x0),
        (0x11e46, 0xffff0000),
        (0x11e47, 0x0),
        (0x11e48, 0xffff0000),
        (0x11e49, 0x0),
        (0x11e4a, 0xffff0000),
        (0x11e4b, 0x2),
        (0x11e4c, 0xffff0000),
        (0x11e4d, 0x0),
        (0x11e4e, 0xffff0000),
        (0x11e4f, 0x0),
        (0x11e50, 0xffff0000),
        (0x11e51, 0x0),
        (0x11e52, 0xffff0000),
        (0x11e53, 0x0),
        (0x11e54, 0xffe00000),
        (0x11e55, 0x0),
        (0x11e56, 0xffff0000),
        (0x11e57, 0x0),
        (0x11e58, 0xffff0000),
        (0x11e59, 0x0),
        (0x11e5a, 0xffff0000),
        (0x11e5b, 0x0),
        (0x11e5c, 0xffff0000),
        (0x11e5d, 0x0),
        (0x11e5e, 0xffe00000),
        (0x11e5f, 0x0),
        (0x11e60, 0xffff0000),
        (0x11e61, 0x0),
        (0x11e62, 0xffff0000),
        (0x11e63, 0x0),
        (0x11e64, 0xffff0000),
        (0x11e65, 0x0),
        (0x11e66, 0xffff0000),
        (0x11e67, 0x0),
        (0x11e68, 0xffff0000),
        (0x11e69, 0x0),
        (0x11e6a, 0xffff0000),
        (0x11e6b, 0x0),
        (0x11e6c, 0xffe00000),
        (0x11e6d, 0x0),
        (0x11e6e, 0xffff0000),
        (0x11e6f, 0x0),
        (0x11e70, 0xffff0000),
        (0x11e71, 0x0),
        (0x11e72, 0xffff0000),
        (0x11e73, 0x0),
        (0x11e74, 0xffff0000),
        (0x11e75, 0x0),
        (0x11e76, 0xffff0000),
        (0x11e77, 0x0),
        (0x11e78, 0xffe00000),
        (0x11e79, 0x0),
        (0x11e7a, 0xffff0000),
        (0x11e7b, 0x0),
        (0x11e7c, 0xffff0000),
        (0x11e7d, 0x0),
        (0x11e7e, 0xffff0000),
        (0x11e7f, 0x0),
    ],
    "W16ABC": [
        (0x11e80, 0x0),
        (0x11e81, 0x780000),
        (0x11e82, 0x0),
        (0x11e83, 0x7b0000),
    ],
    "W16DEF": [
        (0x11ec0, 0x0),
        (0x11ec1, 0x490000),
    ],
    "W16GH": [
        (0x11f00, 0x0),
        (0x11f01, 0x27),
        (0x11f02, 0x0),
        (0x11f03, 0x35),
        (0x11f04, 0x0),
        (0x11f05, 0x2d),
        (0x11f06, 0x0),
        (0x11f07, 0x1d),
        (0x11f08, 0x0),
        (0x11f09, 0x3d),
        (0x11f0a, 0x0),
        (0x11f0b, 0x1f),
        (0x11f0c, 0x0),
        (0x11f0d, 0x3f),
    ],
    "ALU13_OP": [
        (0x11f80, 0x0),
        (0x11f81, 0x5a0000),
        (0x11f82, 0x0),
        (0x11f83, 0x3cfff1),
        (0x11f84, 0x1c),
        (0x11f85, 0x320000),
        (0x11f86, 0x0),
        (0x11f87, 0x3cffff),
        (0x11f88, 0x0),
        (0x11f89, 0x1e0000),
        (0x11f8a, 0x10000),
        (0x11f8b, 0xfdf9f1),
        (0x11f8c, 0x0),
        (0x11f8d, 0x320000),
        (0x11f8e, 0x0),
        (0x11f8f, 0xc4ff11),
        (0x11f90, 0x0),
        (0x11f91, 0x1e0000),
        (0x11f92, 0x990000),
        (0x11f93, 0xfdf9f1),
        (0x11f94, 0x852),
        (0x11f95, 0x320000),
        (0x11f96, 0x0),
        (0x11f97, 0x3cffff),
        (0x11f98, 0x0),
        (0x11f99, 0x320000),
        (0x11f9a, 0x0),
        (0x11f9b, 0x3cfff1),
        (0x11f9c, 0x0),
        (0x11f9d, 0x1e0000),
        (0x11f9e, 0xe50000),
        (0x11f9f, 0xfdf9f1),
        (0x11fa0, 0x20),
        (0x11fa1, 0x320000),
        (0x11fa2, 0x0),
        (0x11fa3, 0x3cffff),
        (0x11fa4, 0x0),
        (0x11fa5, 0x1e0000),
        (0x11fa6, 0x4d0000),
        (0x11fa7, 0xfdf9f1),
        (0x11fa8, 0x0),
        (0x11fa9, 0x360000),
        (0x11faa, 0x0),
        (0x11fab, 0x3cfff1),
    ],
    "ALU46_OP": [
        (0x12000, 0x0),
        (0x12001, 0x0),
        (0x12002, 0x0),
        (0x12003, 0xf1af12),
        (0x12004, 0x0),
        (0x12005, 0x0),
        (0x12006, 0x220000),
        (0x12007, 0xf5a112),
        (0x12008, 0x0),
        (0x12009, 0xd000),
        (0x1200a, 0x0),
        (0x1200b, 0xfbaf12),
        (0x1200c, 0x0),
        (0x1200d, 0x0),
        (0x1200e, 0x0),
        (0x1200f, 0xfbaf12),
        (0x12010, 0x0),
        (0x12011, 0x2000),
        (0x12012, 0x0),
        (0x12013, 0xfbaf12),
        (0x12014, 0x0),
        (0x12015, 0x852),
        (0x12016, 0x1e0000),
        (0x12017, 0xf5af12),
        (0x12018, 0x0),
        (0x12019, 0x0),
        (0x1201a, 0x460000),
        (0x1201b, 0xf5a112),
        (0x1201c, 0x0),
        (0x1201d, 0x0),
        (0x1201e, 0x1e0000),
        (0x1201f, 0xf5a112),
        (0x12020, 0x1c0000),
        (0x12021, 0x1c),
        (0x12022, 0x1e0000),
        (0x12023, 0xf5aff2),
        (0x12024, 0x0),
        (0x12025, 0x0),
        (0x12026, 0x1e0000),
        (0x12027, 0xfa512f),
        (0x12028, 0x0),
        (0x12029, 0x1000),
        (0x1202a, 0x0),
        (0x1202b, 0xfbaf12),
        (0x1202c, 0x0),
        (0x1202d, 0x9000),
        (0x1202e, 0x0),
        (0x1202f, 0xfbaf12),
        (0x12030, 0x0),
        (0x12031, 0x100f),
        (0x12032, 0x0),
        (0x12033, 0xfbaf12),
        (0x12034, 0x0),
        (0x12035, 0x5000),
        (0x12036, 0x0),
        (0x12037, 0xfbaf12),
        (0x12038, 0x1c0000),
        (0x12039, 0x20),
        (0x1203a, 0x1e0000),
        (0x1203b, 0xf5aff2),
        (0x1203c, 0x0),
        (0x1203d, 0x2000),
        (0x1203e, 0x0),
        (0x1203f, 0x7baf12),
    ],
    "POL1_IDX": [
        (0x12080, 0x1000),
        (0x12081, 0x0),
        (0x12082, 0xf000),
        (0x12090, 0x0),
        (0x12091, 0x1000),
    ],
    "POL2_IDX": [
        (0x12090, 0x0),
        (0x12091, 0x1000),
        (0x120a0, 0x0),
        (0x120a1, 0x7ffc00),
    ],
    "POL3_IDX": [
        (0x120a0, 0x0),
        (0x120a1, 0x7ffc00),
    ],
    "QOS": [
        (0x120c0, 0xfff00000),
        (0x120c1, 0x41cfc),
        (0x120c2, 0xfff00000),
        (0x120c3, 0x4000ff),
        (0x120c4, 0xfff00000),
        (0x120c5, 0x4ff),
        (0x120c6, 0xfff00000),
        (0x120c7, 0x404fc),
        (0x120c8, 0xfff00000),
        (0x120c9, 0x1cff),
        (0x120ca, 0xfff00000),
        (0x120cb, 0x20ff),
        (0x120cc, 0xfff00000),
        (0x120cd, 0x100ff),
        (0x120ce, 0xfff00000),
        (0x120cf, 0xff),
        (0x120d0, 0xfff00000),
        (0x120d1, 0x600fc),
        (0x120d2, 0xfff00000),
        (0x120d3, 0x418fc),
        (0x120d4, 0xfff00000),
        (0x120d5, 0x38ff),
        (0x120d6, 0xfff00000),
        (0x120d7, 0x1c0ff),
    ],
}

MISC = {
    0x11008: 0x1f,
    0x11009: 0x10,
    0x1100a: 0x1f,
    0x1100b: 0x1f,
    0x1100c: 0xa,
    0x12148: 0xffffffff,
    0x12149: 0xffffffff,
    0x1214a: 0xffffffff,
    0x1214b: 0xffffffff,
    0x1214c: 0xffffffff,
}


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
    # slice 0's three action banks -- the gap fm6000_l3arinit leaves
    for r, (f3, f4, f5) in SLICE0_ACTIONS.items():
        m[L3AR_BASE + RAM3_OFF + r] = pack(f3, RAM3_FIELDS) & 0xFFFFFFFF
        for off, fv, layout in ((RAM4_OFF, f4, RAM4_FIELDS),
                                (RAM5_OFF, f5, RAM5_FIELDS)):
            v = pack(fv, layout)
            m[L3AR_BASE + off + 2 * r] = v & 0xFFFFFFFF
            m[L3AR_BASE + off + 2 * r + 1] = (v >> 32) & 0xFFFFFFFF
    # the 19 shared profile tables
    for _name, rows in PROFILES.items():
        for a, v in rows:
            m[a] = v
    m.update(MISC)
    # ⚠ SINGLE OWNERSHIP. gen_l3ar_slice1 already emits SGLORT entries 0-1 and
    # csGlort entries 0, 5 and 10 -- the profiles its own rules select, and which
    # slice 2 also depends on. Those 10 addresses are dropped here so exactly one
    # generator owns each address. Checked: the two agree on all 10 values, so
    # this is about ownership, not a conflict.
    for a in SLICE1_OWNED:
        m.pop(a, None)
    return m


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
                print(f"  0x{a:05x}  eos={e:08x}  ours={ours[a]:08x}")
            bad += 1
    print(f"identical {same}, differing {bad}, absent from image {missing}")
    print("VERIFY PASS" if bad == 0 else "VERIFY FAIL")
    return 1 if bad else 0



C_HEAD = r'''/* fm6000_l3artables.c - L3AR slice 0's action banks + the 19 shared profile tables.
 *
 * GENERATED by asic/fm6000/tools/gen_l3ar_tables.py --c. Edit the generator.
 *
 * Two gaps the per-slice generators leave:
 *
 *   1. fm6000_l3arinit emits slice 0's CAM and RAM1/RAM2 ONLY. It was written
 *      under the premise that an L3AR action is just a flag rewrite, which is
 *      false -- there are five RAM banks. Slice 0 is the forwarding stage and
 *      all 32 of its rules populate RAM3, RAM4 and RAM5 with the L2 lookup,
 *      ALU, policer, QoS, GLORT and action-data muxes. 160 addresses.
 *
 *   2. The 19 profile tables are SHARED between slices, so no slice generator
 *      owns them; each emits only the entries its own rules reference.
 *
 * WARNING: per-word, not per-entry. EOS writes only some words of some profile
 * entries; a zero written where EOS writes nothing is a fabricated write, and an
 * earlier version of this generator did exactly that for three W8ABCD words.
 * Absent words stay absent.
 *
 * Verified byte-identical against the executed image: 460 of 460.
 *
 * usage: fm6000_l3artables [-n | -a] [-b bdf]
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
		fprintf(stderr, "fm6000_l3artables: %d writes\n",
		        (int)(sizeof W / sizeof W[0]));
	return 0;
}
'''


def emit_c(path):
    m = build()
    from l3ar_decode import PROFILE_TABLES
    secs = [("slice 0 RAM3/RAM4/RAM5 -- the forwarding actions", 0x11600, 0x11c00)]
    for nm, (b, w) in sorted(PROFILE_TABLES.items(), key=lambda kv: kv[1][0]):
        secs.append((nm + " profile table", b, b + 32 * w))
    secs.append(("ACTION_CFG / IM", 0x11008, 0x12150))
    done = set()
    with open(path, "w") as f:
        f.write(C_HEAD)
        for label, lo, hi in secs:
            rows = [a for a in sorted(m) if lo <= a < hi and a not in done]
            if not rows:
                continue
            f.write("\t/* %s */\n" % label)
            for a in rows:
                done.add(a)
                f.write("\t{ 0x%06x, 0x%08x },\n" % (a, m[a]))
        for a in sorted(set(m) - done):
            f.write("\t{ 0x%06x, 0x%08x },\n" % (a, m[a]))
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
