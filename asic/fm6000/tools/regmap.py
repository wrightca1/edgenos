#!/usr/bin/env python3
"""regmap.py - decode FM6000 register addresses into named registers + indices.

This is the missing semantic layer. `fwd4.txt` is addresses and values with no
meaning attached, and every technique so far has worked on its *shape* --
redundancy, loop structure, pre/post-loop position. That took us to ~93%, and
what is left resists shape analysis because the remaining writes are control
registers whose behaviour we do not know.

The address arithmetic turns out not to need reverse engineering at all. The
FM6000 register header carries 632 parameterised macros of the form

    FM6000_CM_PORT_RXMP_PRIVATE_WM(index1, index0)
        ((0x00010) * (index1) + (0x00001) * (index0) + (0x02800) + FM6000_CM_BASE)

plus 458 `_ENTRIES*` bounds and per-field bit positions. Enumerating those gives
address -> (register, indices) for the whole chip.

As a check on both sides: the CM formula above is exactly what was recovered by
hand from the trace (0x112800, 76 ports x 12, stride 16) before the header was
consulted, and the EPL macros' (0x400 * bank + 0x80 * lane) independently
confirms the EPL lane decode in epl_decode.py.

⚠ PROVENANCE. The header is marked INTEL CONFIDENTIAL and lives only in the
private notes repo. This tool READS it at runtime and never embeds it; nothing
Intel-authored enters this repository. Register names and index arithmetic are
interface facts, each independently confirmable from our own traces -- which is
how several of them were found in the first place.

Usage:
    regmap.py --header <fm6000_api_regs_int.h> --addr 0x112810
    regmap.py --header <h> --annotate <replay.txt> [--only-unknown]
"""
import argparse
import collections
import re
import sys

MACRO = re.compile(r'^#define\s+FM6000_([A-Z0-9_]+)\(([^)]*)\)\s+(.+)$')
# Scalar registers: no index, just an offset from a block base. These were
# missed entirely by the first version, which only enumerated the parameterised
# macros -- so several thousand writes came back as "unnamed" when the header
# names them perfectly well.
SCALAR = re.compile(r'^#define\s+FM6000_([A-Z0-9_]+)\s+'
                    r'\(\((0x[0-9a-fA-F]+)\)\s*\+\s*\(FM6000_([A-Z0-9_]+_BASE)\)\)')
# _BASE/_SIZE are hex; _ENTRIES/_WIDTH are DECIMAL. Accept both -- requiring
# 0x silently dropped every bound and left the whole map empty.
PLAIN = re.compile(r'^#define\s+FM6000_([A-Z0-9_]+)\s+(0x[0-9a-fA-F]+|\d+)\s*$')
TERM = re.compile(r'\(0x([0-9a-fA-F]+)\)\s*\*\s*\(\s*\(([a-z0-9]+)\)')


def parse(path):
    """Return (consts, regs). regs: name -> (strides, offset, base, nidx)."""
    consts, regs, raw, scalars = {}, {}, {}, {}
    for line in open(path, errors="ignore"):
        m = SCALAR.match(line)
        if m:
            scalars[m.group(1)] = (int(m.group(2), 16), m.group(3))
            continue
        m = PLAIN.match(line)
        if m:
            t = m.group(2)
            consts[m.group(1)] = int(t, 16) if t.startswith("0x") else int(t)
            continue
        m = MACRO.match(line)
        if m:
            raw[m.group(1)] = (m.group(2), m.group(3))

    for name, (args, body) in raw.items():
        # strides, in the order the index names appear in the arg list
        strides = {v: int(k, 16) for k, v in TERM.findall(body)}
        argnames = [a.strip() for a in args.split(",")]
        # The constant offset: bare (0x...) terms NOT multiplied by an index.
        # The negative lookahead matters -- without it a stride like
        # "(0x00001) * ((index0)..." is also counted as an offset, which shifted
        # every single-stride register by one word and silently produced a map
        # that decoded nothing.
        off = 0
        for c in re.findall(r'\+\s*\(0x([0-9a-fA-F]+)\)(?!\s*\*)', body):
            off += int(c, 16)
        bm = re.search(r'FM6000_([A-Z0-9_]+_BASE)', body)
        if not bm or bm.group(1) not in consts:
            continue
        base = consts[bm.group(1)]
        idx = [a for a in argnames if a in strides]
        if not idx:
            continue
        bounds = []
        for i, a in enumerate(idx):
            n = (consts.get(f"{name}_ENTRIES_{len(idx)-1-i}")
                 or consts.get(f"{name}_ENTRIES")
                 or consts.get(f"{name}_ENTRIES_{i}"))
            bounds.append(n)
        width = consts.get(f"{name}_WIDTH", 1)
        regs[name] = ([strides[a] for a in idx], off, base, bounds, width, idx)

    # resolve scalars once the bases are known
    for name, (off, basename) in scalars.items():
        if basename in consts and name not in regs:
            regs[name] = ([], off, consts[basename], [], 1, [])
    return consts, regs


def build_index(regs, limit=4_000_000):
    """address -> (name, indices). Skips registers with unknown bounds."""
    amap = {}
    for name, (strides, off, base, bounds, width, idx) in regs.items():
        if any(b is None for b in bounds):
            continue
        if not strides:                      # scalar register
            amap.setdefault(base + off, (name, (), 0))
        elif len(strides) == 1:
            n0 = bounds[0]
            if n0 * width > limit:
                continue
            for i in range(n0):
                for w in range(width):
                    amap.setdefault(base + off + strides[0] * i + w, (name, (i,), w))
        elif len(strides) == 2:
            n1, n0 = bounds[0], bounds[1]
            if n1 * n0 * width > limit:
                continue
            for i1 in range(n1):
                for i0 in range(n0):
                    for w in range(width):
                        amap.setdefault(base + off + strides[0] * i1 + strides[1] * i0 + w,
                                        (name, (i1, i0), w))
    return amap


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--header", required=True)
    ap.add_argument("--addr")
    ap.add_argument("--annotate")
    ap.add_argument("--only-unknown", action="store_true")
    a = ap.parse_args()

    consts, regs = parse(a.header)
    print(f"# {len(regs)} indexed registers, {len(consts)} constants", file=sys.stderr)
    amap = build_index(regs)
    print(f"# {len(amap)} addresses decoded", file=sys.stderr)

    if a.addr:
        v = int(a.addr, 0)
        hit = amap.get(v)
        print(f"{v:#08x} = {hit[0]}{list(hit[1])} word {hit[2]}" if hit
              else f"{v:#08x} = (not in the indexed map)")
        return

    if a.annotate:
        hits = collections.Counter()
        miss = collections.Counter()
        for line in open(a.annotate):
            p = line.split()
            if len(p) < 2:
                continue
            try:
                addr = int(p[0], 16)
            except ValueError:
                continue
            h = amap.get(addr)
            (hits if h else miss)[h[0] if h else f"{addr & ~0xFFF:#08x}"] += 1
        tot = sum(hits.values()) + sum(miss.values())
        print(f"decoded {sum(hits.values())} of {tot} writes "
              f"({sum(hits.values())/max(tot,1)*100:.1f}%)\n")
        src = miss if a.only_unknown else hits
        label = "UNDECODED, by 4K page" if a.only_unknown else "decoded registers"
        print(f"{label}:")
        for k, n in src.most_common(30):
            print(f"  {k:<44}{n:>8}")


if __name__ == "__main__":
    main()
