#!/usr/bin/env python3
"""l3ar_decode.py - decode and encode the FM6000 L3AR rules.

L3AR is fm6000_l3arinit.c, 3,928 transcribed microcode pairs
(docs/PROVENANCE.md 2.5). Decoder and encoder in one file because the block is
small and the two halves share the layout tables.

★ GEOMETRY, from the register header:

    L3AR_CAM(slice, rule, seg, word) = 0x10000 + 0x200*slice + 0x10*rule + 4*seg
        5 slices x 32 rules x 4 segments x 4 words -- a 256-bit key
    L3AR_RAM1(slice, rule, word)     = 0x11200 + 0x40*slice + 2*rule
    L3AR_RAM2(slice, rule, word)     = 0x11400 + 0x40*slice + 2*rule

⚠ ADDRESS. L3AR is at 0x10000. Two documents in this repo recorded it at
0x158000 for months -- that is MOD_CAM. The 0x010000 page REPLAY-TRIAGE.md
group 3 called "unnamed", and EOS-SOURCES.md later filed as generic microcode,
is L3AR. It was identified by clustering writes instead of reading
FM6000_L3AR_BASE.

★ THE ACTION IS A FLAG REWRITE, AND THAT IS ALL IT IS.

    RAM1  Mask_ACTION_FLAGS_LO[25:0]  Set_ACTION_FLAGS_LO[57:32]
    RAM2  Mask_ACTION_FLAGS_HI[25:0]  Set_ACTION_FLAGS_HI[57:32]

26 + 26 = 52 bits, exactly the width of ACTION_FLAGS in datasheet Table 5-30 --
which says "the L3AR's SetFlags action enables all configurable flag bits to be
redefined at this stage of the pipeline". So a rule is: match a 256-bit key
(overwhelmingly ACTION_FLAGS itself), then mask and set flags. No destination
mask, no CPU code, no mirror -- none of the entanglement that blocks L2AR's
generator.

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
RAM_SLICE_STRIDE = 0x40
SEGMENTS, NUM_SLICES, RULES_PER_SLICE = 4, 5, 32
MASK64 = 0xFFFFFFFFFFFFFFFF

# FM6000_L3AR_CAM_KEYS -- exact positions, 256 bits over 4 segments x 64.
KEY_LAYOUT = [
    ("ACTION_FLAGS", 0, 51), ("SRC_PORT", 52, 58), ("SRC_PORT_ID4", 59, 66),
    ("ISL_SGLORT", 67, 82), ("QOS_ISL_PRI", 83, 86), ("L2_TYPE_ID2", 87, 90),
    ("L2_DMAC_ID3", 91, 95), ("L2_SMAC_ID3", 96, 100), ("L3_DIP_ID3", 101, 105),
    ("L3_SIP_ID3", 106, 110), ("L3_PROT_ID2", 111, 114), ("MAP_VID1", 115, 126),
    ("reserved0", 127, 127), ("MAP_VID2", 128, 139), ("FFU_DATA_W8A", 140, 147),
]
RAM_LAYOUT = [("Mask_LO", 0, 25), ("Set_LO", 32, 57)]
RAM2_LAYOUT = [("Mask_HI", 0, 25), ("Set_HI", 32, 57)]


def cam_addr(s, r, seg, w):
    return L3AR_BASE + CAM_SLICE_STRIDE * s + CAM_RULE_STRIDE * r + 4 * seg + w


def ram_addr(s, r, w, second=False):
    off = RAM2_OFF if second else RAM1_OFF
    return L3AR_BASE + off + RAM_SLICE_STRIDE * s + 2 * r + w


def read_rule(mem, s, r):
    segs, live = [], False
    for seg in range(SEGMENTS):
        w = [mem.get(cam_addr(s, r, seg, i)) for i in range(4)]
        if any(x is None for x in w):
            segs.append(None)
            continue
        if any(x not in (0, 0xFFFFFFFF) for x in w):
            live = True
        keyinvert = (w[1] << 32) | w[0]
        key = (w[3] << 32) | w[2]
        segs.append((key, keyinvert) + ternary(key, keyinvert))
    if not live:
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
    for p in prob:
        print("  " + p)
    good = bad == 0 and rbad == 0 and not prob
    print("\nVERIFY " + ("PASS - the encoder reproduces a program it did not write"
                         if good else "FAIL"))
    return 0 if good else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", required=True)
    ap.add_argument("--names")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--slice", type=int)
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
