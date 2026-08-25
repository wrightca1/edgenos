#!/usr/bin/env python3
"""gen_cmwm.py - author the FM6000 congestion-management watermark tables.

The six per-port watermark tables are the largest block still replayed from EOS
after the L3AR work: about 20,000 writes, of which `fm6000_cmminit` covers 72
addresses. They are also the most regular thing left in the replay.

STRUCTURE, measured off the image

Every table is one word per entry indexed **`port * 16 + traffic_class`**:

    RXMP_PRIVATE    0x112800   76 ports x 12 classes =  912
    RXMP_HOG        0x113000   76 x 16               = 1216
    TXMP_PRIVATE    0x113800   80 x 16               = 1280
    TXMP_HOG        0x114000   80 x 16               = 1280
    RXMP_PAUSE_ON   0x115000   76 x 12               =  912
    RXMP_PAUSE_OFF  0x115800   76 x 12               =  912

and the contents collapse to a handful of **port groups**, each with one
per-class vector. `RXMP_HOG` is a single constant (`0xffffffff`) across all 1,216
entries; `RXMP_PAUSE_ON`/`OFF` are two values across two groups.

The grouping is the port's role, not an arbitrary list: ports 1-2, 4-19 and 48-51
carry `0xffffffff` (no limit) in the RX tables while 0, 3, 20-47 and 52-75 carry
real watermarks, and TX ports 76-79 -- which have no front-panel presence -- are
zeroed.

⚠ WHY THIS IS AUTHORING AND NOT TRANSCRIPTION. These are buffer-sizing constants
applied across a port x class matrix, the same shape `fm6000_ffubstinit` already
exploits for the FFU BST default fill. What is recorded below is the *structure*
-- which port ranges share a policy and what each class vector is -- not 6,500
opaque words. Contrast JSS/SBUS lane tuning, which is board-measured and stays in
the replay (docs/EDGENOS-7150.md (was BLOB-REMOVAL-PLAN)).

⚠ These are watermarks: they decide when the chip drops and when it asserts
PAUSE. Getting them wrong does not fail loudly -- it shows up as packet loss or
head-of-line blocking under load, which the transit test would not catch. Hence
--verify is a strict byte comparison against the image.

usage:
    gen_cmwm.py --emit | --addrs | --verify <image> | --c FILE
SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import sys

# name: (base, ports, classes, [(port runs, per-class values)])
TABLES = {
    "RXMP_PRIVATE": (0x112800, 76, 12, [
        ([(0,0), (3,3), (20,47), (52,75)], [0x13003a, 0x60014, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0]),
        ([(1,2), (4,19), (48,51)], [0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff]),
    ]),
    "RXMP_HOG": (0x113000, 76, 16, [
        ([(0,75)], [0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff]),
    ]),
    "TXMP_PRIVATE": (0x113800, 80, 16, [
        ([(0,0)], [0x8014, 0x8014, 0x8014, 0x8014, 0x8014, 0x8014, 0x8014, 0x8014, 0x8014, 0x8014, 0x8014, 0x8000, 0x8000, 0x8000, 0x8000, 0x7]),
        ([(1,2), (4,19), (48,51)], [0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff]),
        ([(3,3), (20,47), (52,75)], [0x8004, 0x8004, 0x8004, 0x8004, 0x8004, 0x8004, 0x8004, 0x8004, 0x8004, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x7]),
        ([(76,79)], [0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0]),
    ]),
    "TXMP_HOG": (0x114000, 80, 16, [
        ([(0,0)], [0xd6, 0xd6, 0xd6, 0xd6, 0xd6, 0xd6, 0xd6, 0xd6, 0xd6, 0xd6, 0xd6, 0x0, 0x0, 0x0, 0x0, 0xcd]),
        ([(1,2), (4,19), (48,51)], [0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x3fff]),
        ([(3,3), (20,47), (52,75)], [0x15f5, 0x15f5, 0x15f5, 0x15f5, 0x15f5, 0x15f5, 0x15f5, 0x15f5, 0x15f5, 0x15f5, 0x15f5, 0x0, 0x0, 0x0, 0x0, 0xcd]),
        ([(76,79)], [0x111, 0x111, 0x111, 0x111, 0x111, 0x111, 0x111, 0x111, 0x111, 0x111, 0x111, 0x111, 0x111, 0x111, 0x111, 0x111]),
    ]),
    "RXMP_PAUSE_ON": (0x115000, 76, 12, [
        ([(0,0), (20,47), (52,75)], [0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000]),
        ([(1,19), (48,51)], [0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff]),
    ]),
    "RXMP_PAUSE_OFF": (0x115800, 76, 12, [
        ([(0,0), (20,47), (52,75)], [0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000, 0x4000c000]),
        ([(1,19), (48,51)], [0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff]),
    ]),
}


def build():
    m = {}
    for _name, (base, nports, nclass, groups) in TABLES.items():
        for runs, vals in groups:
            for lo, hi in runs:
                for p in range(lo, hi + 1):
                    if p >= nports:
                        continue
                    for c in range(nclass):
                        v = vals[c]
                        if v is None:
                            continue
                        m[base + p * 16 + c] = v
    return m


def verify(image):
    sys.path.insert(0, __file__.rsplit("/", 1)[0])
    from parser_decode import load
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
    # and the other direction: anything in these ranges we fail to emit
    extra = 0
    for _n, (base, _np, _nc, _g) in TABLES.items():
        for a in range(base, base + 0x800):
            if a in eos and a not in ours:
                extra += 1
    print(f"identical {same}, differing {bad}, absent from image {missing}, "
          f"in image but not emitted {extra}")
    print("VERIFY PASS" if bad == 0 and extra == 0 else "VERIFY FAIL")
    return 0 if (bad == 0 and extra == 0) else 1



C_HEAD = r'''/* fm6000_cmwm.c - congestion-management watermark tables.
 *
 * GENERATED by asic/fm6000/tools/gen_cmwm.py --c. Edit the generator.
 *
 * Six per-port tables, one word per entry, indexed port * 16 + traffic_class:
 *
 *     RXMP_PRIVATE    0x112800   76 ports x 12 classes
 *     RXMP_HOG        0x113000   76 x 16   (a single constant, 0xffffffff)
 *     TXMP_PRIVATE    0x113800   80 x 16
 *     TXMP_HOG        0x114000   80 x 16
 *     RXMP_PAUSE_ON   0x115000   76 x 12
 *     RXMP_PAUSE_OFF  0x115800   76 x 12
 *
 * Emitted structurally -- port groups and per-class vectors -- rather than as
 * 6,512 opaque words, because the structure is the point: ports 1-2, 4-19 and
 * 48-51 take "no limit" in the RX tables while 0, 3, 20-47 and 52-75 take real
 * watermarks, and TX ports 76-79 are zeroed.
 *
 * WARNING: these decide when the chip drops and when it asserts PAUSE. A wrong
 * value does not fail loudly -- it appears as loss or head-of-line blocking
 * under load, which a transit test will not catch. The generator byte-verifies
 * against the image in both directions.
 *
 * usage: fm6000_cmwm [-n | -a] [-b bdf]
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

struct group { int lo, hi; const uint32_t *vals; };
struct table { const char *name; uint32_t base; int nports, nclass;
               const struct group *groups; int ngroups; };

'''

C_TAIL = r'''
static volatile uint32_t *M;

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	int dry = 0, list = 0, i, g, p, c;
	long n = 0;

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

	for (i = 0; i < (int)(sizeof T / sizeof T[0]); i++)
		for (g = 0; g < T[i].ngroups; g++)
			for (p = T[i].groups[g].lo; p <= T[i].groups[g].hi; p++) {
				if (p >= T[i].nports) continue;
				for (c = 0; c < T[i].nclass; c++) {
					uint32_t a = T[i].base + p * 16 + c;
					uint32_t v = T[i].groups[g].vals[c];
					if (list)      printf("%08x\n", a);
					else if (dry)  printf("%08x %08x\n", a, v);
					else         { M[a] = v; __sync_synchronize(); }
					n++;
				}
			}
	if (!dry && !list)
		fprintf(stderr, "fm6000_cmwm: %ld writes\n", n);
	return 0;
}
'''


def emit_c(path):
    with open(path, "w") as f:
        f.write(C_HEAD)
        tn = 0
        for name, (base, np_, nc, groups) in TABLES.items():
            for gi, (runs, vals) in enumerate(groups):
                f.write("static const uint32_t V_%s_%d[] = { %s };\n"
                        % (name, gi, ", ".join("0x%xu" % (v or 0) for v in vals)))
            f.write("static const struct group G_%s[] = {\n" % name)
            for gi, (runs, _v) in enumerate(groups):
                for lo, hi in runs:
                    f.write("\t{ %d, %d, V_%s_%d },\n" % (lo, hi, name, gi))
            f.write("};\n\n")
            tn += 1
        f.write("static const struct table T[] = {\n")
        for name, (base, np_, nc, groups) in TABLES.items():
            nruns = sum(len(r) for r, _ in groups)
            f.write('\t{ "%s", 0x%06xu, %d, %d, G_%s, %d },\n'
                    % (name, base, np_, nc, name, nruns))
        f.write("};\n")
        f.write(C_TAIL)
    print("wrote %s" % path, file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", action="store_true")
    ap.add_argument("--addrs", action="store_true")
    ap.add_argument("--verify")
    ap.add_argument("--c", dest="cfile")
    a = ap.parse_args()
    if a.cfile:
        emit_c(a.cfile)
        return 0
    if a.verify:
        return verify(a.verify)
    m = build()
    for addr in sorted(m):
        print(f"{addr:08x}" if a.addrs else f"{addr:08x} {m[addr]:08x}")
    if not a.emit and not a.addrs:
        print(f"{len(m)} writes", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
