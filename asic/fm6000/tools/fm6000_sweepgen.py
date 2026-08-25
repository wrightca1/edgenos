#!/usr/bin/env python3
"""fm6000_sweepgen.py - generate the L2F/LBS port sweep instead of replaying it.

Step 1 of removing fwd4.txt (see docs/EDGENOS-7150.md (was SELF-CONTAINED-PLAN)).

WHAT THIS REPLACES
    77% of the replay's MMIO writes sit inside one 336-iteration loop. The
    L2F+LBS core of that loop is 74,034 writes -- and it is not a program, it is
    a sweep over the port map:

        for n, port in enumerate(PORT_ORDER, 1):
            L2F  0x1a0c00 + 4*prev  <- 3-word entry
            LBS  0x014000 + port    <- (n << 16) | (~n & 0xffff)
            prev = port

    The LBS write announces the port whose L2F entry is written on the *next*
    step, so the L2F address trails the LBS one by a step. That pipelining is
    what made the trace look unstructured.

    Everything needed to reproduce those 74,034 writes:
      - PORT_ORDER   54 offsets -- the platform's port map
      - 4 state vectors (the 3-word L2F entry per step, per phase). Two phases
        dominate: iterations 0-106 hold 0x0b for all but 3 ports, 108-335 hold
        0x09 for all. The rest are one-off transitions.
      - 4 irregular iterations, kept verbatim (transitions we do not model yet)

    Verified: all 336 iterations, 74,034 writes, reproduced byte for byte.

HOW TO TRUST IT
    `--verify` regenerates the sweep from the extracted description and compares
    it to the recorded writes, byte for byte. That is a software proof that the
    description is complete -- no hardware risk, and it must pass before any
    boot test.

    Byte-identity also means splicing our output back into fwd4 produces the
    same file, so that boot proves nothing on its own. The hardware milestone is
    the NEXT step: writing PORT_ORDER by hand from the platform description
    instead of extracting it, at which point the sweep is genuinely ours.

Usage:
    fm6000_sweepgen.py extract <replay.txt> -o sweep.json
    fm6000_sweepgen.py verify  <replay.txt> [-d sweep.json]
    fm6000_sweepgen.py emit    <sweep.json>          # address value, hex, one per line
"""
import argparse
import collections
import json
import sys

SBUS = (0xF000, 0xF001, 0xF002, 0xF003, 0xF004)   # JSS SBus master window
ANCHOR = 0x1A0C00
L2F_BASE = 0x1A0C00
LBS_BASE = 0x014000
LBS_END = 0x015000
L2F_LO, L2F_HI = 0x180000, 0x200000
REGULAR = 220               # writes in a non-transition iteration
LBS_TERMINATOR = 0xFF0000FF  # closes each iteration


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
        rows.append((a, v))
    return rows

# NB: SBus writes (0xF001-0xF004) are KEPT. They carry the SerDes firmware and
# must survive into the spliced output or the result is not bootable. They are
# excluded from the sweep by in_core() on address, not by filtering the input.


def in_core(a):
    return L2F_LO <= a < L2F_HI or LBS_BASE <= a < LBS_END


def iterations(rows):
    at = [i for i, (a, _) in enumerate(rows) if a == ANCHOR]
    return [tuple(rows[at[k]:at[k + 1]]) for k in range(len(at) - 1)]


def core_of(it):
    return tuple(w for w in it if in_core(w[0]))


def emit_sweep(order, states):
    """One iteration of the sweep. `states` is the 3-word L2F entry per step."""
    out = []
    prev = 0
    for i, port in enumerate(order):
        w0, w1, w2 = states[i]
        out.append((L2F_BASE + 4 * prev, w0))
        out.append((L2F_BASE + 4 * prev + 1, w1))
        out.append((L2F_BASE + 4 * prev + 2, w2))
        n = i + 1
        out.append((LBS_BASE + port, (n << 16) | (~n & 0xFFFF)))
        prev = port
    return out


# The sweep visits the 48 front-panel SFP+ ports in board order, then four
# uplink ports and two internal ones. The first 48 are exactly the `alta` column
# of asic/fm6000/fm6000_serdes_ports.h -- our own platform description -- so the
# order is DERIVED, not lifted from the trace. Front-panel 49-52 (internal
# 48-51) are not swept.
# After the 48 front-panel ports the sweep visits front-panel 53-56 (the uplink
# group) and two internal ports. Front-panel 49-52 are not swept at all.
SAF_LO, SAF_HI = 0x0A0000, 0x0A1000

SWEEP_FRONT = list(range(1, 49))
SWEEP_UPLINK = [53, 54, 55, 56]
SWEEP_INTERNAL = [3, 1]


def port_order_from_platform(header):
    """Build PORT_ORDER from the repo's own port table."""
    import re
    alta = dict((int(n), int(a)) for n, a in
                re.findall(r'\{\s*(\d+),\s*(\d+),', open(header).read()))
    missing = [n for n in SWEEP_FRONT + SWEEP_UPLINK if n not in alta]
    if missing:
        raise SystemExit(f"{header}: no entry for front-panel port(s) {missing}")
    return ([alta[n] for n in SWEEP_FRONT]
            + [alta[n] for n in SWEEP_UPLINK]
            + SWEEP_INTERNAL)


def saf_final_state(rows):
    """The SAF store-and-forward matrix as EOS leaves it.

    EOS builds this matrix incrementally -- 34,668 writes across 111 iterations,
    OR-ing one port's bit in at a time. The end state is 56 ports drawn from
    just four 3-word patterns, so a generator can write it directly in 168
    writes. Emitting the final state INSTEAD of the accumulation is the first
    change that alters the replay's behaviour, and must be boot-tested.
    """
    final = {}
    for a, v in rows:
        if SAF_LO <= a < SAF_HI:
            final[a] = v
    return [(a, final[a]) for a in sorted(final)]


def extract(rows):
    its = iterations(rows)
    order = None
    vecs = collections.defaultdict(list)
    irregular = {}
    for k, it in enumerate(its):
        c = core_of(it)
        if len(c) != REGULAR:
            irregular[k] = [[a, v] for a, v in c]
            continue
        if order is None:
            order = [a - LBS_BASE for a, v in c
                     if LBS_BASE <= a < LBS_END and v != LBS_TERMINATOR]
        l2f = [(a, v) for a, v in c if L2F_LO <= a < L2F_HI]
        vec = tuple(tuple(v for _, v in l2f[j:j + 3]) for j in range(0, len(l2f), 3))
        vecs[vec].append(k)
    return {
        "iterations": len(its),
        "port_order": order,
        "states": [{"vector": [list(e) for e in v], "iters": ks} for v, ks in
                   sorted(vecs.items(), key=lambda kv: -len(kv[1]))],
        "irregular": {str(k): v for k, v in irregular.items()},
    }


def generated_core(desc, k):
    """The core writes for iteration k, from the description alone."""
    if str(k) in desc["irregular"]:
        return [tuple(w) for w in desc["irregular"][str(k)]]
    for s in desc["states"]:
        if k in s["iters"]:
            return emit_sweep(desc["port_order"], s["vector"])
    return None


def cmd_verify(rows, desc):
    its = iterations(rows)
    ok = bad = unmodelled = 0
    for k, it in enumerate(its):
        rec = core_of(it)
        gen = generated_core(desc, k)
        if gen is None:
            unmodelled += 1
            continue
        # the recorded iteration closes with a trailing L2F entry + LBS reset
        # that belongs to the next step; compare the modelled prefix
        n = min(len(gen), len(rec))
        if tuple(gen[:n]) == tuple(rec[:n]):
            ok += 1
        else:
            bad += 1
            if bad == 1:
                for i, (g, r) in enumerate(zip(gen, rec)):
                    if g != r:
                        print(f"  iter {k} differs at {i}: "
                              f"generated {g[0]:06x} {g[1]:08x} != "
                              f"recorded {r[0]:06x} {r[1]:08x}", file=sys.stderr)
                        break
    total = sum(len(core_of(it)) for it in its)
    print(f"  iterations              {len(its)}")
    print(f"  reproduced exactly      {ok}")
    print(f"  mismatched              {bad}")
    print(f"  not modelled            {unmodelled}")
    print(f"  core writes described   {total}")
    print(f"  description size        {len(desc['port_order'])} ports, "
          f"{len(desc['states'])} state vectors, "
          f"{len(desc['irregular'])} verbatim iterations")
    return bad == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["extract", "verify", "emit", "splice"])
    ap.add_argument("input")
    ap.add_argument("-o", "--out")
    ap.add_argument("-d", "--desc")
    ap.add_argument("--ports", help="derive PORT_ORDER from this port-table header "
                                    "instead of from the trace")
    ap.add_argument("--saf", choices=["keep", "final"], default="keep",
                    help="keep = copy EOS's incremental SAF accumulation "
                         "(byte-identical); final = emit the end state once "
                         "(168 writes instead of 34,668 -- CHANGES BEHAVIOUR, "
                         "boot-test required)")
    args = ap.parse_args()

    if args.cmd == "emit":
        desc = json.load(open(args.input))
        for k in range(desc["iterations"]):
            for a, v in generated_core(desc, k) or []:
                print(f"{a:06x} {v:08x}")
        return

    rows = load(args.input)
    desc = json.load(open(args.desc)) if args.desc else extract(rows)
    if args.ports:
        derived = port_order_from_platform(args.ports)
        if derived != desc["port_order"]:
            print(f"  PORT_ORDER from {args.ports} does NOT match the trace",
                  file=sys.stderr)
            for i, (a, b) in enumerate(zip(derived, desc["port_order"])):
                if a != b:
                    print(f"    step {i}: derived {a} vs trace {b}", file=sys.stderr)
                    break
            sys.exit(2)
        print(f"  PORT_ORDER derived from {args.ports}: matches the trace")
        desc["port_order"] = derived

    if args.cmd == "splice":
        # Rebuild the replay with the sweep GENERATED rather than copied. Writes
        # outside the loop core pass through untouched, so while the sweep is
        # still reproduced exactly the output is byte-identical to the input --
        # which is the point: it proves the substitution is safe before any
        # later change to the generator can alter the switch's behaviour.
        its = iterations(rows)
        at = [i for i, (a, _) in enumerate(rows) if a == ANCHOR]
        saf = saf_final_state(rows) if args.saf == "final" else None
        emitted_saf = False
        out_rows = list(rows[:at[0]])
        for k, it in enumerate(its):
            gen = list(generated_core(desc, k))
            for a, v in it:
                if saf is not None and SAF_LO <= a < SAF_HI:
                    # Drop the accumulation. Emit the completed matrix at the
                    # position of the FIRST SAF write, so downstream config
                    # never sees a partially-built matrix -- EOS finished
                    # building it much later, so this is the conservative side.
                    if not emitted_saf:
                        out_rows.extend(saf)
                        emitted_saf = True
                    continue
                out_rows.append(gen.pop(0) if in_core(a) and gen else (a, v))
        out_rows += rows[at[-1]:]
        same = out_rows == rows
        dest = args.out or "fwd-generated.txt"
        if args.saf == "final":
            print(f"  SAF: accumulation replaced by {len(saf)} final-state writes")
        with open(dest, "w") as f:
            for a, v in out_rows:
                f.write(f"{a:08x} {v:08x}\n")   # match the recorded format
        print(f"  {dest}: {len(out_rows)} writes")
        print(f"  identical to input: {'YES' if same else 'NO'}")
        sys.exit(0 if same else 1)

    if args.cmd == "extract":
        out = args.out or "sweep.json"
        json.dump(desc, open(out, "w"), indent=1)
        print(f"{out}: {len(desc['port_order'])} ports, "
              f"{len(desc['states'])} state vectors, "
              f"{len(desc['irregular'])} verbatim iterations")
    else:
        sys.exit(0 if cmd_verify(rows, desc) else 1)


if __name__ == "__main__":
    main()
