#!/usr/bin/env python3
"""gen_esched.py - author the egress scheduler's per-port config.

The 104 write-once ESCHED addresses `fm6000_eschedinit` leaves uncovered. Unlike
every other generator in this tree, this one carries **no value table**: the rule
is small enough to state outright, and stating it is the point.

    for each of the 52 front-panel data ports:
        ESCHED_CFG_1[port] = 0xffffff
        ESCHED_CFG_2[port] = 0xffffff

★ THE PORT SET IS DERIVED, NOT LISTED. Both registers are 76 entries of 1 word
(SDK geometry, sdk_regmap.py). The replay writes 52 of them, and that set is
**exactly the configured-port set minus {0, 1, 3}** -- i.e. the front-panel data
ports 20-47 and 52-75, with the CPU and management ports left out. That is what
you would expect of an EGRESS SCHEDULER: ports 0, 1 and 3 are not front-panel
data ports and are not scheduled the same way. The set is computed from
ACTIVE_PORTS below and checked against the replay by --verify, so if the
hypothesis is wrong the check fails rather than the value table quietly hiding it.

★ ONE VALUE, EVERYWHERE. All 104 writes are 0xffffff -- 24 bits, all set. A
scheduler credit/quantum field at its maximum, i.e. every front-panel port
unshaped and equal. There is nothing per-port about the values at all, which is
why a table would have been 104 lines of noise obscuring a one-line rule.

⚠ PORT 0 IS DELIBERATELY NOT OURS. It is written 37 times (CFG_1) and 61 times
(CFG_2), always with the same value (0xfff800 / 0xfff000 -- note NOT 0xffffff,
the CPU port is shaped differently). Idempotent repeats, so collapsing them would
be harmless in principle, but claiming the address makes gen_list splice all of
them out of the replay and this generator would then have to own the CPU port's
scheduling too. Out of scope; it stays in the replay. --counts enforces it.

usage:
    gen_esched.py --emit | --addrs | --structure | --verify <img>
                  | --counts <img> | --c FILE
SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import load  # noqa: E402

# base, words, axes -- from the SDK register descriptor table (sdk_regmap.py)
GEOM = {
    "FM6000_ESCHED_CFG_1": (0x002000, 1, [(76, 1)]),
    "FM6000_ESCHED_CFG_2": (0x002080, 1, [(76, 1)]),
}

# The configured-port set the CM watermarks, MAPPER, LBS_CAM and
# PARSER_INIT_FIELDS all independently agree on.
ACTIVE_PORTS = [0, 1, 3] + list(range(20, 48)) + list(range(52, 76))

# Ports that exist but are not front-panel data ports: 0 is the CPU port, 1 and 3
# are management. An egress scheduler does not schedule them like a data port --
# and the replay confirms it by writing 0xfff800/0xfff000 to port 0 rather than
# the uniform value below.
NOT_FRONT_PANEL = {0, 1, 3}

FRONT_PANEL = [p for p in ACTIVE_PORTS if p not in NOT_FRONT_PANEL]

# 24 bits all set: the scheduler credit field at maximum, every port unshaped.
ESCHED_CFG_VALUE = 0xffffff


def build_seq():
    seq = []
    for reg in sorted(GEOM, key=lambda r: GEOM[r][0]):
        base, _w, ax = GEOM[reg]
        for p in FRONT_PANEL:
            if p >= ax[0][0]:
                raise AssertionError(f"{reg}: port {p} beyond {ax[0][0]}")
            seq.append((base + p * ax[0][1], ESCHED_CFG_VALUE))
    return seq


def build():
    return {a: v for a, v in build_seq()}


def structure():
    print(f"front-panel ports: {len(FRONT_PANEL)}  "
          f"({FRONT_PANEL[0]}-{FRONT_PANEL[27]}, {FRONT_PANEL[28]}-{FRONT_PANEL[-1]})")
    print(f"excluded (not front-panel): {sorted(NOT_FRONT_PANEL)}")
    for reg in sorted(GEOM, key=lambda r: GEOM[r][0]):
        base, w, ax = GEOM[reg]
        print(f"{reg[7:]:20s} 0x{base:06x} w={w} [{ax[0][0]}@{ax[0][1]:#x}]  "
              f"{len(FRONT_PANEL)} writes of 0x{ESCHED_CFG_VALUE:06x}")
    print(f"\ntotal {len(build_seq())} writes over {len(build())} addresses")
    return 0


def counts(image):
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
    """Also checks the DERIVED port set: an address we emit that the replay never
    wrote means FRONT_PANEL is wrong, which is the hypothesis under test."""
    eos, ours = load(image), build()
    same = bad = missing = 0
    for a in sorted(ours):
        e = eos.get(a)
        if e is None:
            print(f"  0x{a:06x} emitted but ABSENT from the replay "
                  f"-- derived port set is wrong")
            missing += 1
        elif e == ours[a]:
            same += 1
        else:
            if bad < 8:
                print(f"  0x{a:06x}  eos={e:08x}  ours={ours[a]:08x}")
            bad += 1
    # and the converse: a write-once ESCHED address we did NOT emit
    extra = 0
    for reg in GEOM:
        base, _w, ax = GEOM[reg]
        for p in range(ax[0][0]):
            a = base + p
            if a in eos and a not in ours and eos[a] == ESCHED_CFG_VALUE:
                print(f"  0x{a:06x} (port {p}) has our value but we do not emit it")
                extra += 1
    print(f"identical {same}, differing {bad}, absent {missing}, unclaimed {extra}")
    ok = not (bad or missing or extra)
    print("VERIFY PASS" if ok else "VERIFY FAIL")
    return 0 if ok else 1


C_HEAD = r"""/* fm6000_esched.c - egress scheduler per-port config.
 *
 * GENERATED by asic/fm6000/tools/gen_esched.py --c. Edit the generator.
 *
 * The 104 write-once ESCHED addresses fm6000_eschedinit leaves uncovered. The
 * whole block is one rule:
 *
 *     for each of the 52 front-panel data ports (20-47, 52-75):
 *         ESCHED_CFG_1[port] = 0xffffff
 *         ESCHED_CFG_2[port] = 0xffffff
 *
 * The port set is the configured-port set MINUS {0, 1, 3} -- the CPU and
 * management ports, which an egress scheduler does not schedule like a data
 * port. 0xffffff is 24 bits all set: the credit field at maximum, every
 * front-panel port unshaped and equal.
 *
 * ⚠ Port 0 is NOT ours. The replay writes it 37/61 times with 0xfff800/0xfff000
 * -- a different, shaped value -- and it stays in the replay.
 *
 * Verified byte-identical against the executed image: 104 of 104, and the
 * derived port set checked in both directions.
 *
 * usage: fm6000_esched [-n | -a] [-b bdf]
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

#define ESCHED_CFG_1 0x002000u
#define ESCHED_CFG_2 0x002080u
#define ESCHED_CFG_VALUE 0x00ffffffu

/* the 52 front-panel data ports */
static const unsigned char front_panel[] = {
"""

C_TAIL = r"""};

static volatile uint32_t *M;

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	int dry = 0, list = 0, i, k, n = 0;
	static const uint32_t base[2] = { ESCHED_CFG_1, ESCHED_CFG_2 };

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

	for (k = 0; k < 2; k++) {
		for (i = 0; i < (int)sizeof front_panel; i++) {
			uint32_t a = base[k] + front_panel[i];
			if (list)      printf("%08x\n", a);
			else if (dry)  printf("%08x %08x\n", a, ESCHED_CFG_VALUE);
			else         { M[a] = ESCHED_CFG_VALUE; __sync_synchronize(); }
			n++;
		}
	}
	if (!dry && !list)
		fprintf(stderr, "fm6000_esched: %d writes\n", n);
	return 0;
}
"""


def emit_c(path):
    with open(path, "w") as f:
        f.write(C_HEAD)
        for i in range(0, len(FRONT_PANEL), 12):
            f.write("\t" + " ".join("%d," % p for p in FRONT_PANEL[i:i + 12]) + "\n")
        f.write(C_TAIL)
    print("wrote %s: %d writes" % (path, len(build_seq())), file=sys.stderr)
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
    for ad, v in build_seq():
        print(f"{ad:08x}" if a.addrs else f"{ad:08x} {v:08x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
