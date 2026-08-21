#!/usr/bin/env python3
"""sdk_regmap.py - recover the FM6000 register map from the FocalPoint SDK.

The INTEL CONFIDENTIAL register header does not define every block. L3AR is the
worst case: its 0x10000 macros are absent entirely (EOS-SOURCES.md:77), so the
block was mapped for months by clustering replay writes. That gets base addresses
roughly right and ENTRY WIDTHS wrong, and a wrong width is a wrong slice stride,
which returns a neighbouring slice's content under the wrong label without ever
erroring. docs/L3AR-STRUCTURE.md records one such error.

libFocalpointSDK.so carries a register descriptor table in .rodata that gives both:

    56-byte stride, entry = {
        +0  name_ptr        +32  dim2_max     (0 if unused)
        +4  n_dims          +36  dim1_max     (0 if unused)
        +8  base_addr       +40  dim0_max     (0 if unused)
       +12  0               +44  words_per_entry
       +16  words_per_entry +48  entry_stride (words)
       +20  0               +52  outer_stride (words)
       +24  0
       +28  0                                                              }

703 registers across 79 blocks. The GEOMETRY columns (+32..+52) matter as much as
the address: an entry stride is what turns a base address into the right row, and
a wrong stride returns a neighbouring entry's content under the right label
without ever erroring. docs/L3AR-STRUCTURE.md records exactly that failure --
L3AR_RAM3 was decoded at stride 0x40 for weeks when the silicon uses 0x20, which
silently returned slices 2-3's data under slice 1's name. This table says
words=1, stride=0x20 for RAM3 and words=2, stride=0x40 for RAM1/2/4/5, which is
what the wire eventually showed. It would have prevented the error outright. The table is what the SDK's own register-dump and
verify paths read, which is why the names are intact in a binary whose static
symbols are stripped.

PROVENANCE. Reads the SDK at runtime and embeds nothing, the same rule regmap.py
follows for the Intel header. What we take is facts about the silicon, recorded in
our own words (PROVENANCE.md). The .so is not in any git tree -- it lives outside
at ../eos-4.16.8M/extracted/, see EOS-SOURCES.md.

⚠ VALIDATE BEFORE TRUSTING. --check re-derives addresses we established
independently by other means. It passes today on L3AR_CAM (0x10000),
MOD_COMMAND_RAM (0x159000, w=1), MOD_VALUE_RAM (0x159400, w=2), MOD_CAM
(0x158000) and L2AR_CAM (0x140000) -- the MOD widths matching the entry shapes
measured byte-by-byte off the wire is the strongest evidence the width column
means what it appears to mean.

usage:
    sdk_regmap.py --so <libFocalpointSDK.so> --check
    sdk_regmap.py --so <lib> [--block L3AR] [--json out.json]
"""
import argparse
import collections
import json
import struct
import sys

NAME_PREFIX = b"FM6000_"
STRIDE = 56          # bytes between descriptor entries
ADDR_OFF, WIDTH_OFF = 8, 16   # offsets within an entry, from the name pointer
NDIM_OFF = 4                  # count of used dimensions
DIM_OFFS = (32, 36, 40)       # dim2_max, dim1_max, dim0_max -- INCLUSIVE maxima
STRIDE_OFF, OUTER_OFF = 48, 52

# Addresses established independently of this table -- by clustering replay
# writes, by the Intel header, or by decoding entry shapes off the wire. If the
# table disagrees with these, it is not the table we think it is.
KNOWN = {
    "FM6000_L3AR_CAM":        (0x010000, None),
    "FM6000_L2AR_CAM":        (0x140000, None),
    "FM6000_MOD_CAM":         (0x158000, None),
    "FM6000_MOD_COMMAND_RAM": (0x159000, 1),
    "FM6000_MOD_VALUE_RAM":   (0x159400, 2),
}

# Geometry established independently, off the wire, before this table was read.
# These are the checks that make the stride column trustworthy rather than
# merely plausible -- KNOWN above only ever tested addresses and widths.
KNOWN_GEOM = {
    # name                     words  entry_stride  outer_stride
    "FM6000_L3AR_RAM3":         (1,   0x20,  1),   # the stride we once had wrong
    "FM6000_L3AR_RAM1":         (2,   0x40,  1),
    "FM6000_L3AR_RAM5":         (2,   0x40,  1),
    "FM6000_L3AR_CAM":          (4,   0x10,  0x200),
}


def load_segments(d):
    """LOAD segments as (vaddr, file_offset, filesz), for vaddr->offset mapping."""
    if d[:4] != b"\x7fELF" or d[4] != 1:
        sys.exit("not a 32-bit ELF")
    e_phoff = struct.unpack_from("<I", d, 0x1C)[0]
    e_phentsize = struct.unpack_from("<H", d, 0x2A)[0]
    e_phnum = struct.unpack_from("<H", d, 0x2C)[0]
    segs = []
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _, p_filesz = struct.unpack_from(
            "<IIIII", d, e_phoff + i * e_phentsize)
        if p_type == 1:
            segs.append((p_vaddr, p_offset, p_filesz))
    return segs


def scan(path):
    d = open(path, "rb").read()
    segs = load_segments(d)
    lo = min(s[0] for s in segs)
    hi = max(s[0] + s[2] for s in segs)

    def rdstr(v):
        for va, fo, sz in segs:
            if va <= v < va + sz:
                o = fo + (v - va)
                break
        else:
            return None
        e = d.find(b"\0", o)
        if e < 0 or e == o or e - o > 72:
            return None
        s = d[o:e]
        return s.decode("ascii") if all(32 <= c < 127 for c in s) else None

    # Anchor the 56-byte lattice on any entry, then walk it. Scanning every
    # 4-byte position instead would pick up name pointers held elsewhere (891 of
    # them) that are not descriptor entries at all.
    anchor = None
    for off in range(0, len(d) - 4, 4):
        v = struct.unpack_from("<I", d, off)[0]
        if lo <= v < hi and rdstr(v) == "FM6000_L3AR_CAM":
            anchor = off
            break
    if anchor is None:
        sys.exit("descriptor table not found -- is this the FocalPoint SDK?")

    ents, o = [], anchor % STRIDE
    while o + STRIDE <= len(d):
        v = struct.unpack_from("<I", d, o)[0]
        if lo <= v < hi:
            s = rdstr(v)
            if s and s.startswith(NAME_PREFIX.decode()):
                a = struct.unpack_from("<I", d, o + ADDR_OFF)[0]
                w = struct.unpack_from("<I", d, o + WIDTH_OFF)[0]
                # a register at >32M or wider than 64 words is a false positive
                if a < 0x2000000 and 0 < w <= 64:
                    dims = [struct.unpack_from("<I", d, o + x)[0]
                            for x in DIM_OFFS]
                    # stored as INCLUSIVE maxima; 0 means the dimension is unused
                    # (a 1-entry axis and an absent axis are indistinguishable
                    #  here, which is why counts are reported as max+1 only for
                    #  the axes n_dims says are real)
                    ents.append((s, a, w,
                                 struct.unpack_from("<I", d, o + STRIDE_OFF)[0],
                                 struct.unpack_from("<I", d, o + OUTER_OFF)[0],
                                 tuple(dims)))
        o += STRIDE
    return ents


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--so", required=True)
    ap.add_argument("--block")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--json")
    args = ap.parse_args()

    ents = scan(args.so)
    m = {e[0]: (e[1], e[2]) for e in ents}
    geom = {e[0]: e[3:] for e in ents}
    print(f"{len(ents)} registers, "
          f"{len({e[0].split('_')[1] for e in ents})} blocks", file=sys.stderr)

    if args.check:
        bad = 0
        for name, (exp_a, exp_w) in KNOWN.items():
            got = m.get(name)
            if not got:
                print(f"  {name:26s} ABSENT"); bad += 1; continue
            ok = got[0] == exp_a and (exp_w is None or got[1] == exp_w)
            bad += not ok
            print(f"  {name:26s} 0x{got[0]:06x} w={got[1]}  "
                  f"{'ok' if ok else 'MISMATCH, expected 0x%06x' % exp_a}")
        # Geometry, checked against strides measured off the wire. This is the
        # column that silently corrupts a decode when it is wrong, so it gets a
        # check of its own rather than riding on the address check.
        for name, (ew, es, eo) in KNOWN_GEOM.items():
            g = geom.get(name)
            w = m.get(name, (0, 0))[1]
            if g is None:
                print(f"  {name:26s} ABSENT"); bad += 1; continue
            stride, outer, _dims = g
            ok = (w == ew and stride == es and outer == eo)
            bad += not ok
            print(f"  {name:26s} words={w} stride=0x{stride:x} outer=0x{outer:x}"
                  f"  {'ok' if ok else 'MISMATCH, expected words=%d stride=0x%x outer=0x%x' % (ew, es, eo)}")
        print("CHECK PASS" if not bad else f"CHECK FAIL ({bad})")
        return 1 if bad else 0

    if args.json:
        json.dump(sorted(ents, key=lambda e: e[1]), open(args.json, "w"), indent=1)
        return 0

    sel = [e for e in ents
           if not args.block or e[0].split("_")[1] == args.block.upper()]
    if args.block:
        for s, a, w, stride, outer, dims in sorted(sel, key=lambda e: e[1]):
            n = [x + 1 for x in dims if x]
            shape = "x".join(str(x) for x in n) if n else "1"
            print(f"{a:08x}  w={w:<2d} stride=0x{stride:<4x} outer=0x{outer:<4x}"
                  f"  [{shape:>12s}]  {s}")
    else:
        for b, n in collections.Counter(
                e[0].split("_")[1] for e in sel).most_common():
            print(f"{n:4d}  {b}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
