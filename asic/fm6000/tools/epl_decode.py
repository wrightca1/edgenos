#!/usr/bin/env python3
"""epl_decode.py - decode the EPL block's per-lane bring-up sequence.

EPL is the last big EOS-derived block in the replay (22,051 writes) and the one
that resisted every technique that worked elsewhere: collapsing it to an end
state WEDGES THE CHIP, because EPL is the SerDes/PCS bring-up itself and its
intermediate states drive hardware.

That framing was right but incomplete. EPL is not unstructured -- it is a
per-lane state sequence, and a sequence can be REGENERATED even though it
cannot be collapsed.

WHAT IT LOOKS LIKE (measured, EPL instance 7)

    instance base   0x0e0000 + 0x800 * instance      10 instances touched
    lane groups     offset 0x010, 0x090, 0x110, 0x190,
                           0x410, 0x490, 0x510, 0x590     (stride 0x80, 2 banks)
    per lane        ~87 bursts of 4 consecutive words
    sharing         7 of the 8 lanes get the IDENTICAL 87-burst sequence;
                    one lane gets 90

    first bursts of a lane:
        00000000 00800000 00000000 00005381
        20000000 00000000 00000000 00005381
        20000300 400003e0 00000600 00001841
        20000300 400003e0 00000600 00001841

    the offset stream is 69% self-similar at period 32 -- 8 lanes x 4 words.

So the block is a short state table replicated across lanes, not 22,051
independent facts. A generator emits the table per lane instead of collapsing
it, which is what the earlier attempt got wrong.

⚠ NOT YET IMPLEMENTED AS A GENERATOR. What remains is the interleaving: the
recorded stream does not run lane-major (all of lane A, then lane B) -- bursts
for different lanes are interspersed, and there are non-burst writes between
them (e.g. offset 0x301). Since collapsing EPL demonstrably wedges the chip,
the ordering has to be reproduced rather than assumed, and that means decoding
the interleave before writing the generator.

Usage:
    epl_decode.py <replay.txt> [--instance N]
"""
import argparse
import collections

EPL_LO, EPL_HI = 0x0E0000, 0x100000
INST_STRIDE = 0x800
LANE_MASK = 0x7F          # a lane group starts at offset & 0x7f == 0x10


def load(path):
    per = collections.defaultdict(list)
    for line in open(path):
        p = line.split()
        if len(p) < 2:
            continue
        try:
            a, v = int(p[0], 16), int(p[1], 16)
        except ValueError:
            continue
        if EPL_LO <= a < EPL_HI:
            off = (a - EPL_LO) % INST_STRIDE
            per[(a - EPL_LO) // INST_STRIDE].append((off, v))
    return per


def bursts_of(seq, lanes):
    out, i = [], 0
    while i < len(seq):
        b = seq[i][0] & ~0x3
        if b in lanes and i + 3 < len(seq) and \
           all((seq[i + k][0] & ~0x3) == b for k in range(4)):
            out.append((b, tuple(v for _, v in seq[i:i + 4])))
            i += 4
        else:
            i += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay")
    ap.add_argument("--instance", type=int)
    a = ap.parse_args()

    per = load(a.replay)
    total = sum(len(v) for v in per.values())
    print(f"EPL: {total} writes across {len(per)} instances "
          f"{sorted(per)}\n")
    print(f"  {'inst':>4}{'writes':>8}{'lanes':>7}{'bursts':>8}"
          f"{'distinct lane-seqs':>20}{'table':>8}")

    grand_raw = grand_tab = 0
    for inst in sorted(per):
        if a.instance is not None and inst != a.instance:
            continue
        seq = per[inst]
        lanes = sorted({o & ~0x3 for o, _ in seq if (o & LANE_MASK) == 0x10})
        bl = bursts_of(seq, lanes)
        byl = collections.defaultdict(list)
        for b, w in bl:
            byl[b].append(w)
        uniq = {tuple(v) for v in byl.values()}
        # what a generator would have to store: the distinct lane sequences
        tab = sum(len(u) * 4 for u in uniq)
        grand_raw += len(seq)
        grand_tab += tab
        print(f"  {inst:>4}{len(seq):>8}{len(lanes):>7}{len(bl):>8}"
              f"{len(uniq):>20}{tab:>8}")

    if a.instance is None:
        print(f"\n  raw EPL writes            {grand_raw:>7}")
        print(f"  distinct burst tables     {grand_tab:>7}   "
              f"({grand_raw / max(grand_tab,1):.1f}x)")
        print("\n  NB: the table is what a generator would STORE. It still has to"
              "\n  EMIT the full sequence -- EPL cannot be collapsed, only"
              "\n  regenerated. See the module docstring.")


if __name__ == "__main__":
    main()
