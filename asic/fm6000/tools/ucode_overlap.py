#!/usr/bin/env python3
# ucode_overlap.py - how much of the vendor's microcode is embedded in our C?
#
# This is the question that gates redistribution, and it is NOT the same question
# provenance_audit.py answers. That one grades how a value got into a file
# (authored / table / relocated). This one asks a narrower and harder thing:
# does a given {addr, value} pair appear verbatim in the operator's microcode
# files? A file can be graded TABLE -- our code decides where each entry goes --
# and still carry the vendor's exact pairs.
#
# Trivial fill (0x00000000, 0xffffffff) is excluded. It is not meaningfully
# anybody's program and counting it inflates the result; an earlier hand count in
# docs/PROVENANCE.md did exactly that and had to be revised.
#
# Usage:
#   ucode_overlap.py ucode_l2.raw ucode_tail.raw [--src asic/fm6000]
#
# SPDX-License-Identifier: GPL-2.0-or-later
import re, glob, os, argparse

PAIR = re.compile(r"\{\s*0x([0-9a-fA-F]+)\s*,\s*0x([0-9a-fA-F]+)\s*\}")
TRIVIAL = {0x00000000, 0xffffffff}

def read_pairs(path):
    o = set()
    for line in open(path):
        f = line.split()
        if len(f) == 2:
            try: o.add((int(f[0], 16), int(f[1], 16)))
            except ValueError: pass
    return o

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ucode", nargs="+", help="operator-supplied microcode files")
    ap.add_argument("--src", default=os.path.join(os.path.dirname(__file__), ".."),
                    help="directory of .c sources to scan")
    a = ap.parse_args()

    ucode = set()
    for f in a.ucode: ucode |= read_pairs(f)
    nontrivial = {p for p in ucode if p[1] not in TRIVIAL}
    print("microcode: %d distinct pairs, %d non-trivial\n" % (len(ucode), len(nontrivial)))

    rows = []
    for src in sorted(glob.glob(os.path.join(a.src, "*.c"))):
        ps = {(int(x, 16), int(y, 16))
              for x, y in PAIR.findall(open(src, errors="ignore").read())}
        if not ps: continue
        hit = ps & nontrivial
        if hit: rows.append((os.path.basename(src), len(ps), len(hit)))
    rows.sort(key=lambda r: -r[2])

    print("%-30s %9s %12s %7s" % ("source file", "pairs", "ucode pairs", "share"))
    for n, p, t in rows:
        print("%-30s %9d %12d %6.0f%%" % (n, p, t, 100.0 * t / p))
    tot = sum(r[2] for r in rows)
    print("\nvendor microcode pairs embedded in our C: %d of %d (%.1f%%), across %d files"
          % (tot, len(nontrivial), 100.0 * tot / len(nontrivial), len(rows)))
    if rows:
        top3 = sum(r[2] for r in rows[:3])
        print("concentration: the top 3 files carry %d of %d (%.0f%%)"
              % (top3, tot, 100.0 * top3 / tot))

if __name__ == "__main__":
    main()
