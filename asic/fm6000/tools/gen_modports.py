#!/usr/bin/env python3
"""gen_modports.py - author MOD's per-front-panel-port frame settings.

Two registers, no value table, one rule each:

    MOD_MIN_LENGTH[port] = 64      minimum Ethernet frame, in bytes
    MOD_TX_PORT_TAG[port] = 0      Tag=0, PAUSE_Tag=0 -- no egress tagging

for each of the 52 front-panel data ports, written TWICE (the replay writes each
twice with the same value; that is reproduced rather than collapsed, so --counts
matches exactly).

★ THE PORT SET IS DERIVED AND CHECKED BOTH WAYS. Both registers are 76 entries
of 1 word (sdk_regmap.py). The replay's uncovered entries are **exactly** the
configured-port set minus {0, 1, 3} -- and the excluded ports are visibly
different rather than merely absent, which is the useful confirmation:

    MOD_MIN_LENGTH   ports 1, 3   also 64, but written once and owned elsewhere
    MOD_TX_PORT_TAG  ports 0, 1   Tag = 2, NOT 0 -- the CPU and management ports
                                  really are tagged differently

So "front panel" is not an assumption imported from another block; this register
file states it independently. --verify fails if the derived set is wrong, in
either direction.

Field layout from sdk_fieldmap.py:

    MOD_MIN_LENGTH   MinLength[7:0]
    MOD_TX_PORT_TAG  Tag[1:0], PAUSE_Tag[3:2]

usage:
    gen_modports.py --emit | --addrs | --structure | --verify <img>
                    | --counts <img> | --c FILE
SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import load  # noqa: E402

GEOM = {
    "FM6000_MOD_TX_PORT_TAG": (0x15F280, 1, [(76, 1)]),
    "FM6000_MOD_MIN_LENGTH":  (0x15F680, 1, [(76, 1)]),
}

ACTIVE_PORTS = [0, 1, 3] + list(range(20, 48)) + list(range(52, 76))
NOT_FRONT_PANEL = {0, 1, 3}
FRONT_PANEL = [p for p in ACTIVE_PORTS if p not in NOT_FRONT_PANEL]

MIN_FRAME_BYTES = 64                  # MinLength[7:0]
TX_PORT_TAG_NONE = 0                  # Tag=0, PAUSE_Tag=0
WRITES_PER_ENTRY = 2                  # the replay writes each twice

# Ports that legitimately hold one of our values but are NOT ours to write --
# another generator already owns them. Listed explicitly so the both-directions
# check below stays strict: without this it reports them as "unclaimed", which
# is the check doing its job, not a false alarm to be silenced by weakening it.
#   MOD_MIN_LENGTH ports 1 and 3 are also 64, written once, owned elsewhere.
#   MOD_TX_PORT_TAG ports 0 and 1 are Tag=2, so they never collide.
OWNED_ELSEWHERE = {"FM6000_MOD_MIN_LENGTH": {1, 3}}


def build_seq():
    seq = []
    for _ in range(WRITES_PER_ENTRY):
        for reg, val in (("FM6000_MOD_TX_PORT_TAG", TX_PORT_TAG_NONE),
                         ("FM6000_MOD_MIN_LENGTH", MIN_FRAME_BYTES)):
            base, _w, ax = GEOM[reg]
            for p in FRONT_PANEL:
                if p >= ax[0][0]:
                    raise AssertionError(f"{reg}: port {p} beyond {ax[0][0]}")
                seq.append((base + p * ax[0][1], val))
    return seq


def build():
    return {a: v for a, v in build_seq()}


def structure():
    print(f"front-panel ports: {len(FRONT_PANEL)} "
          f"({FRONT_PANEL[0]}-{FRONT_PANEL[27]}, {FRONT_PANEL[28]}-{FRONT_PANEL[-1]})")
    print(f"excluded: {sorted(NOT_FRONT_PANEL)} (owned elsewhere, and different)")
    print(f"  MOD_MIN_LENGTH  = {MIN_FRAME_BYTES} bytes")
    print(f"  MOD_TX_PORT_TAG = 0x{TX_PORT_TAG_NONE:x} (no egress tag)")
    seq = build_seq()
    print(f"\ntotal {len(seq)} writes over {len(build())} addresses")
    return 0


def counts(image):
    import collections
    rep = collections.Counter()
    ours = set(a for a, _ in build_seq())
    for line in open(image, errors="replace"):
        f = line.split()
        if len(f) == 2:
            try:
                a = int(f[0], 16)
            except ValueError:
                continue
            if a in ours:
                rep[a] += 1
    mine = collections.Counter(a for a, _ in build_seq())
    bad = sum(1 for a in rep if rep[a] != mine[a])
    for a in sorted(rep):
        if rep[a] != mine[a]:
            print(f"  0x{a:06x} replay {rep[a]}, ours {mine[a]}")
    print(f"addresses whose write count differs: {bad}")
    print("COUNTS PASS" if not bad else "COUNTS FAIL")
    return 1 if bad else 0


def verify(image):
    """Checks the DERIVED port set in both directions: an address we emit that
    the replay never wrote means FRONT_PANEL is too wide, and an address holding
    our value that we do not emit means it is too narrow."""
    eos, ours = load(image), build()
    same = bad = missing = 0
    for a in sorted(ours):
        e = eos.get(a)
        if e is None:
            print(f"  0x{a:06x} emitted but ABSENT from the replay")
            missing += 1
        elif e == ours[a]:
            same += 1
        else:
            if bad < 6:
                print(f"  0x{a:06x}  eos={e:08x}  ours={ours[a]:08x}")
            bad += 1
    extra = 0
    for reg, val in (("FM6000_MOD_TX_PORT_TAG", TX_PORT_TAG_NONE),
                     ("FM6000_MOD_MIN_LENGTH", MIN_FRAME_BYTES)):
        base, _w, ax = GEOM[reg]
        for p in range(ax[0][0]):
            a = base + p
            if p in OWNED_ELSEWHERE.get(reg, ()):
                continue
            if a in eos and a not in ours and eos[a] == val:
                print(f"  0x{a:06x} (port {p}) has our value but we do not emit it")
                extra += 1
    print(f"identical {same}, differing {bad}, absent {missing}, unclaimed {extra}")
    ok = not (bad or missing or extra)
    print("VERIFY PASS" if ok else "VERIFY FAIL")
    return 0 if ok else 1


C_HEAD = r"""/* fm6000_modports.c - MOD per-front-panel-port frame settings.
 *
 * GENERATED by asic/fm6000/tools/gen_modports.py --c. Edit the generator.
 *
 *   MOD_MIN_LENGTH[port]  = 64   minimum Ethernet frame, in bytes
 *   MOD_TX_PORT_TAG[port] = 0    Tag=0, PAUSE_Tag=0 -- no egress tagging
 *
 * for each of the 52 front-panel data ports, written twice as the replay does.
 * The port set is derived (configured ports minus 0, 1, 3) and checked in both
 * directions; ports 0/1 carry Tag=2 rather than 0, so this register file states
 * the front-panel/CPU split independently rather than importing it.
 *
 * Verified byte-identical against the executed image: 104 of 104 addresses.
 *
 * usage: fm6000_modports [-n | -a] [-b bdf]
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
		fprintf(stderr, "fm6000_modports: %d writes\n",
		        (int)(sizeof W / sizeof W[0]));
	return 0;
}
"""


def emit_c(path):
    seq = build_seq()
    with open(path, "w") as f:
        f.write(C_HEAD)
        for a, v in seq:
            f.write("\t{ 0x%06x, 0x%08x },\n" % (a, v))
        f.write(C_TAIL)
    print("wrote %s: %d writes" % (path, len(seq)), file=sys.stderr)
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
    if a.addrs:
        for ad in sorted(set(x for x, _ in seq)):
            print(f"{ad:08x}")
    else:
        for ad, v in seq:
            print(f"{ad:08x} {v:08x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
