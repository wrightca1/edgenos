#!/usr/bin/env python3
# l2ar_disasm.py - disassemble L2 Action Resolution into readable rules.
#
# L2AR decides forward / trap / drop, and it is the largest transcribed file
# left in the tree (fm6000_l2arseq.c, 29,110 writes). Authoring it means first
# reading it, which is what this does.
#
# Geometry and field offsets both come from the SDK's own tables (sdk_regmap.py
# and sdk_fieldmap.py). Nothing here is guessed.
#
#     L2AR_CAM   [6 x 64 x 8] w=4 stride 0x20 outer 0x800  @0x140000
#                -> 8 slices x 64 rules, each a 384-bit key in 6 chunks
#                   of Key[64] @64 / KeyInvert[64] @0
#     L2AR_RAM   [64 x 8]     w=2 stride 0x80              @0x145400
#                -> the action for each of those 512 rules
#     L2AR_SLICE_CFG  SliceDisable[8] @8, ChainedPrecedence[8] @0
#
# Ternary semantics, as everywhere on this chip: a bit is DON'T CARE where Key
# and KeyInvert are both set -- so an all-ones rule is the universal default,
# not an empty slot -- and NEVER-MATCH where both are clear.
#
# SPDX-License-Identifier: GPL-2.0-or-later
import argparse, collections

CAM_BASE, CAM_SLICE, CAM_RULE, CHUNKS = 0x140000, 0x800, 0x20, 6
RAM_BASE, RAM_SLICE, RAM_W = 0x145400, 0x80, 2
SLICE_CFG = 0x145000
NSLICE, NRULE = 8, 64

RAM_FIELDS = {
    "FLAGS_TAG": (0, 8), "DMT_PROFILE": (8, 5), "TransformDestMask": (16, 1),
    "DMT_NEXT_STAGE": (17, 1), "SetCpuCode": (18, 1), "SetTrapHeader": (19, 1),
    "SetMirror_0": (20, 1), "SetMirror_1": (21, 1),
    "SetMirror_2": (22, 1), "SetMirror_3": (23, 1),
    "MuxOutput_QOS": (24, 1), "MuxOutput_MA_WRITEBACK": (25, 1),
    "MuxOutput_DGLORT": (26, 1), "MuxOutput_W16AB": (27, 1),
    "MuxOutput_W16CDEF": (28, 1), "MuxOutput_W8ABCDE": (29, 1),
    "MuxOutput_W4": (30, 1), "MuxOutput_VID": (31, 1),
    "MuxOutput_DMASK_IDX": (32, 1), "MuxOutput_STATS_IDX5AB": (33, 1),
    "MuxOutput_STATS_IDX5C": (34, 1), "MuxOutput_STATS_IDX12A": (35, 1),
    "MuxOutput_STATS_IDX12B": (36, 1), "MuxOutput_STATS_IDX16A": (37, 1),
    "MuxOutput_STATS_IDX16B": (38, 1),
}
M64 = (1 << 64) - 1

def read(path):
    d = {}
    for line in open(path):
        f = line.split()
        if len(f) == 2:
            try: d[int(f[0], 16)] = int(f[1], 16)
            except ValueError: pass
    return d

def rule_key(w, s, r):
    """Return (key, care, nwords) for one rule's 384-bit ternary key."""
    key = care = 0; n = 0
    for c in range(CHUNKS):
        a0 = CAM_BASE + s * CAM_SLICE + r * CAM_RULE + c * 4
        v = 0; present = False
        for i in range(4):
            x = w.get(a0 + i)
            if x is not None: v |= x << (32 * i); present = True; n += 1
        if not present: continue
        k, inv = (v >> 64) & M64, v & M64
        key |= k << (64 * c); care |= (k ^ inv) << (64 * c)
    return key, care, n

def rule_action(w, s, r):
    a0 = RAM_BASE + s * RAM_SLICE + r * RAM_W
    v = 0; n = 0
    for i in range(RAM_W):
        x = w.get(a0 + i)
        if x is not None: v |= x << (32 * i); n += 1
    return v, n

def describe(v):
    out = []
    for name, (off, wd) in RAM_FIELDS.items():
        f = (v >> off) & ((1 << wd) - 1)
        if not f: continue
        out.append(name if wd == 1 else "%s=%d" % (name, f))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--slice", type=int, default=None)
    ap.add_argument("--max", type=int, default=10)
    ap.add_argument("--summary", action="store_true")
    a = ap.parse_args()
    w = read(a.file)

    cfg = w.get(SLICE_CFG)
    if cfg is not None:
        print("L2AR_SLICE_CFG = %08x  ChainedPrecedence=%02x  SliceDisable=%02x"
              % (cfg, cfg & 0xff, (cfg >> 8) & 0xff))

    kinds = collections.Counter()
    per = collections.defaultdict(collections.Counter)
    rules = []
    for s in range(NSLICE):
        for r in range(NRULE):
            key, care, n = rule_key(w, s, r)
            if not n: kinds["not written"] += 1; per[s]["absent"] += 1; continue
            act, an = rule_action(w, s, r)
            if care == 0 and key != 0: k = "universal default (matches all)"
            elif key == 0 and care == 0: k = "never-match (zeroed)"
            else: k = "real rule"
            kinds[k] += 1; per[s][k] += 1
            rules.append((s, r, key, care, act, k))
    total = sum(kinds.values())
    print("\n512 rule slots (8 slices x 64):")
    for k, c in kinds.most_common():
        print("   %-34s %4d  (%.0f%%)" % (k, c, 100.0 * c / total))
    print("\nby slice:")
    for s in range(NSLICE):
        print("   slice %d: %s" % (s, dict(per[s])))
    if a.summary: return

    for s in (range(NSLICE) if a.slice is None else [a.slice]):
        rs = [x for x in rules if x[0] == s and x[5] == "real rule"]
        if not rs: continue
        print("\n=== slice %d : %d real rules ===" % (s, len(rs)))
        for _, r, key, care, act, _k in rs[:a.max]:
            kb = "%096x" % key; cb = "%096x" % care
            print("  [%2d] act=%s" % (r, " ".join(describe(act)) or "-"))
            print("       key  %s" % " ".join(kb[i:i+16] for i in range(0, 96, 16)))
            print("       care %s" % " ".join(cb[i:i+16] for i in range(0, 96, 16)))
        if len(rs) > a.max: print("  ... %d more" % (len(rs) - a.max))

if __name__ == "__main__":
    main()
