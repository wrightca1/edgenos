#!/usr/bin/env python3
"""l2ar_gen.py - encode FM6000 L2AR rules, and prove the encoder on EOS's own.

The encoder half of replacing fm6000_l2arpre.c and fm6000_l2arseq.c, which carry
12,473 microcode pairs each (docs/PROVENANCE.md 2.5). l2ar_decode.py reads their
program; this writes ours.

    encode_cam(value, care, never)  -> (Key, KeyInvert)   per 64-bit segment
    encode_action({field: value})   -> 2 RAM words

★ THE POINT OF --verify, and it is the same point as gen_parser's. An encoder is
only trustworthy if it can reproduce a program nobody here authored. --verify
decodes every populated rule of an EOS image, re-encodes it from the decoded
fields, and demands bit-identical output across all 6 segments and the action.

That check earned its keep on the parser: it failed first time and exposed the
never-match encoding, a fourth ternary state that had been silently collapsed.

⚠ WHAT IT CANNOT DO. A round-trip proves the packing is SELF-CONSISTENT. It
cannot prove the interpretation is right -- shifted field boundaries re-encode
to identical bits and pass perfectly. On the parser, TWO different wrong action
layouts both round-tripped 2,117/2,117. Only an external fact settles
interpretation, and for L2AR that fact is FM6000_L2AR_CAM_KEYS in the register
header, which is where the key layout in l2ar_decode.py comes from.

PROVENANCE. Reads an image at runtime for --verify only, and embeds nothing.
Field names, widths and positions are register-header facts.

Usage:
    l2ar_gen.py --verify --image <fm6000Microcode.raw>
    l2ar_gen.py --self-test
    l2ar_gen.py --keymap
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import load, ternary  # noqa: E402
from l2ar_decode import (  # noqa: E402
    read_rule, action_fields, RAM_LAYOUT, NUM_SLICES, RULES_PER_SLICE,
    CAM_SEGMENTS,
)

MASK64 = 0xFFFFFFFFFFFFFFFF

# FM6000_L2AR_CAM_KEYS -- exact bit positions, 384 bits over 6 segments x 64.
# ⚠ NOT the order Table 5-71 lists them in; that ordering was tested and refuted
# (see docs/L2AR-MICROCODE-STRUCTURE.md). DMASK_A is deliberately absent: it is
# matched by a separate structure, FM6000_L2AR_CAM_DMASK, which is why the
# datasheet gives it bitwise-OR semantics unlike anything here.
KEY_LAYOUT = [
    ("SMASK", 0, 75), ("ACTION_FLAGS", 76, 151), ("L2F_ISTATE", 152, 164),
    ("DGLORT_TAG", 165, 165), ("MA1_TAG", 166, 177), ("MA2_TAG", 178, 189),
    ("reserved0", 190, 191), ("L2L_ETAG1", 192, 203), ("L2L_ETAG2", 204, 215),
    ("ALU13_Z", 216, 231), ("ALU46_Z", 232, 247), ("ISL_USER", 248, 255),
    ("ACTION_DATA_W8F", 256, 263), ("L2_DMAC_ID3", 264, 268),
    ("L2_SMAC_ID3", 269, 273), ("L2_TYPE_ID2", 274, 277),
    ("POL1_TAG1_TOP", 278, 281), ("POL1_TAG2_TOP", 282, 285),
    ("POL2_TAG1_TOP", 286, 289), ("POL2_TAG2_TOP", 290, 293),
    ("POL3_TAG_TOP", 294, 297), ("DGLORT", 298, 313), ("reserved1", 314, 319),
    ("SGLORT", 320, 335), ("DROP_CODE", 336, 343), ("MA1_HPV", 344, 347),
    ("MA1_FID2_IVL", 348, 348), ("MA2_FID2_IVL", 349, 349),
    ("MA1_LOOKUP", 350, 350), ("MA2_LOOKUP", 351, 351),
    ("MA2_HPV", 352, 355), ("MA2_MPV", 356, 359),
    ("L2L_ITAG1", 360, 371), ("L2L_ITAG2", 372, 383),
]


def encode_cam(value, care, never=0):
    """Per-segment ternary encode. Same convention as the parser's CAM."""
    if value & ~care:
        raise ValueError("value has bits set outside care mask")
    if never & care:
        raise ValueError("never-match bits overlap the care mask")
    key = (value | ~care) & MASK64 & ~never
    keyinvert = (~value | ~care) & MASK64 & ~never
    return key, keyinvert


def encode_action(fields):
    """Pack {field: value} into the 64-bit action (2 words)."""
    acc = 0
    for name, lo, hi in RAM_LAYOUT:
        v = fields.get(name, 0)
        width = hi - lo + 1
        if v >> width:
            raise ValueError(f"{name}=0x{v:x} exceeds {width} bits")
        acc |= v << lo
    return [acc & 0xFFFFFFFF, (acc >> 32) & 0xFFFFFFFF]


def key_field(value, care, name):
    """Extract a named key field's (value, care) from the 384-bit key."""
    for n, lo, hi in KEY_LAYOUT:
        if n == name:
            w = (1 << (hi - lo + 1)) - 1
            return (value >> lo) & w, (care >> lo) & w
    raise KeyError(name)


def rule_key(segs):
    """Assemble the 384-bit (value, care, never) from 6 decoded segments."""
    value = care = never = 0
    for i, seg in enumerate(segs):
        if seg is None:
            continue
        _, _, v, c, n = seg
        value |= v << (64 * i)
        care |= c << (64 * i)
        never |= n << (64 * i)
    return value, care, never


def verify(image):
    mem = load(image)
    seg_ok = seg_bad = act_ok = act_bad = 0
    failures = []
    for s in range(NUM_SLICES):
        for r in range(RULES_PER_SLICE):
            res = read_rule(mem, s, r)
            if not res:
                continue
            segs, action = res
            for i, seg in enumerate(segs):
                if seg is None:
                    continue
                key, keyinvert, value, care, never = seg
                rk, ri = encode_cam(value, care, never)
                if (rk, ri) == (key, keyinvert):
                    seg_ok += 1
                else:
                    seg_bad += 1
                    if len(failures) < 5:
                        failures.append(
                            f"  CAM s{s} r{r} seg{i}: got 0x{rk:016x}/0x{ri:016x}, "
                            f"want 0x{key:016x}/0x{keyinvert:016x}")
            if action is not None:
                w = encode_action(action_fields(action))
                want = [action & 0xFFFFFFFF, (action >> 32) & 0xFFFFFFFF]
                if w == want:
                    act_ok += 1
                else:
                    act_bad += 1
                    if len(failures) < 10:
                        failures.append(
                            f"  ACT s{s} r{r}: got {[hex(x) for x in w]}, "
                            f"want {[hex(x) for x in want]}")
    print(f"CAM segments round-tripped: {seg_ok} ok, {seg_bad} mismatched")
    print(f"actions    round-tripped:   {act_ok} ok, {act_bad} mismatched")
    for f in failures:
        print(f)
    # the action packs 39 of 64 bits; bits outside the layout would be dropped
    stray = 0
    for s in range(NUM_SLICES):
        for r in range(RULES_PER_SLICE):
            res = read_rule(mem, s, r)
            if res and res[1] is not None and res[1] >> 39:
                stray += 1
    print(f"actions with bits set above the 39-bit layout: {stray}")
    good = seg_bad == 0 and act_bad == 0
    print("\nVERIFY " + ("PASS - the encoder reproduces a program it did not write"
                         if good else "FAIL"))
    return 0 if good else 1


def keymap(image):
    """Which named key fields EOS's rules actually constrain."""
    mem = load(image)
    import collections
    used = collections.Counter()
    for s in range(NUM_SLICES):
        for r in range(RULES_PER_SLICE):
            res = read_rule(mem, s, r)
            if not res:
                continue
            value, care, never = rule_key(res[0])
            for n, lo, hi in KEY_LAYOUT:
                w = (1 << (hi - lo + 1)) - 1
                if (care >> lo) & w:
                    used[n] += 1
    print("key fields constrained by EOS's L2AR rules:")
    for n, c in used.most_common():
        print(f"  {n:<18} {c:>4} rules")
    unused = [n for n, _, _ in KEY_LAYOUT if n not in used]
    print(f"\nnever constrained: {', '.join(unused) if unused else '(none)'}")
    return 0


def self_test():
    fails = []
    k, i = encode_cam(0x0800, 0xFFFF)
    v, c, n = ternary(k, i)
    if (v, c, n) != (0x0800, 0xFFFF, 0):
        fails.append(f"exact match round-trip: {v:#x}/{c:#x}/{n:#x}")
    k, i = encode_cam(0, 0)
    if (k, i) != (MASK64, MASK64):
        fails.append("full don't-care must be all-ones/all-ones")
    w = encode_action({"SetTrapHeader": 1})
    if not (w[0] >> 19) & 1:
        fails.append("SetTrapHeader must land at bit 19")
    w = encode_action({"MuxOutput_STATS_IDX16B": 1})
    if not (w[1] >> (38 - 32)) & 1:
        fails.append("MuxOutput_STATS_IDX16B must land at bit 38")
    try:
        encode_action({"DMT_PROFILE": 64})
        fails.append("over-wide field not rejected")
    except ValueError:
        pass
    # the key layout must tile 384 bits with no gap or overlap
    pos = 0
    for n, lo, hi in KEY_LAYOUT:
        if lo != pos:
            fails.append(f"key layout gap/overlap at {n}: expected {pos}, got {lo}")
        pos = hi + 1
    if pos != 384:
        fails.append(f"key layout covers {pos} bits, expected 384 (6 segments x 64)")
    for f in fails:
        print("  FAIL:", f)
    print("self-test " + ("PASS" if not fails else "FAIL"))
    return 0 if not fails else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--keymap", action="store_true",
                    help="which key fields EOS's rules constrain")
    ap.add_argument("--image", help="fm6000Microcode.raw (operator-supplied)")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    rc = 0
    if args.self_test:
        rc |= self_test()
    if args.verify or args.keymap:
        if not args.image:
            sys.exit("--verify/--keymap need --image")
        if args.verify:
            rc |= verify(args.image)
        if args.keymap:
            rc |= keymap(args.image)
    if not (args.self_test or args.verify or args.keymap):
        ap.print_help()
    return rc


if __name__ == "__main__":
    sys.exit(main())
