#!/usr/bin/env python3
# build_schedule.py - turn an executed bring-up stream into a SCHEDULE plus a
# residual, so the generators can run LIVE in the right order.
#
# WHY THIS EXISTS
#
# alpha64 showed the dataplane comes up with no vendor replay by replaying
# /mnt/flash/bringup.txt -- the deterministic output of the generator
# transformation. That works, but it is a frozen artifact: no generator runs, the
# boot log correctly reports "0 of 90396 executed writes come from our
# generators", and the file carries the vendor's values inline so it cannot be
# redistributed.
#
# The fix is to keep the ORDER and throw away the frozen values. Each generator's
# output appears in the stream as one contiguous block, so the stream decomposes
# into an alternating sequence:
#
#     RES <n>        apply the next n writes from the residual
#     GEN <tool>     run this generator, live, from our own source
#
# A schedule is an ordering of our own writes. It contains no vendor values; the
# only vendor data left is the residual, which is what no generator covers yet.
#
# ⚠ A generator is placed ONLY if its entire -n output matches the stream exactly
# and contiguously. Anything that does not match is left in the residual rather
# than guessed at -- a mis-placed block would reorder the bring-up, and ordering
# is the one thing this whole exercise established you cannot get wrong.
#
# ⚠ Generators applied by direct MMIO (ffuinit, l2linit, hashinit, ffubstinit)
# are legitimately absent: gen_drop removed their lines from the stream and the
# sequence runs them separately, before the replay. Do not try to place them.
#
# SPDX-License-Identifier: GPL-2.0-or-later
import os, glob, argparse

def rows(path):
    o = []
    for line in open(path):
        f = line.split()
        if len(f) == 2:
            try: o.append((int(f[0], 16), int(f[1], 16)))
            except ValueError: pass
    return o

def find_block(stream, block, taken):
    """First index where block occurs contiguously and does not overlap a
    already-placed span. Returns -1 if absent."""
    n, first = len(block), block[0]
    for i in range(len(stream) - n + 1):
        if stream[i] != first: continue
        if any(taken[i:i + n]): continue
        if stream[i:i + n] == block: return i
    return -1


def decompose(stream, index, block, taken):
    """Split a generator's output into contiguous runs of the stream.

    A generator's writes do not always land as one block. fm6000_l2arseq is the
    clear case: the sequence splices its first 25,426 writes before the port loop
    and leaves the remaining 3,684 -- the in-loop bursts -- inside it. Whole-block
    matching cannot place that, so those 3,684 stayed in the residual as vendor
    data even though our own generator produces them byte for byte.

    Returns a list of (stream_pos, first, count) covering the WHOLE output, or
    None. All-or-nothing on purpose: a partially placed generator leaves the rest
    in the residual at a different point in the order, which is how scheduling
    SBus with 11 of 12 segments took a port down."""
    runs, i = [], 0
    while i < len(block):
        best_pos, best_len = -1, 0
        for p in index.get(block[i], ()):
            if taken[p]: continue
            n = 0
            while (i + n < len(block) and p + n < len(stream)
                   and not taken[p + n] and stream[p + n] == block[i + n]):
                n += 1
            if n > best_len: best_pos, best_len = p, n
        if best_len == 0: return None
        runs.append((best_pos, i, best_len))
        for k in range(best_pos, best_pos + best_len): taken[k] = True
        i += best_len
    return runs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stream", help="executed bring-up stream (fwd-executed.txt)")
    ap.add_argument("--gens", required=True, help="dir of <tool>.n ordered write dumps")
    ap.add_argument("--out-schedule", default="schedule.txt")
    ap.add_argument("--out-residual", default="resid.txt")
    a = ap.parse_args()

    ex = rows(a.stream)
    taken = [False] * len(ex)

    # ⚠ PLACE LONGEST BLOCKS FIRST. Placing in name order let a short, highly
    # repetitive segment land INSIDE a longer block's span and block it:
    # fm6000_sbusseq segment 7 is 54 writes occurring 12 times in the stream,
    # and one of those occurrences sits inside segment 8, which is 540 writes
    # occurring exactly ONCE. Greedy name-order placement took the short one
    # first, and segment 8 could then never be placed.
    #
    # That is not a cosmetic loss. A PARTIALLY placed generator is worse than an
    # unplaced one: the segments left behind stay in the residual and run at a
    # different point relative to the generated ones, which reorders the
    # bring-up. Scheduling SBus with 11 of its 12 segments took Et2 DOWN in
    # silicon while the box still forwarded over Et1 -- alpha66.
    #
    # Longest-first fixes it: a long block claims its span before any short
    # block can squat inside it.
    #
    # A dump may be named "<tool>@<arg>@<arg>.n" for a generator that emits only
    # part of its output, which is how the 12 interleaved SBus runs are placed.
    cands = []
    for path in sorted(glob.glob(os.path.join(a.gens, "*.n"))):
        g = rows(path)
        if g: cands.append((os.path.basename(path)[:-2].replace("@", " "), g))
    cands.sort(key=lambda c: -len(c[1]))

    # index the stream by (addr, value) so run decomposition is not quadratic
    index = {}
    for p, w in enumerate(ex): index.setdefault(w, []).append(p)

    SBUS = range(0x00f000, 0x00f010)

    # TWO PASSES, and the order matters. Whole-block placement runs for every
    # candidate FIRST, so a decomposition can never steal a position that a
    # generator would have matched outright.
    placed, leftover = [], []
    for name, g in cands:
        i = find_block(ex, g, taken)
        if i >= 0:
            for k in range(i, i + len(g)): taken[k] = True
            placed.append((i, len(g), name))
        else:
            leftover.append((name, g))

    # Pass 2: cover what is left with several runs, delivered by fm6000_slice.
    #
    # ⚠ A decomposition into many short runs is not a placement, it is a
    # coincidence: short write sequences repeat all over a bring-up stream, and
    # an unconstrained greedy match will happily "cover" a generator with two
    # hundred fragments scattered anywhere they happen to fit. The first version
    # of this did exactly that and made coverage WORSE -- 231 blocks, 85.9% down
    # to 85.3% -- because the fragments displaced real matches.
    #
    # So a decomposition is only accepted if it looks like genuine interleaving:
    # few runs, and no run so short it could match by chance.
    # MIN_RUN is a cost/benefit knob, measured on this stream:
    #     32 -> 15 slices, residual 10,292      <- chosen
    #     64 -> 11 slices, residual 10,533
    #    128 ->  7 slices, residual 10,842
    #    256 ->  3 slices, residual 11,587
    # Below 32 it collapses: MIN_RUN=8 "recovered" 199 runs, but 184 of them were
    # under 32 writes and 183 re-ran fm6000_l2arseq -- whose output is 29,110
    # lines -- for a few writes each. That is 5.3M lines generated and thrown
    # away at boot, and most of those matches are coincidence rather than
    # placement: short write sequences repeat all over a bring-up stream.
    MAX_RUNS, MIN_RUN = 16, 32
    unplaced, sliced = [], 0
    for name, g in leftover:
        # ⚠ Never slice a block containing SBus addresses: fullreplay drives
        # those as transactions with a completion poll, and replaying them
        # literally corrupts the bus. Those blocks have their own generator.
        if any(w[0] in SBUS for w in g) or " " in name:
            unplaced.append((name, len(g)))
            continue
        save = list(taken)
        runs = decompose(ex, index, g, taken)
        if not runs or len(runs) > MAX_RUNS or min(r[2] for r in runs) < MIN_RUN:
            taken[:] = save
            unplaced.append((name, len(g)))
            continue
        for pos, first, count in runs:
            placed.append((pos, count, "fm6000_slice %s %d %d" % (name, first, count)))
        sliced += 1
    # PASS 3: the gaps. A run of residual writes can be an exact SLICE of a
    # generator's output that the bring-up performs a SECOND time. l2arseq is
    # the case that matters: the sequence splices its 29,110 writes before the
    # port loop, and the last 3,684 of them -- the in-loop bursts -- are written
    # AGAIN inside the loop. The generator is fully placed already, so passes 1
    # and 2 see nothing to do, and those 3,684 sat in the residual as vendor data
    # even though our own code produces them byte for byte.
    #
    # So: walk what is still unclaimed and try to cover each run with
    # fm6000_slice <tool> <first> <count>.
    gen_index = {}
    for name, g in cands:
        if " " in name: continue
        for j, w in enumerate(g): gen_index.setdefault(w, []).append((name, g, j))
    pos, gapfill = 0, 0
    while pos < len(ex):
        if taken[pos]: pos += 1; continue
        best = None
        for name, g, j in gen_index.get(ex[pos], ()):
            n = 0
            while (pos + n < len(ex) and j + n < len(g)
                   and not taken[pos + n] and ex[pos + n] == g[j + n]):
                n += 1
            if n >= MIN_RUN and (best is None or n > best[0]):
                best = (n, name, j)
        if best:
            n, name, j = best
            if not any(w[0] in SBUS for w in ex[pos:pos + n]):
                for k in range(pos, pos + n): taken[k] = True
                placed.append((pos, n, "fm6000_slice %s %d %d" % (name, j, n)))
                gapfill += 1
                pos += n
                continue
        pos += 1
    placed.sort()
    steps, resid, run = [], [], 0
    pos = 0
    for start, length, name in placed:
        gap = ex[pos:start]
        for w in gap: resid.append(w)
        if gap: steps.append("RES %d" % len(gap))
        steps.append("GEN %s" % name)
        pos = start + length
    tail = ex[pos:]
    for w in tail: resid.append(w)
    if tail: steps.append("RES %d" % len(tail))

    with open(a.out_schedule, "w") as f:
        f.write("# generated by build_schedule.py -- ordering only, no vendor values\n")
        f.write("# steps: RES <n> = next n writes from the residual; GEN <tool> = run it live\n")
        for s in steps: f.write(s + "\n")
    with open(a.out_residual, "w") as f:
        for addr, val in resid: f.write("%08x %08x\n" % (addr, val))

    gen_writes = sum(l for _, l, _ in placed)
    print("stream            : %d writes" % len(ex))
    print("generator blocks  : %d, covering %d writes (%.1f%%)"
          % (len(placed), gen_writes, 100.0 * gen_writes / len(ex)))
    print("residual          : %d writes (%.1f%%) -- the only vendor data left"
          % (len(resid), 100.0 * len(resid) / len(ex)))
    print("schedule          : %d steps" % len(steps))
    if sliced or gapfill:
        print("slices            : %d split generators, %d gap runs recovered"
              % (sliced, gapfill))
    if unplaced:
        # Never silent. An unplaced generator means its writes are still vendor
        # data in the residual; a PARTIALLY placed one is a live hazard.
        print("\nunplaced (%d) -- these writes remain vendor data:" % len(unplaced))
        for name, n in sorted(unplaced, key=lambda u: -u[1])[:10]:
            print("   %-34s %d writes" % (name, n))
        fam_placed = {p[2].split()[0] for p in placed}
        partial = sorted({n.split()[0] for n, _ in unplaced} & fam_placed)
        if partial:
            print("\n⚠ PARTIALLY PLACED, DO NOT SHIP THIS SCHEDULE: %s" % ", ".join(partial))
    print("\nwrote %s and %s" % (a.out_schedule, a.out_residual))

if __name__ == "__main__":
    main()
