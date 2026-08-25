#!/usr/bin/env python3
"""gen_erl.py - author the egress rate limiter, INCLUDING its two-phase init.

ERL is the first block here whose replay writes cannot be reduced to a final
value at all: every one of its 967 addresses is written **exactly twice**, and
636 of them with two different values. The census that ranked blocks by
authorability (docs/EDGENOS-7150.md (was BLOB-REMOVAL-PLAN)) put ERL at zero write-once addresses --
`gen_list` in its collapsing form is unusable here.

It is authorable anyway, because the two writes are a PROTOCOL and the protocol is
simple. This generator reproduces both phases in order, the same technique the
SSCHED freelist seeding needed in gen_smalltables.py.

★ THE TWO PHASES

    phase 1:  ERL_CFG[port][tc] = 0x40001000   for ALL 76 ports x 12 classes
    phase 2:  ERL_CFG[port][tc] = 0x80001000   for the 52 front-panel ports
              ERL_CFG[0][tc]    = PORT0_TC[tc] for the CPU port
              (the remaining 23 ports are left at their phase-1 value)

Bit 30 in phase 1 and bit 31 in phase 2 are the whole difference: every entry is
parked in one state, then the ports that carry traffic are moved to the other.
Verified exhaustively against the replay: **all 912 first writes are 0x40001000**,
all 624 front-panel second writes are 0x80001000, and all 276 second writes to
the other 23 ports are 0x40001000 again.

★ ONLY THE CPU PORT NEEDS A TABLE. Phase 2 has just **6 distinct values across
912 entries**, and 624 of them are the single constant 0x80001000. The only
per-entry data in the whole block is port 0's 12 traffic classes, which get real
per-class rates -- the CPU port is shaped where the front-panel ports are not.
That is the same split ESCHED showed (gen_esched.py): front-panel ports uniform
and unshaped, port 0 different.

    ERL_CFG_IFG[port] = 0x14, twice, for the 55 configured ports

is the rest, and the two writes there are identical, so order does not matter --
but the COUNT still does, which is why they are emitted twice as well.

⚠ PLACEMENT. In the replay phase 1 begins around line 9,559 and phase 2 lands
near 67,000. gen_list_early splices this tool at the block's FIRST write, so
phase 1 stays where EOS put it and phase 2 moves ~57,000 writes earlier. The end
state is identical and the phase order per address is preserved; what changes is
that the real rate limits are installed sooner. That is the safe direction, but
it IS a behavioural change and it is why this is validated under load against the
EOS reference rather than by a single ping (docs/EDGENOS-7150.md (was LOAD-LOSS-OPEN)).

usage:
    gen_erl.py --emit | --addrs | --structure | --verify <img> | --counts <img> | --c FILE
SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import load  # noqa: E402

GEOM = {
    "FM6000_ERL_CFG":     (0x117000, 1, [(12, 1), (76, 0x10)]),
    "FM6000_ERL_CFG_IFG": (0x117800, 1, [(76, 1)]),
}

ACTIVE_PORTS = [0, 1, 3] + list(range(20, 48)) + list(range(52, 76))
NOT_FRONT_PANEL = {0, 1, 3}
FRONT_PANEL = [p for p in ACTIVE_PORTS if p not in NOT_FRONT_PANEL]

N_PORTS, N_TC = 76, 12
PHASE1 = 0x40001000          # every entry parked here first
PHASE2_FRONT = 0x80001000    # front-panel ports: the other state, all 12 classes
IFG_VALUE = 0x14

PORT0_TC = [
    0x003b1000,
    0x00131000,
    0x00171000,
    0x00061000,
    0x00171000,
    0x00061000,
    0x00171000,
    0x00171000,
    0x00171000,
    0x000b1000,
    0x000b1000,
    0x00061000,
]

def cfg_addr(port, tc):
    base, _w, ax = GEOM["FM6000_ERL_CFG"]
    return base + port * ax[1][1] + tc * ax[0][1]


def build_seq():
    """Ordered: phase 1 for every entry, then phase 2, then the IFG pair."""
    seq = []
    for p in range(N_PORTS):
        for t in range(N_TC):
            seq.append((cfg_addr(p, t), PHASE1))
    for p in range(N_PORTS):
        for t in range(N_TC):
            if p in FRONT_PANEL:
                v = PHASE2_FRONT
            elif p == 0:
                v = PORT0_TC[t]
            else:
                v = PHASE1          # re-parked; EOS writes it again, so do we
            seq.append((cfg_addr(p, t), v))
    ifg = GEOM["FM6000_ERL_CFG_IFG"][0]
    for _ in range(2):
        for p in ACTIVE_PORTS:
            seq.append((ifg + p, IFG_VALUE))
    return seq


def build():
    m = {}
    for a, v in build_seq():
        m[a] = v
    return m


def structure():
    print(f"ERL_CFG   {N_PORTS} ports x {N_TC} classes = {N_PORTS*N_TC} entries, "
          f"each written twice")
    print(f"  phase 1: 0x{PHASE1:08x} everywhere")
    print(f"  phase 2: 0x{PHASE2_FRONT:08x} x {len(FRONT_PANEL)*N_TC} "
          f"(front-panel), port 0 table x {N_TC}, "
          f"0x{PHASE1:08x} x {(N_PORTS-len(FRONT_PANEL)-1)*N_TC} (re-parked)")
    print(f"  port 0 rates: {[hex(v) for v in PORT0_TC]}")
    print(f"ERL_CFG_IFG  0x{IFG_VALUE:x} x {len(ACTIVE_PORTS)} ports, twice")
    seq = build_seq()
    print(f"\ntotal {len(seq)} writes over {len(build())} addresses")
    return 0


def counts(image):
    import collections
    rep = collections.Counter()
    vals = collections.defaultdict(list)
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
                vals[a].append(v)
    mine = collections.Counter(a for a, _ in build_seq())
    myseq = collections.defaultdict(list)
    for a, v in build_seq():
        myseq[a].append(v)
    bad = 0
    for a in sorted(rep):
        if rep[a] != mine[a]:
            print(f"  0x{a:06x} replay writes {rep[a]}, we write {mine[a]}")
            bad += 1
        elif vals[a] != myseq[a]:
            print(f"  0x{a:06x} SEQUENCE differs: replay "
                  f"{[hex(x) for x in vals[a]]} vs ours {[hex(x) for x in myseq[a]]}")
            bad += 1
    print(f"addresses whose write count OR value sequence differs: {bad}")
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


C_HEAD = r"""/* fm6000_erl.c - egress rate limiter, including its two-phase init.
 *
 * GENERATED by asic/fm6000/tools/gen_erl.py --c. Edit the generator.
 *
 * Every ERL address is written TWICE by the replay, 636 of them with two
 * different values, so this block has no write-once part at all and cannot be
 * collapsed to a final value. The two writes are a protocol:
 *
 *   phase 1:  ERL_CFG[port][tc] = 0x40001000   all 76 ports x 12 classes
 *   phase 2:  ERL_CFG[port][tc] = 0x80001000   the 52 front-panel ports
 *             ERL_CFG[0][tc]    = per-class rate for the CPU port
 *             (the other 23 ports are written 0x40001000 again)
 *
 * Phase 2 holds only 6 distinct values across 912 entries; 624 are the single
 * constant 0x80001000. The only per-entry data in the block is port 0's 12
 * traffic classes -- the CPU port is shaped, the front-panel ports are not.
 *
 * ERL_CFG_IFG = 0x14 for the 55 configured ports, also written twice.
 *
 * ⚠ ORDER MATTERS: phase 1 must precede phase 2 for each address. Do not sort
 * or dedupe this table -- it is a sequence, not a set.
 *
 * Verified against the executed image: 967 of 967 addresses byte-identical, and
 * every address's two-write VALUE SEQUENCE checked against the replay.
 *
 * usage: fm6000_erl [-n | -a] [-b bdf]
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
		fprintf(stderr, "fm6000_erl: %d writes\n",
		        (int)(sizeof W / sizeof W[0]));
	return 0;
}
"""


def emit_c(path):
    seq = build_seq()
    n1 = N_PORTS * N_TC
    with open(path, "w") as f:
        f.write(C_HEAD)
        f.write("\t/* phase 1: park every entry */\n")
        for a, v in seq[:n1]:
            f.write("\t{ 0x%06x, 0x%08x },\n" % (a, v))
        f.write("\t/* phase 2: front-panel ports on, CPU port shaped */\n")
        for a, v in seq[n1:2 * n1]:
            f.write("\t{ 0x%06x, 0x%08x },\n" % (a, v))
        f.write("\t/* ERL_CFG_IFG, written twice */\n")
        for a, v in seq[2 * n1:]:
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
