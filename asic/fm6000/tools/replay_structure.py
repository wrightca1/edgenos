#!/usr/bin/env python3
"""replay_structure.py - find the loop structure inside an FM6000 boot replay.

replay_classify.py answers "what KIND of writes are these". This answers the
question that actually decides whether a generator is feasible: "how much unique
state is in here, and how much is a loop repeating itself?"

The replay is a transcription of EOS driving the chip, so it records EOS's
control flow, not just its intent. Where EOS ran a per-port loop, the trace
holds N nearly-identical copies of one body. A generator does not have to
reproduce the transcription -- only the state it leaves behind, plus whatever
ordering the hardware genuinely requires.

Measured on fwd4.txt (389,809 writes / 296,084 after SBus is excluded):

    distinct addresses            93,659      3.2x redundancy
    non-zero final values         70,396

    the outer loop                336 iterations, 227,745 writes = 77% of MMIO
      L2F+LBS core                74,034 writes -> only 8 distinct variants
      per-port remainder         153,711 writes -> SAF, CM, EPL, L2L

So a quarter of the whole replay is eight patterns written 336 times.

Usage:
    replay_structure.py <replay.txt> [--anchor 0x1a0c00]
"""
import argparse
import collections

SBUS = (0xF001, 0xF002, 0xF003, 0xF004)

# The loop body starts each iteration by touching this register. Any address
# that appears once per iteration works as an anchor; this one is the first
# write of the L2F sweep.
DEFAULT_ANCHOR = 0x1A0C00

# The part of the body that turns out to be near-constant across iterations.
CORE = ((0x180000, 0x200000), (0x014000, 0x015000))   # L2F, LBS


def load(path):
    rows = []
    for line in open(path):
        p = line.split()
        if len(p) < 2:
            continue
        try:
            a, v = int(p[0], 16), int(p[1], 16)
        except ValueError:
            continue
        if a not in SBUS:
            rows.append((a, v))
    return rows


def in_core(a):
    return any(lo <= a < hi for lo, hi in CORE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay")
    ap.add_argument("--anchor", type=lambda s: int(s, 0), default=DEFAULT_ANCHOR)
    args = ap.parse_args()

    rows = load(args.replay)
    final = {}
    for a, v in rows:
        final[a] = v

    print(f"{args.replay}\n")
    print(f"  MMIO writes            {len(rows):>8}")
    print(f"  distinct addresses     {len(final):>8}   "
          f"({len(rows)/max(len(final),1):.1f}x redundancy)")
    print(f"  non-zero final values  {sum(1 for v in final.values() if v):>8}")

    at = [i for i, (a, _) in enumerate(rows) if a == args.anchor]
    if len(at) < 3:
        print(f"\n  no loop found on anchor {args.anchor:#08x}")
        return
    passes = [tuple(rows[at[k]:at[k + 1]]) for k in range(len(at) - 1)]
    inside = sum(len(p) for p in passes)

    print(f"\n  outer loop on {args.anchor:#08x}: {len(passes)} iterations, "
          f"{inside} writes ({100*inside/len(rows):.0f}% of MMIO)")

    cores = [tuple(w for w in p if in_core(w[0])) for p in passes]
    rests = [tuple(w for w in p if not in_core(w[0])) for p in passes]
    print(f"    L2F+LBS core   {sum(len(c) for c in cores):>7} writes -> "
          f"{len(set(cores))} distinct variant(s)")
    print(f"    remainder      {sum(len(r) for r in rests):>7} writes -> "
          f"{len(set(rests))} distinct")

    rb = collections.Counter()
    for r in rests:
        for a, _ in r:
            rb[a & ~0xFFF] += 1
    print("\n  what the varying remainder touches:")
    for b, n in rb.most_common(8):
        print(f"    {b:06x}xxx {n:>7}")

    keep = sum(len(c) for c in set(cores)) + sum(len(r) for r in rests)
    print(f"\n  writes needed if each core variant is emitted once: "
          f"{keep} of {len(rows)} ({100*keep/len(rows):.0f}%)")
    print("  NOTE: a bound on the generator's output, not a replay you can boot --"
          "\n  ordering has not been shown to be irrelevant. Test on hardware.")


if __name__ == "__main__":
    main()
