#!/usr/bin/env python3
# counter-probe.py - analyse counter-probe.sh output.
#
# Two modes, because they answer different questions:
#
#   strict    A counter is reported for a class only if it moved in EVERY
#             repetition of that class and in NO idle window of any class.
#             This is the control the hand-rolled attempts lacked -- but it
#             CANNOT see receive-side counters. An idle switch on a live
#             network still receives OSPF hellos, ARP and broadcast, so every
#             RX counter moves at idle and is discarded. Absence of an RX
#             counter under --mode strict is not evidence; it is the filter.
#
#   baseline  Subtract each class's own idle delta from its run delta and
#             require the excess to clear --min in every repetition. This sees
#             through steady background traffic. It is noisier: a counter with
#             a bursty rather than steady idle rate can clear the threshold on
#             background alone, so treat single-rep survivors with suspicion.
#
# SPDX-License-Identifier: GPL-2.0-or-later
import sys, collections, argparse

# STATS_BANK_COUNTER: 0x200000, [2048 x 16], word = base + bank*0x1000 + i*2
STATS_BASE, STATS_BANKS, BANK_STRIDE = 0x200000, 16, 0x1000
STATS_END = STATS_BASE + STATS_BANKS * BANK_STRIDE

def parse(path):
    runs = collections.defaultdict(dict)   # (rep,label,kind) -> {addr: delta}
    key = None
    for line in open(path):
        line = line.strip()
        if line.startswith("==="):
            _, rep, label, kind = line.split()
            key = (rep, label, kind)
            runs.setdefault(key, {})
            continue
        if line.startswith("#"):
            print(line, file=sys.stderr)
            continue
        if not key or not line: continue
        f = line.split()
        if len(f) != 3: continue
        try:
            runs[key][int(f[0], 16)] = int(f[2], 16) - int(f[1], 16)
        except ValueError:
            pass
    return runs

def idx(a):
    """Name a counter. The bank formula is only meaningful inside the stats
    bank; outside it, say so rather than printing a nonsense bank number."""
    if STATS_BASE <= a < STATS_END:
        return "b%d.%d" % ((a - STATS_BASE) // BANK_STRIDE,
                           ((a - STATS_BASE) % BANK_STRIDE) // 2)
    return "@%06x" % a

def collect(runs, label, kind):
    """{addr: [delta per rep]} for one class and window kind."""
    out = collections.defaultdict(list)
    for (rep, l, k), d in runs.items():
        if l == label and k == kind:
            for a, v in d.items(): out[a].append(v)
    return out

def show(label, per_rep, nreps, limit):
    print("%-10s %d counters" % (label, len(per_rep)))
    for x in sorted(per_rep)[:limit]:
        v = per_rep[x]
        same = "consistent" if len(set(v)) == 1 else "varies %s" % v
        print("    %-10s delta %-6s %s" % (idx(x), v[0], same))
    if len(per_rep) > limit: print("    ... %d more" % (len(per_rep) - limit))
    print()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--mode", choices=("strict", "baseline"), default="strict")
    ap.add_argument("--min", type=int, default=8,
                    help="baseline mode: run-minus-idle must exceed this in every rep")
    ap.add_argument("--limit", type=int, default=10)
    a = ap.parse_args()

    runs = parse(a.file)
    labels = sorted({l for _, l, _ in runs})
    reps = {r for r, _, _ in runs}
    nreps = len(reps)
    print("repetitions: %d   classes: %s   mode: %s" % (nreps, ", ".join(labels), a.mode))

    clean = {}
    if a.mode == "strict":
        noisy = set()
        for (rep, label, kind), d in runs.items():
            if kind == "IDLE": noisy |= set(d)
        print("counters excluded for moving at idle: %d" % len(noisy))
        print("NOTE: strict mode cannot see receive-side counters -- see header.\n")
        for label in labels:
            per = [set(d) - noisy for (r, l, k), d in runs.items()
                   if l == label and k == "RUN"]
            if not per: continue
            stable = set.intersection(*per)
            amounts = {x: v for x, v in collect(runs, label, "RUN").items() if x in stable}
            clean[label] = stable
            show(label, amounts, nreps, a.limit)
    else:
        print("threshold: run-minus-idle > %d in every rep\n" % a.min)
        for label in labels:
            idle = collect(runs, label, "IDLE")
            run = collect(runs, label, "RUN")
            nrun = max((len(v) for v in run.values()), default=0)
            excess = {}
            for x, rv in run.items():
                iv = idle.get(x, [])
                base = sum(iv) / len(iv) if iv else 0.0
                e = [round(v - base) for v in rv]
                if len(rv) == nrun and nrun and all(v > a.min for v in e):
                    excess[x] = e
            clean[label] = set(excess)
            show(label, excess, nreps, a.limit)

    if len(clean) > 1:
        print("discriminating counters (unique to one class):")
        for label in labels:
            others = set().union(*[s for l, s in clean.items() if l != label])
            u = sorted(clean[label] - others)
            print("  ONLY %-10s %d: %s" % (label, len(u),
                                           " ".join(idx(x) for x in u[:a.limit])))
        shared = set.intersection(*clean.values()) if clean else set()
        print("  shared by all %d classes: %d" % (len(clean), len(shared)))

if __name__ == "__main__":
    main()
