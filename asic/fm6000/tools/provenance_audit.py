#!/usr/bin/env python3
"""provenance_audit.py - classify every tracked file by where its CONTENT came from.

docs/EDGENOS-7150.md (was PROVENANCE) sets the rule this checks:

    Register addresses, field layouts and hardware behaviour are facts about the
    chip and are fine. A verbatim transcription of a proprietary program's data
    tables is not.

The awkward case is a `{ address, value }` table. The ADDRESS is a fact. The
VALUE is a fact only if we can say WHY it has that value. So files are graded on
whether their own generator derived the structure, or merely moved the vendor's
bytes:

    AUTHORED     structure recovered and named; values follow from stated intent,
                 or there are no value literals at all
    TABLE        our code decides where each entry goes, but the values are still
                 the vendor's -- defensible as configuration, weakest of the
                 "keep" categories
    RELOCATED    the file's own header says it REPLAYS a captured sequence. This
                 is transcription and is the category that must not grow.
    CAPTURE      raw captured vendor behaviour. Must not be in the tree at all.

⚠ This grades by declared intent (the generator and the file's own header), not
by inspecting values, so it cannot catch a file that lies about itself. It is a
audit aid, not a proof. Read the flagged files.

usage: provenance_audit.py [--md]
SPDX-License-Identifier: GPL-2.0-or-later
"""
import os
import re
import subprocess
import sys

PAIR = re.compile(r"^\s*\{\s*0x[0-9a-fA-F]+\s*,\s*0x[0-9a-fA-F]+\s*\}\s*,")

# Files whose headers describe them as replaying a captured sequence.
RELOCATED = {
    "fm6000_l2arseq.c", "fm6000_l2arpre.c", "fm6000_eplseq.c",
    "fm6000_mapperpre.c", "fm6000_mgmt2pre.c",
}
# Generators that recover structure and emit from it.
AUTHORED = {
    "fm6000_esched.c", "fm6000_erl.c", "fm6000_cmrest.c",
    "fm6000_parserfields.c", "fm6000_smalltables.c", "fm6000_cmwm.c",
    "fm6000_mapper.c", "fm6000_l3artables.c",
    "fm6000_l3arslice1.c", "fm6000_l3arslice2.c",
    "fm6000_l3arslice3.c", "fm6000_l3arslice4.c",
}


def grade(path, text, npairs):
    base = os.path.basename(path)
    if base in RELOCATED:
        return "RELOCATED"
    if base in AUTHORED:
        return "AUTHORED"
    if npairs == 0:
        return "AUTHORED"
    if re.search(r"^\s*\*\s*.*\breplay(s|ing)?\b.*\bin order\b", text, re.M | re.I):
        return "RELOCATED"
    return "TABLE"


def main():
    files = [f for f in subprocess.run(["git", "ls-files"], capture_output=True,
                                       text=True).stdout.split() if os.path.isfile(f)]
    rows = []
    for f in files:
        if not f.endswith((".c", ".h")):
            continue
        try:
            t = open(f, errors="surrogateescape").read()
        except OSError:
            continue
        n = sum(1 for l in t.split("\n") if PAIR.match(l))
        if n:
            rows.append((grade(f, t, n), n, f))
    rows.sort(key=lambda r: (-r[1],))

    tot = {}
    for g, n, _ in rows:
        tot[g] = tot.get(g, 0) + n

    md = "--md" in sys.argv
    if md:
        print("| grade | pairs | file |")
        print("|---|---:|---|")
        for g, n, f in rows:
            print(f"| {g} | {n:,} | `{f}` |")
    else:
        for g, n, f in rows:
            print(f"  {g:10s} {n:7d}  {f}")

    print()
    for g in ("AUTHORED", "TABLE", "RELOCATED", "CAPTURE"):
        if g in tot:
            print(f"{g:10s} {tot[g]:7,d} pairs in "
                  f"{sum(1 for r in rows if r[0] == g)} files")
    bad = tot.get("RELOCATED", 0) + tot.get("CAPTURE", 0)
    print(f"\n{bad:,} pairs are transcription rather than authorship.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
