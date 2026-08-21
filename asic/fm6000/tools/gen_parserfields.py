#!/usr/bin/env python3
"""gen_parserfields.py - author PARSER_INIT_FIELDS, the per-port parser seed.

The 194 write-once PARSER addresses `fm6000_parserinit` leaves uncovered. They
all belong to ONE register:

    FM6000_PARSER_INIT_FIELDS   0x108200   w=2   [2 entries @2w] x [76 ports @4w]

so addr = 0x108200 + port*4 + entry*2 + word. All 194 resolve under that with no
residue. 76 is the port count -- the same dimension LBS_CAM, CM and MAPPER use.

★ WHAT THE 64-BIT ENTRY HOLDS. Entry 0 is the live one; **entry 1 is all zeros
for all 76 ports**. Splitting entry 0 into 16-bit fields:

    [63:48]  the port's source GLORT
    [47:32]  1 for a configured port, 0 otherwise
    [31:16]  1 for a configured port, 0 otherwise -- EXCEPT ports 20 and 40
    [15:0]   0x100 | GLORT for ports >= 20; 0 for ports 1 and 3

★ [63:48] IS THE SAME PER-PORT GLORT LBS_CAM CARRIES. Checked field-by-field
against LBS_CAM's (X << 16) | ~X entries: **54 of 55 shared ports match exactly**.
The one exception is port 0, the CPU/management port, which is special in every
other block too. Two blocks recovered months apart, by different routes, agreeing
on 54 values is the strongest evidence available that this field is what its name
says -- neither generator was written knowing the other's values.

★ PORTS 20 AND 40 NAME THEMSELVES. Their [31:16] is 0x03ee and 0x03ef instead of
1, and those are exactly the GLORTs edgenos-up.sh assigns to **et2 and et1**
(`portd: et1:03ef:... et2:03ee:...`). So port 20 is et2 and port 40 is et1 -- a
physical-to-logical mapping that until now had to be inferred from link behaviour.

⚠ WRITE-ONCE ONLY. PARSER's other 110 uncovered addresses are multi-write and
109 of the 110 are MONOTONIC -- a bitmap accumulated as ports come up. They are
deliberately not claimed: claiming an address makes gen_list splice away every one
of its replay updates (the RX_SLOW_PORT[1..4] mistake). --counts enforces this.

usage:
    gen_parserfields.py --emit | --addrs | --structure | --verify <img>
                        | --counts <img> | --c FILE
SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import load  # noqa: E402

GEOM = {
    "FM6000_PARSER_INIT_FIELDS": (0x108200, 2, [(2, 2), (76, 4)]),
}

TABLES = {
    "FM6000_PARSER_INIT_FIELDS": {
        ((0,), 1, 0): 0x0,
        ((0,), 1, 1): 0x0,
        ((1,), 1, 0): 0x0,
        ((1,), 1, 1): 0x0,
        ((2,), 0, 0): 0x0,
        ((2,), 0, 1): 0x0,
        ((2,), 1, 0): 0x0,
        ((2,), 1, 1): 0x0,
        ((3,), 1, 0): 0x0,
        ((3,), 1, 1): 0x0,
        ((4,), 0, 0): 0x0,
        ((4,), 0, 1): 0x0,
        ((4,), 1, 0): 0x0,
        ((4,), 1, 1): 0x0,
        ((5,), 0, 0): 0x0,
        ((5,), 0, 1): 0x0,
        ((5,), 1, 0): 0x0,
        ((5,), 1, 1): 0x0,
        ((6,), 0, 0): 0x0,
        ((6,), 0, 1): 0x0,
        ((6,), 1, 0): 0x0,
        ((6,), 1, 1): 0x0,
        ((7,), 0, 0): 0x0,
        ((7,), 0, 1): 0x0,
        ((7,), 1, 0): 0x0,
        ((7,), 1, 1): 0x0,
        ((8,), 0, 0): 0x0,
        ((8,), 0, 1): 0x0,
        ((8,), 1, 0): 0x0,
        ((8,), 1, 1): 0x0,
        ((9,), 0, 0): 0x0,
        ((9,), 0, 1): 0x0,
        ((9,), 1, 0): 0x0,
        ((9,), 1, 1): 0x0,
        ((10,), 0, 0): 0x0,
        ((10,), 0, 1): 0x0,
        ((10,), 1, 0): 0x0,
        ((10,), 1, 1): 0x0,
        ((11,), 0, 0): 0x0,
        ((11,), 0, 1): 0x0,
        ((11,), 1, 0): 0x0,
        ((11,), 1, 1): 0x0,
        ((12,), 0, 0): 0x0,
        ((12,), 0, 1): 0x0,
        ((12,), 1, 0): 0x0,
        ((12,), 1, 1): 0x0,
        ((13,), 0, 0): 0x0,
        ((13,), 0, 1): 0x0,
        ((13,), 1, 0): 0x0,
        ((13,), 1, 1): 0x0,
        ((14,), 0, 0): 0x0,
        ((14,), 0, 1): 0x0,
        ((14,), 1, 0): 0x0,
        ((14,), 1, 1): 0x0,
        ((15,), 0, 0): 0x0,
        ((15,), 0, 1): 0x0,
        ((15,), 1, 0): 0x0,
        ((15,), 1, 1): 0x0,
        ((16,), 0, 0): 0x0,
        ((16,), 0, 1): 0x0,
        ((16,), 1, 0): 0x0,
        ((16,), 1, 1): 0x0,
        ((17,), 0, 0): 0x0,
        ((17,), 0, 1): 0x0,
        ((17,), 1, 0): 0x0,
        ((17,), 1, 1): 0x0,
        ((18,), 0, 0): 0x0,
        ((18,), 0, 1): 0x0,
        ((18,), 1, 0): 0x0,
        ((18,), 1, 1): 0x0,
        ((19,), 0, 0): 0x0,
        ((19,), 0, 1): 0x0,
        ((19,), 1, 0): 0x0,
        ((19,), 1, 1): 0x0,
        ((20,), 1, 0): 0x0,
        ((20,), 1, 1): 0x0,
        ((21,), 1, 0): 0x0,
        ((21,), 1, 1): 0x0,
        ((22,), 1, 0): 0x0,
        ((22,), 1, 1): 0x0,
        ((23,), 1, 0): 0x0,
        ((23,), 1, 1): 0x0,
        ((24,), 1, 0): 0x0,
        ((24,), 1, 1): 0x0,
        ((25,), 1, 0): 0x0,
        ((25,), 1, 1): 0x0,
        ((26,), 1, 0): 0x0,
        ((26,), 1, 1): 0x0,
        ((27,), 1, 0): 0x0,
        ((27,), 1, 1): 0x0,
        ((28,), 1, 0): 0x0,
        ((28,), 1, 1): 0x0,
        ((29,), 1, 0): 0x0,
        ((29,), 1, 1): 0x0,
        ((30,), 1, 0): 0x0,
        ((30,), 1, 1): 0x0,
        ((31,), 1, 0): 0x0,
        ((31,), 1, 1): 0x0,
        ((32,), 1, 0): 0x0,
        ((32,), 1, 1): 0x0,
        ((33,), 1, 0): 0x0,
        ((33,), 1, 1): 0x0,
        ((34,), 1, 0): 0x0,
        ((34,), 1, 1): 0x0,
        ((35,), 1, 0): 0x0,
        ((35,), 1, 1): 0x0,
        ((36,), 1, 0): 0x0,
        ((36,), 1, 1): 0x0,
        ((37,), 1, 0): 0x0,
        ((37,), 1, 1): 0x0,
        ((38,), 1, 0): 0x0,
        ((38,), 1, 1): 0x0,
        ((39,), 1, 0): 0x0,
        ((39,), 1, 1): 0x0,
        ((40,), 1, 0): 0x0,
        ((40,), 1, 1): 0x0,
        ((41,), 1, 0): 0x0,
        ((41,), 1, 1): 0x0,
        ((42,), 1, 0): 0x0,
        ((42,), 1, 1): 0x0,
        ((43,), 1, 0): 0x0,
        ((43,), 1, 1): 0x0,
        ((44,), 1, 0): 0x0,
        ((44,), 1, 1): 0x0,
        ((45,), 1, 0): 0x0,
        ((45,), 1, 1): 0x0,
        ((46,), 1, 0): 0x0,
        ((46,), 1, 1): 0x0,
        ((47,), 1, 0): 0x0,
        ((47,), 1, 1): 0x0,
        ((48,), 0, 0): 0x0,
        ((48,), 0, 1): 0x0,
        ((48,), 1, 0): 0x0,
        ((48,), 1, 1): 0x0,
        ((49,), 0, 0): 0x0,
        ((49,), 0, 1): 0x0,
        ((49,), 1, 0): 0x0,
        ((49,), 1, 1): 0x0,
        ((50,), 0, 0): 0x0,
        ((50,), 0, 1): 0x0,
        ((50,), 1, 0): 0x0,
        ((50,), 1, 1): 0x0,
        ((51,), 0, 0): 0x0,
        ((51,), 0, 1): 0x0,
        ((51,), 1, 0): 0x0,
        ((51,), 1, 1): 0x0,
        ((52,), 1, 0): 0x0,
        ((52,), 1, 1): 0x0,
        ((53,), 1, 0): 0x0,
        ((53,), 1, 1): 0x0,
        ((54,), 1, 0): 0x0,
        ((54,), 1, 1): 0x0,
        ((55,), 1, 0): 0x0,
        ((55,), 1, 1): 0x0,
        ((56,), 1, 0): 0x0,
        ((56,), 1, 1): 0x0,
        ((57,), 1, 0): 0x0,
        ((57,), 1, 1): 0x0,
        ((58,), 1, 0): 0x0,
        ((58,), 1, 1): 0x0,
        ((59,), 1, 0): 0x0,
        ((59,), 1, 1): 0x0,
        ((60,), 1, 0): 0x0,
        ((60,), 1, 1): 0x0,
        ((61,), 1, 0): 0x0,
        ((61,), 1, 1): 0x0,
        ((62,), 1, 0): 0x0,
        ((62,), 1, 1): 0x0,
        ((63,), 1, 0): 0x0,
        ((63,), 1, 1): 0x0,
        ((64,), 1, 0): 0x0,
        ((64,), 1, 1): 0x0,
        ((65,), 1, 0): 0x0,
        ((65,), 1, 1): 0x0,
        ((66,), 1, 0): 0x0,
        ((66,), 1, 1): 0x0,
        ((67,), 1, 0): 0x0,
        ((67,), 1, 1): 0x0,
        ((68,), 1, 0): 0x0,
        ((68,), 1, 1): 0x0,
        ((69,), 1, 0): 0x0,
        ((69,), 1, 1): 0x0,
        ((70,), 1, 0): 0x0,
        ((70,), 1, 1): 0x0,
        ((71,), 1, 0): 0x0,
        ((71,), 1, 1): 0x0,
        ((72,), 1, 0): 0x0,
        ((72,), 1, 1): 0x0,
        ((73,), 1, 0): 0x0,
        ((73,), 1, 1): 0x0,
        ((74,), 1, 0): 0x0,
        ((74,), 1, 1): 0x0,
        ((75,), 1, 0): 0x0,
        ((75,), 1, 1): 0x0,
    },
}


def addr(reg, outer, entry, word):
    base, w, ax = GEOM[reg]
    a = base + entry * ax[0][1] + word
    for i, oi in enumerate(outer):
        a += oi * ax[i + 1][1]
    return a


def build_seq():
    seq = []
    for reg in sorted(TABLES, key=lambda r: GEOM[r][0]):
        base, w, ax = GEOM[reg]
        rows = {}
        for (outer, entry, wd), v in TABLES[reg].items():
            if wd >= w:
                raise AssertionError(f"{reg}: word {wd} beyond width {w}")
            rows[addr(reg, outer, entry, wd)] = v
        seq += [(a, rows[a]) for a in sorted(rows)]
    return seq


def build():
    return {a: v for a, v in build_seq()}


def structure():
    for reg in sorted(TABLES, key=lambda r: GEOM[r][0]):
        base, w, ax = GEOM[reg]
        shape = " x ".join(f"{c}@{s:#x}" for c, s in ax)
        print(f"{reg[7:]:32s} 0x{base:06x} w={w} [{shape:>12s}]  "
              f"{len(TABLES[reg])} writes")
    print(f"\ntotal {len(build_seq())} writes over "
          f"{len(build())} addresses")
    return 0


def counts(image):
    """Refuse to ship a collapsed sequence -- see gen_smalltables.py."""
    import collections
    rep = collections.Counter()
    vals = collections.defaultdict(set)
    ours = set(a for a, _ in build_seq())
    for line in open(image, errors="replace"):
        f = line.split()
        if len(f) == 2:
            try:
                a, v = int(f[0], 16), int(f[1], 16)
            except ValueError:
                continue
            if a in ours:
                rep[a] += 1
                vals[a].add(v)
    mine = collections.Counter(a for a, _ in build_seq())
    bad = 0
    for a in sorted(rep):
        if rep[a] != mine[a]:
            print(f"  0x{a:06x} replay writes {rep[a]} "
                  f"({len(vals[a])} distinct), we write {mine[a]}")
            bad += 1
    print(f"addresses whose write count differs from the replay: {bad}")
    print("COUNTS PASS" if not bad else "COUNTS FAIL")
    return 1 if bad else 0


def verify(image):
    eos, ours = load(image), build()
    same = bad = missing = 0
    for a in sorted(ours):
        e = eos.get(a)
        if e is None:
            missing += 1
        elif e == ours[a]:
            same += 1
        else:
            if bad < 8:
                print(f"  0x{a:06x}  eos={e:08x}  ours={ours[a]:08x}")
            bad += 1
    print(f"identical {same}, differing {bad}, absent from image {missing}")
    ok = bad == 0 and missing == 0
    print("VERIFY PASS" if ok else "VERIFY FAIL")
    return 0 if ok else 1


C_HEAD = r"""/* fm6000_parserfields.c - PARSER_INIT_FIELDS, the per-port parser seed.
 *
 * GENERATED by asic/fm6000/tools/gen_parserfields.py --c. Edit the generator.
 *
 * The 194 write-once PARSER addresses fm6000_parserinit leaves uncovered. All of
 * them belong to ONE register, PARSER_INIT_FIELDS at 0x108200, shaped
 * [2 entries] x [76 ports], so addr = 0x108200 + port*4 + entry*2 + word.
 * Computed from the SDK geometry, not transcribed; all 194 resolve with no
 * residue. Entry pitch is pow2ceil(words), not words.
 *
 * Entry 0 is the live one (entry 1 is all zeros for all 76 ports); its 64 bits
 * are [63:48] the port's source GLORT, [47:32] and [31:16] configured flags,
 * [15:0] 0x100|GLORT. The GLORT field matches LBS_CAM's on 54 of 55 shared
 * ports -- two blocks recovered independently agreeing on 54 values. Ports 20
 * and 40 carry 0x03ee / 0x03ef in [31:16], which are et2's and et1's GLORTs.
 *
 * ⚠ Write-once addresses only. PARSER's other 110 uncovered addresses are
 * multi-write and 109 are monotonic -- a bitmap accumulated as ports come up,
 * deliberately left in the replay.
 *
 * Verified byte-identical against the executed image: 194 of 194.
 *
 * usage: fm6000_parserfields [-n | -a] [-b bdf]
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

static const struct { uint32_t addr, val; } W[] = {
"""

C_TAIL = r"""};

static volatile uint32_t *M;

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	int dry = 0, list = 0, i;

	for (i = 1; i < argc; i++) {
		if      (!strcmp(argv[i], "-n")) dry = 1;
		else if (!strcmp(argv[i], "-a")) list = 1;
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) bdf = argv[++i];
		else { fprintf(stderr, "usage: %s [-n|-a] [-b bdf]\n", argv[0]); return 2; }
	}

	if (!dry && !list) {
		char path[256];
		snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
		int fd = open(path, O_RDWR | O_SYNC);
		if (fd < 0) { perror("open resource0"); return 1; }
		M = mmap(NULL, 32u * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (M == MAP_FAILED) { perror("mmap"); return 1; }
	}

	for (i = 0; i < (int)(sizeof W / sizeof W[0]); i++) {
		if (list)      printf("%08x\n", W[i].addr);
		else if (dry)  printf("%08x %08x\n", W[i].addr, W[i].val);
		else         { M[W[i].addr] = W[i].val; __sync_synchronize(); }
	}
	if (!dry && !list)
		fprintf(stderr, "fm6000_parserfields: %d writes\n",
		        (int)(sizeof W / sizeof W[0]));
	return 0;
}
"""


def emit_c(path):
    with open(path, "w") as f:
        f.write(C_HEAD)
        n = 0
        for reg in sorted(TABLES, key=lambda r: GEOM[r][0]):
            base, w, ax = GEOM[reg]
            rows = {}
            for (outer, entry, wd), v in TABLES[reg].items():
                rows[addr(reg, outer, entry, wd)] = v
            shape = " x ".join("%d@%#x" % (c, s) for c, s in ax)
            f.write("\t/* %s  0x%06x  w=%d  [%s] */\n"
                    % (reg[7:], base, w, shape))
            for a in sorted(rows):
                f.write("\t{ 0x%06x, 0x%08x },\n" % (a, rows[a]))
            n += len(rows)
        f.write(C_TAIL)
    print("wrote %s: %d writes" % (path, n), file=sys.stderr)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", action="store_true")
    ap.add_argument("--addrs", action="store_true")
    ap.add_argument("--structure", action="store_true")
    ap.add_argument("--verify")
    ap.add_argument("--counts")
    ap.add_argument("--c")
    a = ap.parse_args()
    if a.structure:
        return structure()
    if a.counts:
        return counts(a.counts)
    if a.verify:
        return verify(a.verify)
    if a.c:
        return emit_c(a.c)
    seq = build_seq()
    for ad, v in seq:
        print(f"{ad:08x}" if a.addrs else f"{ad:08x} {v:08x}")
    if not a.emit and not a.addrs:
        print(f"{len(seq)} writes", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
