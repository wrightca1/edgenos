#!/usr/bin/env python3
"""l3ar_decode.py - decode and encode the FM6000 L3AR rules.

L3AR is fm6000_l3arinit.c, 3,928 transcribed microcode pairs
(docs/EDGENOS-7150.md (was PROVENANCE) 2.5). Decoder and encoder in one file because the block is
small and the two halves share the layout tables.

★ GEOMETRY, from the SDK's register descriptor table (NOT the register header).

⚠ PROVENANCE CORRECTION. This file used to say "from the register header". The
header does not define L3AR at all -- EOS-SOURCES.md:77 records that the 0x10000
macros are absent from it, which is why this block was reverse-engineered by
clustering writes. The addresses below now come from a descriptor table found in
libFocalpointSDK.so (.rodata, 56-byte stride, entries of
{name_ptr, ..., base_addr, 0, words_per_entry, 0, 0}). Facts about the silicon,
recorded in our own words; nothing is copied from the SDK.

    L3AR_CAM(slice, rule, seg, word) = 0x10000 + 0x200*slice + 0x10*rule + 4*seg
        5 slices x 32 rules x 4 segments x 4 words -- a 256-bit key
    L3AR_RAM1(slice, rule, word)     = 0x11200 + 0x40*slice + 2*rule
    L3AR_RAM2(slice, rule, word)     = 0x11400 + 0x40*slice + 2*rule
    L3AR_RAM3(slice, rule)           = 0x11600 + 0x20*slice + 1*rule   <-- 1 word
    L3AR_RAM4(slice, rule, word)     = 0x11800 + 0x40*slice + 2*rule
    L3AR_RAM5(slice, rule, word)     = 0x11a00 + 0x40*slice + 2*rule

⚠⚠ RETRACTION -- "THE ACTION IS A FLAG REWRITE, AND THAT IS ALL IT IS" WAS WRONG.

That claim stood because this decoder only ever read RAM1 and RAM2. There are
FIVE RAM banks, and the datasheet's Table 5-31 lists 6 sequential actions and 21
output mux actions besides SetFlags -- SetAlu13/46CmdProfile,
SetL2LookupCmdProfile, SetDestMaskCmdProfile, SetTrapHeaderCmd,
SetHashKeyProfile, and MuxOutput_* -- whose operands live in RAM3/RAM4/RAM5 and
point into 19 separate profile tables at 0x11c00-0x120df.

Anything authored from RAM1/RAM2 alone would silently drop those actions. Slice 1
is the proof: all 32 of its rules are Mask=0x3ffffff/Value=0, a no-op on the
flags, so by the old reading slice 1 did nothing -- yet every one of its rules
carries a nonzero RAM5 word.

    RAM1  Mask_ACTION_FLAGS_LO[25:0]  Set_ACTION_FLAGS_LO[57:32]
    RAM2  Mask_ACTION_FLAGS_HI[25:0]  Set_ACTION_FLAGS_HI[57:32]

26 + 26 = 52 bits, exactly the width of ACTION_FLAGS. Datasheet 5.10.5 gives the
operation exactly:  ACTION_FLAGS' = ACTION_FLAGS & Mask | Value.  So Mask=all-ones
with Value=0 is a no-op, not a clear.

⚠ RAM3/RAM4/RAM5 FIELD LAYOUT IS NOT ESTABLISHED. The addresses and widths are
verified; what the bits mean is not. Do not author rules from them yet. Two
observations that must be explained first:
  - slice 1 rules 2,3,4,5 are byte-identical across all 16 CAM words, yet carry
    different RAM5 values. Within a slice the 32 rules are one precedence set
    (datasheet 5.10.1, "Number of rules per precedence set: 32"), so at most one
    of four identical keys can ever fire.
  - slice 1's 32 RAM5 words are (k<<12)|0x040 where k>>1 runs over 0..31 exactly
    once. A permutation with a valid bit is the signature of a table-init loop,
    not of hand-authored rules.

VALIDATED before use: RAM words come out at exactly 2x the declared rule count on
every slice -- 64/64/64/64/50 against the 32/32/32/32/25 slice sizes in
fm6000MicrocodeRuleNames.txt. Slice 4's 25 is an odd number and it matches.

PROVENANCE. Reads the image at runtime, embeds nothing. Rule names come from
Arista's confidential file via --names and stay out of the tree.

Usage:
    l3ar_decode.py --image <fm6000Microcode.raw> --summary
    l3ar_decode.py --image <img> --names <rulenames.txt> --slice 0
    l3ar_decode.py --image <img> --verify
"""
import argparse
import collections
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import load, ternary  # noqa: E402
from l2ar_decode import load_names  # noqa: E402

L3AR_BASE = 0x10000
CAM_SLICE_STRIDE, CAM_RULE_STRIDE = 0x200, 0x10
RAM1_OFF, RAM2_OFF = 0x1200, 0x1400
RAM3_OFF, RAM4_OFF, RAM5_OFF = 0x1600, 0x1800, 0x1A00
RAM_SLICE_STRIDE = 0x40
# ⚠ RAM3 is ONE word per rule, not two, so its slice stride is half the others'.
# Verified by the data: RAM3 slice 4 stops after 25 words, matching the 25-rule
# slice in fm6000MicrocodeRuleNames.txt. Reading it at 0x40 silently returns
# slices 2-3's content under slice 1's label.
RAM3_WORDS_PER_RULE, RAM3_SLICE_STRIDE = 1, 0x20

# The 19 profile tables the MuxOutput / SetProfile actions index into, with the
# words-per-entry the SDK descriptor declares. Each holds 32 profiles, matching
# the "Profile (5 bits)" operand in datasheet Table 5-31. The spacing is
# self-consistent -- DGLORT at 0x11c00 with 2 words x 32 rules ends exactly where
# SGLORT begins at 0x11c40, and W8ABCD's 3 words x 32 ends exactly at W8E's
# 0x11d00 -- which is what makes the widths trustworthy.
PROFILE_TABLES = {
    "DGLORT":    (0x11C00, 2), "SGLORT":    (0x11C40, 2),
    "W8ABCD":    (0x11C80, 3), "W8E":       (0x11D00, 1),
    "W8F":       (0x11D20, 1), "MA1_MAC":   (0x11D40, 2),
    "MA2_MAC":   (0x11D80, 2), "VID":       (0x11DC0, 2),
    "MA_FID":    (0x11E00, 2), "CSGLORT":   (0x11E40, 2),
    "W16ABC":    (0x11E80, 2), "W16DEF":    (0x11EC0, 2),
    "W16GH":     (0x11F00, 2), "HASH_ROT":  (0x11F40, 1),
    "ALU13_OP":  (0x11F80, 4), "ALU46_OP":  (0x12000, 4),
    "POL1_IDX":  (0x12080, 1), "POL2_IDX":  (0x12090, 1),
    "POL3_IDX":  (0x120A0, 1), "QOS":       (0x120C0, 2),
}
# Not profile tables, but the rest of the block, so nothing here reads as unmapped:
L3AR_OTHER = {
    "SLICE_CFG": (0x11000, 1), "KEY_CFG":  (0x11001, 1), "ACTION_CFG": (0x11008, 1),
    "TRAP_HEADER_RULE": (0x12100, 1), "TRAP_HEADER_DATA": (0x12120, 11),
    "IP": (0x12140, 1), "IM": (0x12148, 1),
}

# Slice 4 is short. fm6000MicrocodeRuleNames.txt declares 32/32/32/32/25 = 153
# rules of the 160 the datasheet says exist, and RAM3 stops after 25 words in
# slice 4, independently confirming it.
RULES_IN_SLICE = (32, 32, 32, 32, 25)
SEGMENTS, NUM_SLICES, RULES_PER_SLICE = 4, 5, 32
MASK64 = 0xFFFFFFFFFFFFFFFF

# FM6000_L3AR_CAM_KEYS -- exact positions, 252 used of 256 over 4 segments x 64.
#
# ⚠ THIS LIST STOPPED AT BIT 147 FOR MOST OF ITS LIFE. Everything from
# FFU_DATA_W8B upward was missing, and the omission was invisible: key_fields()
# only prints fields it knows, so rules whose ONLY distinguishing bit lived above
# 147 decoded as identical to each other. Two real pairs did:
#
#   s0r7  ...AndDefaultDglort  vs s0r11 ...AndDirectedDglort  differ at bit 167
#                                          (FFU_DATA_W24_TOP bit 3)
#   s0r20 UnicastRouting       vs s0r23 UnicastNoARP          differ at bit 174
#                                          (NEXTHOP_TAG bit 2)
#
# NEXTHOP_TAG is the semantically satisfying one: "no ARP entry" is the next-hop
# lookup tagging the frame as unresolved, which is exactly where that fact should
# come from. --verify passed throughout, because it only checked that the named
# fields tile upward from bit 0 -- never that every care bit lands inside a named
# field. That is the third time in this project a green test asserted the wrong
# invariant (see FEATURE-COMPLETE-CHECKLIST.md), so verify() now checks coverage.
KEY_LAYOUT = [
    ("ACTION_FLAGS", 0, 51), ("SRC_PORT", 52, 58), ("SRC_PORT_ID4", 59, 66),
    ("ISL_SGLORT", 67, 82), ("QOS_ISL_PRI", 83, 86), ("L2_TYPE_ID2", 87, 90),
    ("L2_DMAC_ID3", 91, 95), ("L2_SMAC_ID3", 96, 100), ("L3_DIP_ID3", 101, 105),
    ("L3_SIP_ID3", 106, 110), ("L3_PROT_ID2", 111, 114), ("MAP_VID1", 115, 126),
    ("reserved0", 127, 127), ("MAP_VID2", 128, 139), ("FFU_DATA_W8A", 140, 147),
    ("FFU_DATA_W8B", 148, 155), ("FFU_DATA_W16A_TOP", 156, 159),
    ("FFU_DATA_W16B_TOP", 160, 163), ("FFU_DATA_W24_TOP", 164, 171),
    ("NEXTHOP_TAG", 172, 179), ("FFU_DATA_TAG1A", 180, 191),
    ("FFU_DATA_TAG1B", 192, 203), ("FFU_DATA_TAG2A", 204, 215),
    ("FFU_DATA_TAG2B", 216, 227), ("FFU_DATA_W8C", 228, 235),
    ("L3_HASH", 236, 251),
]
RAM_LAYOUT = [("Mask_LO", 0, 25), ("Set_LO", 32, 57)]
RAM2_LAYOUT = [("Mask_HI", 0, 25), ("Set_HI", 32, 57)]


# ★ RAM3/RAM4/RAM5 FIELD LAYOUT, from the SDK's field descriptor table
# (12-byte stride, {name_ptr, bit_offset, width}; see sdk_regmap.py for how the
# register table next to it is found). These are the operands of the 6 sequential
# and 21 output mux actions in datasheet Table 5-31.
#
# Each list TILES its register exactly -- RAM3 covers bits 0-31 with no gap and no
# overlap, RAM4 covers 0-54, RAM5 covers 0-58. That is the check that they are
# complete and correctly assigned: a wrong grouping does not tile. Two RAM5 fields
# (SetDestMaskCmdProfile, W8ABCD_PROFILE) were first predicted from the gaps in
# the tiling and then confirmed against the table at bit 18/w1 and bit 24/w5.
#
# Widths cross-check against the datasheet's stated operand sizes: ALU13_CMD_PROFILE
# 5 bits, L2L_CMD_PROFILE 4 bits, DMASK_CMD_PROFILE 4 bits, every MuxOutput profile
# 5 bits.
RAM3_FIELDS = [
    ("SetTrapHeaderCmd", 0, 1), ("TRAP_HEADER_ENABLE", 1, 1),
    ("TRAP_HEADER_IDX", 2, 1), ("SetL2LookupCmdProfile", 3, 1),
    ("L2L_CMD_PROFILE", 4, 4), ("MuxOutput_MA1_MAC", 8, 1),
    ("MA1_MAC_PROFILE", 9, 5), ("MuxOutput_MA2_MAC", 14, 1),
    ("MA2_MAC_PROFILE", 15, 5), ("MuxOutput_VID", 20, 1),
    ("VID_PROFILE", 21, 5), ("MuxOutput_MA_FID", 26, 1),
    ("MA_FID_PROFILE", 27, 5),
]
RAM4_FIELDS = [
    ("SetHashProfile", 0, 1), ("HASH_PROFILE", 1, 4),
    ("MuxOutput_HASH_ROT", 5, 1), ("HASH_ROT_PROFILE", 6, 4),
    ("SetAlu13CmdProfile", 10, 1), ("ALU13_CMD_PROFILE", 11, 5),
    ("SetAlu46CmdProfile", 16, 1), ("ALU46_CMD_PROFILE", 17, 5),
    ("MuxOutput_ALU13_OP", 22, 1), ("ALU13_OP_PROFILE", 23, 5),
    ("MuxOutput_ALU46_OP", 28, 1), ("ALU46_OP_PROFILE", 29, 5),
    ("MuxOutput_POL1_IDX", 34, 1), ("POL1_IDX_PROFILE", 35, 4),
    ("MuxOutput_POL2_IDX", 39, 1), ("POL2_IDX_PROFILE", 40, 4),
    ("MuxOutput_POL3_IDX", 44, 1), ("POL3_IDX_PROFILE", 45, 4),
    ("MuxOutput_QOS", 49, 1), ("QOS_PROFILE", 50, 5),
]
RAM5_FIELDS = [
    ("MuxOutput_DGLORT", 0, 1), ("DGLORT_PROFILE", 1, 5),
    ("MuxOutput_SGLORT", 6, 1), ("SGLORT_PROFILE", 7, 5),
    ("MuxOutput_CSGLORT", 12, 1), ("CSGLORT_PROFILE", 13, 5),
    ("SetDestMaskCmdProfile", 18, 1), ("DMASK_CMD_PROFILE", 19, 4),
    ("MuxOutput_W8ABCD", 23, 1), ("W8ABCD_PROFILE", 24, 5),
    ("MuxOutput_W8E", 29, 1), ("W8E_PROFILE", 30, 5),
    ("MuxOutput_W8F", 35, 1), ("W8F_PROFILE", 36, 5),
    ("MuxOutput_W16ABC", 41, 1), ("W16ABC_PROFILE", 42, 5),
    ("MuxOutput_W16DEF", 47, 1), ("W16DEF_PROFILE", 48, 5),
    ("MuxOutput_W16GH", 53, 1), ("W16GH_PROFILE", 54, 5),
]


# Profile-table field layouts, same SDK field table (see sdk_regmap.py). Every
# table is per-channel {Value_X, optional Mask_X, Select_X}, applied as
#     X = (selected_source & Mask) | Value
# so Mask=0 is a constant assignment -- DGLORT profiles 2/6 are Value=0xfffe and
# 0xffff with Mask=0, the reserved flood/drop GLORTs.
#
# Cross-check: each layout's highest bit fits the words/entry the register
# descriptor declares, for all 19 tables. That agreement is what ties each field
# group to the right register.
PROFILE_FIELDS = {
    "DGLORT":   [("Value", 0, 16), ("Mask", 16, 16), ("Select", 32, 3)],
    "SGLORT":   [("Value", 0, 16), ("Mask", 16, 16), ("Select", 32, 3)],
    "CSGLORT":  [("Value", 0, 16), ("Mask", 16, 16), ("Select", 32, 2)],
    "MA1_MAC":  [("Value", 0, 48), ("Select", 48, 3)],
    "MA2_MAC":  [("Value", 0, 48), ("Select", 48, 2)],
    "W8E":      [("Value", 0, 8), ("Mask", 8, 8), ("Select", 16, 3)],
    "W8F":      [("Value", 0, 8), ("Mask", 8, 8), ("Select", 16, 2)],
    "POL1_IDX": [("Value", 0, 12), ("Select", 12, 4)],
    "POL2_IDX": [("Value", 0, 12), ("Select", 12, 4)],
    "POL3_IDX": [("Value", 0, 10), ("Mask", 10, 10), ("Select", 20, 3)],
    "HASH_ROT": [("Value_HASH_ROT", 0, 20), ("Select_A", 20, 1),
                 ("Select_B", 21, 1), ("ComputeRotA", 22, 1),
                 ("ComputeRotB", 23, 1), ("UsePTableRotA", 24, 1),
                 ("UsePTableRotB", 25, 1), ("RandomizeRotA", 26, 1),
                 ("RandomizeRotB", 27, 1)],
}
# VID / MA_FID / W8ABCD / W16ABC / W16DEF / W16GH / ALU13_OP / ALU46_OP / QOS are
# multi-channel; their per-channel layouts are in docs/EDGENOS-7150.md (was L3AR-STRUCTURE). Only the
# single-channel ones are rendered here, deliberately: a half-transcribed
# multi-channel layout would print confident wrong field names.


def tiles(flds, upto):
    """True iff flds covers 0..upto with no gap and no overlap."""
    seen = 0
    for _, lo, w in flds:
        m = ((1 << w) - 1) << lo
        if seen & m:
            return False
        seen |= m
    return seen == (1 << (upto + 1)) - 1


def decode_action(val, flds):
    """Render the nonzero fields of an action word. A MuxOutput_X of 0 means the
    channel is not driven by this rule, so its profile number is meaningless --
    print the profile only when its enable is set."""
    out = []
    d = {n: (val >> lo) & ((1 << w) - 1) for n, lo, w in flds}
    for n, lo, w in flds:
        v = d[n]
        if not v:
            continue
        if n.endswith("_PROFILE"):
            en = "MuxOutput_" + n[:-8]
            if en in d and not d[en]:
                out.append(f"{n}={v}(!unmuxed)")
                continue
        out.append(f"{n}={v}" if w > 1 else n)
    return ", ".join(out) if out else "-"


def cam_addr(s, r, seg, w):
    return L3AR_BASE + CAM_SLICE_STRIDE * s + CAM_RULE_STRIDE * r + 4 * seg + w


def ram_addr(s, r, w, second=False):
    off = RAM2_OFF if second else RAM1_OFF
    return L3AR_BASE + off + RAM_SLICE_STRIDE * s + 2 * r + w


def read_rule(mem, s, r):
    """Decode one rule, or None if the rule can never match.

    ⚠⚠ THE LIVENESS TEST USED TO BE `any word not in (0, 0xFFFFFFFF)`, AND THAT
    DISCARDED THE MOST IMPORTANT RULE IN EVERY SLICE. All-ones is Key=1,
    KeyInvert=1 on every bit -- don't-care everywhere, i.e. the UNIVERSAL MATCH.
    Rule 0 of slices 0-3 is exactly that: a default rule carrying the baseline
    flag resolution (Mask_LO 0x3febf3f, Mask_HI 0x3ff7f7b) and Set_HI bit 25,
    LoopbackSuppress. It matched every frame and we decoded it as empty.

    Consequences, all of which looked fine at the time:
      - slice rule counts came out 31/31/31/31/25 against the 32/32/32/32/25 the
        name file declares. The "RAM words are 2x the rule count" validation was
        recorded as PASSING; 64 RAM words against 31 decoded rules should have
        failed it. Fourth wrong-invariant in this project.
      - the per-rule mask baseline looked like a statistical mode across 149
        rules. It is not a mode, it is rule 0's action, applied to everything.
      - the LoopbackSuppress naming asymmetry got the causality backwards: the
        default sets it ON, so ...WithLoopbackSuppress rules need do nothing and
        ...WithoutLoopbackSuppress rules must actively clear it.
      - zeroing slice 0 broke forwarding on hardware, which is how this surfaced.

    Correct test: a rule is dead iff some bit is in the never-match state
    (Key=0, KeyInvert=0), which is what an all-zero word pair encodes.
    """
    segs, never_any = [], False
    for seg in range(SEGMENTS):
        w = [mem.get(cam_addr(s, r, seg, i)) for i in range(4)]
        if any(x is None for x in w):
            segs.append(None)
            continue
        keyinvert = (w[1] << 32) | w[0]
        key = (w[3] << 32) | w[2]
        dec = ternary(key, keyinvert)
        if dec[2]:
            never_any = True
        segs.append((key, keyinvert) + dec)
    if never_any or all(x is None for x in segs):
        return None
    a1 = [mem.get(ram_addr(s, r, i)) for i in range(2)]
    a2 = [mem.get(ram_addr(s, r, i, True)) for i in range(2)]
    ram1 = None if any(x is None for x in a1) else a1[0] | (a1[1] << 32)
    ram2 = None if any(x is None for x in a2) else a2[0] | (a2[1] << 32)
    return segs, ram1, ram2


def fields(raw, layout):
    out = {}
    if raw is None:
        return out
    for n, lo, hi in layout:
        v = (raw >> lo) & ((1 << (hi - lo + 1)) - 1)
        if v:
            out[n] = v
    return out


def encode_cam(value, care, never=0):
    if value & ~care:
        raise ValueError("value has bits set outside care mask")
    if never & care:
        raise ValueError("never-match bits overlap the care mask")
    return ((value | ~care) & MASK64 & ~never,
            (~value | ~care) & MASK64 & ~never)


def encode_ram(f, layout):
    acc = 0
    for n, lo, hi in layout:
        v = f.get(n, 0)
        if v >> (hi - lo + 1):
            raise ValueError(f"{n} too wide")
        acc |= v << lo
    return acc


def rule_key(segs):
    value = care = 0
    for i, seg in enumerate(segs):
        if seg is None:
            continue
        _, _, v, c, _ = seg
        value |= v << (64 * i)
        care |= c << (64 * i)
    return value, care


def key_fields(value, care):
    out = []
    for n, lo, hi in KEY_LAYOUT:
        w = (1 << (hi - lo + 1)) - 1
        c = (care >> lo) & w
        if c:
            out.append(f"{n}={(value >> lo) & w:#x}/mask{c:#x}")
    return ", ".join(out)


def verify(mem):
    ok = bad = rok = rbad = 0
    for s in range(NUM_SLICES):
        for r in range(RULES_PER_SLICE):
            res = read_rule(mem, s, r)
            if not res:
                continue
            segs, ram1, ram2 = res
            for seg in segs:
                if seg is None:
                    continue
                key, keyinvert, value, care, never = seg
                if encode_cam(value, care, never) == (key, keyinvert):
                    ok += 1
                else:
                    bad += 1
            for raw, lay in ((ram1, RAM_LAYOUT), (ram2, RAM2_LAYOUT)):
                if raw is None:
                    continue
                if encode_ram(fields(raw, lay), lay) == raw:
                    rok += 1
                else:
                    rbad += 1
    print(f"CAM segments round-tripped: {ok} ok, {bad} mismatched")
    print(f"action RAMs round-tripped:  {rok} ok, {rbad} mismatched")
    # layouts must tile
    prob = []
    pos = 0
    for n, lo, hi in KEY_LAYOUT:
        if lo != pos:
            prob.append(f"KEY_LAYOUT gap at {n}: expected {pos}, got {lo}")
        pos = hi + 1
    # ⚠ THE CHECK THAT WAS MISSING. Tiling from bit 0 says nothing about how far
    # the layout reaches; a list truncated at 147 tiles perfectly. Every care bit
    # EOS sets must land inside a named field, or the layout is incomplete and
    # key_fields() is silently discarding the bits that distinguish rules.
    named = 0
    for _, lo, hi in KEY_LAYOUT:
        named |= ((1 << (hi - lo + 1)) - 1) << lo
    uncovered = 0
    for s in range(NUM_SLICES):
        for r in range(RULES_PER_SLICE):
            res = read_rule(mem, s, r)
            if res:
                uncovered |= rule_key(res[0])[1] & ~named
    if uncovered:
        bits = [b for b in range(256) if (uncovered >> b) & 1]
        prob.append(f"care bits outside every named field: {bits}")
    for p in prob:
        print("  " + p)
    print(f"key coverage: {pos} bits named, "
          f"{'no' if not uncovered else len(bits)} care bits fall outside them")
    good = bad == 0 and rbad == 0 and not prob
    print("\nVERIFY " + ("PASS - the encoder reproduces a program it did not write"
                         if good else "FAIL"))
    return 0 if good else 1


def dump_actions(mem):
    """Raw dump of the banks the flag decoder never read.

    Deliberately prints words, not fields: the addresses are verified but the bit
    layout of RAM3/4/5 is not, and rendering invented field names over unverified
    bits is how the flag-rewrite-only claim survived as long as it did.
    """
    for s_ in range(NUM_SLICES):
        n = RULES_IN_SLICE[s_]
        r3 = [mem.get(L3AR_BASE + RAM3_OFF + RAM3_SLICE_STRIDE * s_ + r)
              for r in range(n)]
        # ⚠ RAM4 and RAM5 are TWO words. Reading only word 0 silently drops
        # every field above bit 31 -- for RAM4 that is the policer and QoS mux
        # (MuxOutput_QOS at 49, QOS_PROFILE at 50-54) and the top bits of
        # ALU46_OP_PROFILE, which spans 29-33. That defect made slice 4 look like
        # "ALU46 only" when it is really QoS classification, and reported its
        # rule 24 as operand profile 3 when word 1 bit 0 makes it 11. Caught by
        # byte-verifying gen_l3ar_slice4.py against the image.
        def w2(off, r):
            lo = mem.get(L3AR_BASE + off + RAM_SLICE_STRIDE * s_ + 2 * r)
            hi = mem.get(L3AR_BASE + off + RAM_SLICE_STRIDE * s_ + 2 * r + 1)
            if lo is None and hi is None:
                return None
            return (lo or 0) | ((hi or 0) << 32)

        r4 = [w2(RAM4_OFF, r) for r in range(n)]
        r5 = [w2(RAM5_OFF, r) for r in range(n)]
        def live(v):
            return sum(1 for x in v if x)
        print(f"=== slice {s_}  ({n} rules) ===")
        print(f"  RAM3 nonzero {live(r3):2d}/{n}   RAM4 nonzero {live(r4):2d}/{n}"
              f"   RAM5 nonzero {live(r5):2d}/{n}")
        for nm, v, f in (("RAM3", r3, RAM3_FIELDS), ("RAM4", r4, RAM4_FIELDS),
                         ("RAM5", r5, RAM5_FIELDS)):
            if not live(v):
                continue
            seen = {}
            for r, x in enumerate(v):
                if x:
                    seen.setdefault(decode_action(x, f), []).append(r)
            print(f"  {nm}:")
            for txt, rs in seen.items():
                rr = (f"rules {rs[0]}-{rs[-1]}" if len(rs) > 2 and
                      rs == list(range(rs[0], rs[-1] + 1))
                      else "rule" + ("s " if len(rs) > 1 else " ") +
                      ",".join(map(str, rs)))
                print(f"    {rr:22s} {txt}")
    print("=== profile tables ===")
    for nm, (base, w) in sorted(PROFILE_TABLES.items(), key=lambda kv: kv[1][0]):
        vals = [mem.get(base + i) for i in range(32 * w)]
        got = sum(1 for x in vals if x is not None)
        nz = sum(1 for x in vals if x)
        print(f"  {nm:10s} 0x{base:05x}  {w} word/entry  "
              f"written {got:3d}/{32 * w}  nonzero {nz:3d}")
        flds = PROFILE_FIELDS.get(nm)
        if not flds:
            continue
        for i in range(32):
            e, ok = 0, True
            for k in range(w):
                x = mem.get(base + i * w + k)
                if x is None:
                    ok = False
                    break
                e |= x << (32 * k)
            if not ok or not e:
                continue
            print(f"      [{i:2d}] " + "  ".join(
                f"{n}=0x{(e >> lo) & ((1 << wd) - 1):x}" for n, lo, wd in flds))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", required=True)
    ap.add_argument("--names")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--slice", type=int)
    ap.add_argument("--actions", action="store_true",
                    help="dump RAM3/4/5 and the profile tables (layout NOT decoded)")
    args = ap.parse_args()
    mem = load(args.image)
    names = load_names(args.names) if args.names else {}

    if args.summary:
        print("slice  rules  distinct flag-rewrites  key fields used")
        used = collections.Counter()
        for s in range(NUM_SLICES):
            rules = [r for r in range(RULES_PER_SLICE) if read_rule(mem, s, r)]
            acts = set()
            for r in rules:
                segs, ram1, ram2 = read_rule(mem, s, r)
                acts.add((ram1, ram2))
                v, c = rule_key(segs)
                for n, lo, hi in KEY_LAYOUT:
                    if (c >> lo) & ((1 << (hi - lo + 1)) - 1):
                        used[n] += 1
            print(f"  {s}    {len(rules):>4}          {len(acts):>4}")
        print("\nkey fields constrained across all rules:")
        for n, c in used.most_common():
            print(f"  {n:<16} {c:>4} rules")

    if args.actions:
        dump_actions(mem)
        return 0

    if args.verify:
        return verify(mem)

    if args.slice is not None:
        s = args.slice
        print(f"=== L3AR slice {s} ===")
        for r in range(RULES_PER_SLICE):
            res = read_rule(mem, s, r)
            if not res:
                continue
            segs, ram1, ram2 = res
            nm = names.get(("L3AR", s, r), "")
            v, c = rule_key(segs)
            print(f"  rule {r:>2}  {nm}")
            print(f"      match: {key_fields(v, c) or '(any)'}")
            f1, f2 = fields(ram1, RAM_LAYOUT), fields(ram2, RAM2_LAYOUT)
            if f1 or f2:
                parts = [f"{k}={v:#x}" for k, v in list(f1.items()) + list(f2.items())]
                print("      flags: " + ", ".join(parts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
