#!/usr/bin/env python3
# counter-probe.py - analyse counter-probe.sh output.
#
# A counter is reported for a traffic class only if it moved in EVERY repetition
# of that class and in NO idle window of any class. That is the control the
# hand-rolled attempts lacked.
#
# SPDX-License-Identifier: GPL-2.0-or-later
import sys, collections, argparse

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
        if not key or not line: continue
        f = line.split()
        if len(f) != 3: continue
        try:
            runs[key][int(f[0], 16)] = int(f[2], 16) - int(f[1], 16)
        except ValueError:
            pass
    return runs

def idx(a):
    return "b%d.%d" % ((a - 0x200000) // 0x1000, ((a - 0x200000) % 0x1000) // 2)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    a = ap.parse_args()
    runs = parse(a.file)
    labels, reps = set(), set()
    for rep, label, _ in runs: labels.add(label); reps.add(rep)

    noisy = set()
    for (rep, label, kind), d in runs.items():
        if kind == "IDLE": noisy |= set(d)

    print("repetitions: %d   classes: %s" % (len(reps), ", ".join(sorted(labels))))
    print("counters excluded for moving at idle: %d\n" % len(noisy))

    clean = {}
    for label in sorted(labels):
        per = [set(d) - noisy for (r, l, k), d in runs.items() if l == label and k == "RUN"]
        if not per: continue
        stable = set.intersection(*per) if per else set()
        clean[label] = stable
        amounts = collections.defaultdict(list)
        for (r, l, k), d in runs.items():
            if l == label and k == "RUN":
                for x in stable: amounts[x].append(d[x])
        print("%-10s %d counters move in all %d reps" % (label, len(stable), len(per)))
        for x in sorted(stable)[:10]:
            v = amounts[x]
            same = "consistent" if len(set(v)) == 1 else "varies %s" % v
            print("    %-10s delta %s   %s" % (idx(x), v[0], same))
        if len(stable) > 10: print("    ... %d more" % (len(stable) - 10))
        print()
    if len(clean) > 1:
        print("discriminating counters (unique to one class):")
        for label, st in clean.items():
            others = set().union(*[s for l, s in clean.items() if l != label]) if len(clean) > 1 else set()
            u = sorted(st - others)
            print("  ONLY %-10s %d: %s" % (label, len(u), " ".join(idx(x) for x in u[:10])))

if __name__ == "__main__":
    main()
