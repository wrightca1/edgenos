#!/usr/bin/env python3
"""l2ar_decode.py - decode the FM6000 L2AR rules into named, readable form.

L2AR is the largest transcribed block left in the tree: fm6000_l2arpre.c and
fm6000_l2arseq.c carry 12,473 microcode pairs each (docs/PROVENANCE.md 2.5).
This is the first half of replacing them, and it follows the route that worked
for the parser -- read the geometry out of the register header rather than
inferring it, then decode against it.

★ THE GEOMETRY, from the header (FM6000_L2AR_CAM / _RAM):

    L2AR_CAM(slice, rule, seg, word) = 0x140000 + 0x800*slice + 0x20*rule + 4*seg
        ENTRIES_2 = 8 slices, ENTRIES_1 = 64 rules, ENTRIES_0 = 6 segments,
        WIDTH = 4 words
    L2AR_RAM(slice, rule, word)      = 0x145400 + 0x80*slice + 2*rule
        WIDTH = 2 words

So a rule is 6 segments x 4 words = 24 words of key, with a 2-word action. That
explains the 24-word runs at stride 0x20 measured long before this file existed
(docs/L2AR-MICROCODE-STRUCTURE.md) -- they were 6 CAM segments, not one wide
entry, and the key is 6 x 128 = 768 bits.

Each segment is a ternary pair like the parser's, KeyInvert[63:0] / Key[127:64],
with the same four states per bit including the never-match encoding.

VALIDATED against the image before use: RAM entry counts come out at exactly
2 x the rule count per slice -- 92/48/128/108/102/128/92/128 against the
46/24/64/54/51/64/46/64 slice sizes declared in fm6000MicrocodeRuleNames.txt.
Two independent sources agreeing on all eight slices.

PROVENANCE. Reads the microcode image at runtime and never embeds it, the same
rule regmap.py and parser_decode.py follow. Rule NAMES come from Arista's
fm6000MicrocodeRuleNames.txt, which is marked confidential and stays out of the
tree -- supply it with --names.

Usage:
    l2ar_decode.py --image <fm6000Microcode.raw> --summary
    l2ar_decode.py --image <img> --names <fm6000MicrocodeRuleNames.txt> --slice 0
"""
import argparse
import collections
import re
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import load, ternary  # noqa: E402

L2AR_BASE = 0x140000
CAM_SLICE_STRIDE = 0x800
CAM_RULE_STRIDE = 0x20
CAM_SEG_WORDS = 4
CAM_SEGMENTS = 6
RAM_OFFSET = 0x5400
RAM_SLICE_STRIDE = 0x80
RAM_RULE_WORDS = 2
NUM_SLICES = 8
RULES_PER_SLICE = 64

# FM6000_L2AR_RAM_*, exact bit positions from the header.
RAM_LAYOUT = [
    ("FLAGS_TAG", 0, 7), ("DMT_PROFILE", 8, 12),
    ("TransformDestMask", 16, 16), ("DMT_NEXT_STAGE", 17, 17),
    ("SetCpuCode", 18, 18), ("SetTrapHeader", 19, 19),
    ("SetMirror", 20, 23),
    ("MuxOutput_QOS", 24, 24), ("MuxOutput_MA_WRITEBACK", 25, 25),
    ("MuxOutput_DGLORT", 26, 26), ("MuxOutput_W16AB", 27, 27),
    ("MuxOutput_W16CDEF", 28, 28), ("MuxOutput_W8ABCDE", 29, 29),
    ("MuxOutput_W4", 30, 30), ("MuxOutput_VID", 31, 31),
    ("MuxOutput_DMASK_IDX", 32, 32), ("MuxOutput_STATS_IDX5AB", 33, 33),
    ("MuxOutput_STATS_IDX5C", 34, 34), ("MuxOutput_STATS_IDX12A", 35, 35),
    ("MuxOutput_STATS_IDX12B", 36, 36), ("MuxOutput_STATS_IDX16A", 37, 37),
    ("MuxOutput_STATS_IDX16B", 38, 38),
]


def cam_addr(slice_, rule, seg, word):
    return (L2AR_BASE + CAM_SLICE_STRIDE * slice_ + CAM_RULE_STRIDE * rule
            + CAM_SEG_WORDS * seg + word)


def ram_addr(slice_, rule, word):
    return (L2AR_BASE + RAM_OFFSET + RAM_SLICE_STRIDE * slice_
            + RAM_RULE_WORDS * rule + word)


def read_rule(mem, slice_, rule):
    """Return (segments, action) or None if the rule is entirely unpopulated.

    segments is a list of (key, keyinvert, value, care, never) per segment.
    """
    segs = []
    live = False
    for seg in range(CAM_SEGMENTS):
        w = [mem.get(cam_addr(slice_, rule, seg, i)) for i in range(CAM_SEG_WORDS)]
        if any(x is None for x in w):
            segs.append(None)
            continue
        if any(x not in (0, 0xFFFFFFFF) for x in w):
            live = True
        keyinvert = (w[1] << 32) | w[0]
        key = (w[3] << 32) | w[2]
        value, care, never = ternary(key, keyinvert)
        segs.append((key, keyinvert, value, care, never))
    if not live:
        return None
    a = [mem.get(ram_addr(slice_, rule, i)) for i in range(RAM_RULE_WORDS)]
    action = None if any(x is None for x in a) else (a[0] | (a[1] << 32))
    return segs, action


def action_fields(action):
    out = {}
    if action is None:
        return out
    for name, lo, hi in RAM_LAYOUT:
        v = (action >> lo) & ((1 << (hi - lo + 1)) - 1)
        if v:
            out[name] = v
    return out


def load_names(path):
    """{(section, slice, rule): name} from Arista's rule-name file."""
    names, section = {}, None
    for line in open(path, errors="ignore"):
        m = re.match(r"\s*(\w[\w\s]*?)\s+Microcode rules:", line)
        if m:
            section = m.group(1).strip()
            continue
        m = re.match(r"\s*Slice #(\d+),\s*Rule #(\d+)\s*:\s*(.+?)\s*$", line)
        if m and section:
            names[(section, int(m.group(1)), int(m.group(2)))] = m.group(3)
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", required=True, help="fm6000Microcode.raw (operator-supplied)")
    ap.add_argument("--names", help="fm6000MicrocodeRuleNames.txt (operator-supplied)")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--slice", type=int)
    args = ap.parse_args()

    mem = load(args.image)
    names = load_names(args.names) if args.names else {}

    if args.summary:
        print("slice  rules  with-action  distinct actions")
        total = 0
        for s in range(NUM_SLICES):
            rules = [r for r in range(RULES_PER_SLICE) if read_rule(mem, s, r)]
            acts = collections.Counter()
            for r in rules:
                res = read_rule(mem, s, r)
                if res and res[1] is not None:
                    acts[res[1]] += 1
            total += len(rules)
            print(f"  {s}    {len(rules):>4}   {sum(acts.values()):>6}      {len(acts):>4}")
        print(f"\ntotal populated rules: {total}")
        print("(fm6000MicrocodeRuleNames.txt declares 413 across 46/24/64/54/51/64/46/64;"
              " the shortfall is rules whose key is entirely fill)")

    if args.slice is not None:
        s = args.slice
        print(f"=== L2AR slice {s} ===")
        for r in range(RULES_PER_SLICE):
            res = read_rule(mem, s, r)
            if not res:
                continue
            segs, action = res
            nm = names.get(("L2AR", s, r), "")
            print(f"  rule {r:>2}  {nm}")
            for i, seg in enumerate(segs):
                if seg is None:
                    continue
                _, _, value, care, never = seg
                if not care and not never:
                    continue
                bits = bin(care).count("1")
                print(f"      seg{i} value=0x{value:016x} care=0x{care:016x} ({bits} bits)"
                      + ("  DISABLED" if never else ""))
            f = action_fields(action)
            if f:
                print("      action: " + ", ".join(f"{k}={v}" for k, v in f.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
