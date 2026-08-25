#!/usr/bin/env python3
"""gen_ffuinit.py - regenerate asic/fm6000/fm6000_ffuinit.c from a replay.

    gen_ffuinit.py <fwd4.txt> [-o out.c]
    gen_ffuinit.py <fwd4.txt> --verify <existing.c>

⚠ THIS FILE WAS MISSING. fm6000_ffuinit.c names it as its generator ("GENERATED
by asic/fm6000/tools/gen_ffuinit.py -- do not edit by hand") and it was not in the
tree, so the C file could not be regenerated at all. Recovered 2026-08-20 by
deriving the rule from the C file's own contents and proving it reproduces them.

THE RULE, recovered and verified: emit every register in the FFU region
(0x300000-0x3fffff) that the replay writes EXACTLY ONCE, with that value.

    existing C table                     8,680 pairs
    write-once FFU in the stock replay   8,680  -- identical set
    write-once FFU in the working replay 8,680  -- identical set

Multi-write registers are deliberately excluded and stay in the replay. The C
file's header explains why: a first version wrote the FFU's whole end state,
links came up and unicast forwarded, but OSPF never formed (routes stayed at 2)
because collapsing FFU_ATOMIC_APPLY's 59 writes performs one commit instead of
59 and the CPU-punt traps are never applied.

⚠ THE EXCLUSION IS BROADER THAN IT NEEDS TO BE, and the datasheet says so.
§5.7.13 names exactly which registers are shadowed and require ATOMIC_APPLY:
FFU_SLICE_MASTER_VALID; FFU_BST_{MASTER_VALID,SCENARIO_VALID,PARTITION_MAP,
ROOT_KEYS}. The CAM and BST *entries* are not shadowed. Measured on the working
replay, of 1,963 multi-write registers only ~345 fall in a conservative shadow
superset -- the other 1,618 (4,522 writes) are final-value semantics and could be
lifted, saving ~2,904 lines. See docs/EDGENOS-7150.md (was BLOB-REMOVAL-PLAN).

    ⚠ That extension is a GOAL A / GOAL B TRADE: it shrinks the replay while
    moving more of EOS's values into our source. Decide it deliberately. This
    tool deliberately does NOT do it -- pass --lift-multi to experiment, and read
    the plan doc first.

PROVENANCE. Reads a replay at runtime and embeds nothing of its own; the values
are EOS's, which is what "relocated, not authored" means for this block.
"""
import argparse, collections, re, sys

FFU_LO, FFU_HI = 0x300000, 0x400000


def shadow(a):
    """Conservative superset of the datasheet's shadow registers (§5.7.13).

    Drawn at PAGE granularity, not per-register, because the datasheet names
    FFU_BST_SCENARIO_VALID and FFU_BST_PARTITION_MAP but the register header
    defines neither. A per-register test also missed 0x33c09f -- a known strobe
    that turns out to sit inside FFU_BST_ROOT_KEYS.
    """
    if a == 0x3F0000:                                   # FFU_ATOMIC_APPLY
        return True
    if a >= 0x30C000 and (a - 0x30C000) % 0x10000 < 0x100:   # BST config page
        return True
    if a >= 0x381800 and (a - 0x381800) % 0x4000 < 0x100:    # slice config page
        return True
    return False


def load(path):
    cnt, val = collections.Counter(), {}
    for ln in open(path, errors="replace"):
        p = ln.split()
        if len(p) != 2:
            continue
        try:
            a, v = int(p[0], 16), int(p[1], 16)
        except ValueError:
            continue
        if FFU_LO <= a < FFU_HI:
            cnt[a] += 1
            val[a] = v          # last write wins -> final value
    return cnt, val


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay")
    ap.add_argument("-o", "--out")
    ap.add_argument("--verify", help="compare the generated table against an existing .c")
    ap.add_argument("--lift-multi", action="store_true",
                    help="ALSO emit multi-write registers by final value, minus the "
                         "shadow superset. Goal A/B trade -- read the plan doc.")
    a = ap.parse_args()

    cnt, val = load(a.replay)
    rows = sorted(x for x in cnt if cnt[x] == 1)
    if a.lift_multi:
        rows += sorted(x for x in cnt if cnt[x] > 1 and not shadow(x))
        rows.sort()
    table = [(x, val[x]) for x in rows]
    print(f"# {len(table)} FFU writes "
          f"({sum(1 for x in cnt if cnt[x]==1)} write-once"
          + (f", +{len(table)-sum(1 for x in cnt if cnt[x]==1)} lifted multi-write" if a.lift_multi else "")
          + ")", file=sys.stderr)

    if a.verify:
        cur = set()
        for ln in open(a.verify):
            m = re.match(r"\s*\{0x([0-9a-f]{8}),0x([0-9a-f]{8})\},", ln)
            if m:
                cur.add((int(m.group(1), 16), int(m.group(2), 16)))
        ours = set(table)
        print(f"existing {len(cur)}  generated {len(ours)}  identical={cur == ours}")
        if cur != ours:
            print(f"  only in existing : {len(cur - ours)}")
            print(f"  only in generated: {len(ours - cur)}")
            return 1
        return 0

    body = "".join(f"\t{{0x{x:08x},0x{v:08x}}},\n" for x, v in table)
    if a.out:
        open(a.out, "w").write(body)
        print(f"wrote {a.out} ({len(table)} rows)", file=sys.stderr)
    else:
        sys.stdout.write(body)
    return 0


if __name__ == "__main__":
    sys.exit(main())
