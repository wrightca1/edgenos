#!/usr/bin/env python3
"""gen_mapper.py - author the MAPPER tables still replayed from EOS.

MAPPER was the largest single block left after L3AR and CM: **6,283 writes across
565 addresses**. Five sixths of it is boilerplate, and saying what it is takes
one sentence per table.

★ THE THREE PER-PORT QoS TABLES ARE 5,280 OF THE 6,283 WRITES

    QOS_PER_PORT_VPRI1  0x123f00   identity map, every port
    QOS_PER_PORT_VPRI2  0x124000   identity map, every port
    QOS_PER_PORT_W4     0x124100   all zero, every port

`0x76543210` / `0xfedcba98` is sixteen nibbles where nibble *n* holds *n* — an
**identity priority map**: VLAN priority in, the same priority out. Every one of
the 55 configured ports carries the identical pair, and W4 is unused. That is the
whole content.

PORT SET. Entries exist for ports **0, 1, 3, 20-47, 52-75** and no others. That
is the same active-port set the CM watermarks use (`gen_cmwm.py`, which has
0/3/20-47/52-75), which is an independent check that the index really is a port
number and not something else.

The remainder — `SRC_PORT_TABLE`, the MAC CAMs, the L4 compare registers and the
QoS-to-ISL maps — differ per entry and are emitted address by address.

⚠ These are classification tables: they decide the priority a frame is treated
with. Wrong values do not fail a transit test; they show up as traffic in the
wrong queue under load. `--verify` is a strict byte comparison against the image.

usage:
    gen_mapper.py --emit | --addrs | --verify <image> | --c FILE
SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import load  # noqa: E402

# name: (base, words, [(port runs)], [per-word values])  for uniform tables
UNIFORM = {
    "FM6000_MAPPER_QOS_PER_PORT_VPRI1": (0x123f00, 2, [(0, 1), (3, 3), (20, 47), (52, 75)], [0x76543210, 0xfedcba98]),
    "FM6000_MAPPER_QOS_PER_PORT_VPRI2": (0x124000, 2, [(0, 1), (3, 3), (20, 47), (52, 75)], [0x76543210, 0xfedcba98]),
    "FM6000_MAPPER_QOS_PER_PORT_W4": (0x124100, 2, [(0, 1), (3, 3), (20, 47), (52, 75)], [0x0, 0x0]),
    "FM6000_MAPPER_VID2_TABLE": (0x122000, 1, [(1006, 1007)], [0x1b]),
    "FM6000_MAPPER_DMAC_RAM1": (0x123200, 1, [(1, 1)], [0x11]),
    "FM6000_MAPPER_DMAC_RAM3": (0x123220, 1, [(1, 1)], [0x1]),
}

# tables whose entries differ -- emitted address by address
EXPLICIT = {
    "FM6000_MAPPER_DMAC_CAM1": [
        (0x123104, 0x57cea254),
        (0x123105, 0x5dabbbb3),
        (0x123106, 0x444ca831),
    ],
    "FM6000_MAPPER_DMAC_CAM3": [
        (0x123184, 0x57cea254),
        (0x123185, 0x5dabbbb3),
        (0x123186, 0x444ca831),
        (0x1231a8, 0x57cea254),
        (0x1231a9, 0x5dabbbb3),
        (0x1231aa, 0x444ca831),
    ],
    "FM6000_MAPPER_L4_DST_COMPARE": [
        (0x123d80, 0x1401120d),
        (0x123d81, 0x1),
        (0x123d82, 0x1401180c),
        (0x123d83, 0x1),
        (0x123d84, 0x208000d),
        (0x123d85, 0x8),
        (0x123d86, 0x3fffe0d),
        (0x123d87, 0x8),
        (0x123d88, 0x12011223),
        (0x123d89, 0x1),
        (0x123d8a, 0x12011822),
        (0x123d8b, 0x1),
        (0x123d8c, 0x80023),
        (0x123d8d, 0x8),
        (0x123d8e, 0x1fffe23),
        (0x123d8f, 0x8),
    ],
    "FM6000_MAPPER_L4_SRC_COMPARE": [
        (0x123d00, 0x1401120d),
        (0x123d01, 0x1),
        (0x123d02, 0x1401180c),
        (0x123d03, 0x1),
        (0x123d04, 0x208000d),
        (0x123d05, 0x8),
        (0x123d06, 0x3fffe0d),
        (0x123d07, 0x8),
        (0x123d08, 0x12011223),
        (0x123d09, 0x1),
        (0x123d0a, 0x12011822),
        (0x123d0b, 0x1),
        (0x123d0c, 0x80023),
        (0x123d0d, 0x8),
        (0x123d0e, 0x1fffe23),
        (0x123d0f, 0x8),
    ],
    "FM6000_MAPPER_QOS_L2_VPRI1_TO_ISL": [
        (0x124200, 0x1),
        (0x124201, 0x1),
        (0x124202, 0x0),
        (0x124203, 0x0),
        (0x124204, 0x2),
        (0x124205, 0x2),
        (0x124206, 0x3),
        (0x124207, 0x3),
        (0x124208, 0x4),
        (0x124209, 0x4),
        (0x12420a, 0x5),
        (0x12420b, 0x5),
        (0x12420c, 0x6),
        (0x12420d, 0x6),
        (0x12420e, 0x7),
        (0x12420f, 0x7),
    ],
    "FM6000_MAPPER_QOS_L3_PRI_TO_ISL": [
        (0x124230, 0x1111),
        (0x124231, 0x1111),
        (0x124232, 0x0),
        (0x124233, 0x0),
        (0x124234, 0x2222),
        (0x124235, 0x2222),
        (0x124236, 0x3333),
        (0x124237, 0x3333),
        (0x124238, 0x4444),
        (0x124239, 0x4444),
        (0x12423a, 0x5555),
        (0x12423b, 0x5555),
        (0x12423c, 0x6666),
        (0x12423d, 0x6666),
        (0x12423e, 0x7777),
        (0x12423f, 0x7777),
    ],
    "FM6000_MAPPER_SMAC_CAM3": [
        (0x123384, 0x21524110),
        (0x123385, 0xbeefffff),
        (0x123386, 0xdead),
        (0x123388, 0x57cea254),
        (0x123389, 0x5dabbbb3),
        (0x12338a, 0x444ca831),
    ],
    "FM6000_MAPPER_SRC_PORT_TABLE": [
        (0x123000, 0x0),
        (0x123001, 0x40),
        (0x123002, 0x0),
        (0x123003, 0x41),
        (0x123004, 0x0),
        (0x123005, 0x0),
        (0x123006, 0x0),
        (0x123007, 0x60),
        (0x123008, 0x0),
        (0x123009, 0x0),
        (0x12300a, 0x0),
        (0x12300b, 0x0),
        (0x12300c, 0x0),
        (0x12300d, 0x0),
        (0x12300e, 0x0),
        (0x12300f, 0x0),
        (0x123010, 0x0),
        (0x123011, 0x0),
        (0x123012, 0x0),
        (0x123013, 0x0),
        (0x123014, 0x0),
        (0x123015, 0x0),
        (0x123016, 0x0),
        (0x123017, 0x0),
        (0x123018, 0x0),
        (0x123019, 0x0),
        (0x12301a, 0x0),
        (0x12301b, 0x0),
        (0x12301c, 0x0),
        (0x12301d, 0x0),
        (0x12301e, 0x0),
        (0x12301f, 0x0),
        (0x123020, 0x0),
        (0x123021, 0x0),
        (0x123022, 0x0),
        (0x123023, 0x0),
        (0x123024, 0x0),
        (0x123025, 0x0),
        (0x123026, 0x0),
        (0x123027, 0x0),
        (0x123028, 0x2000000),
        (0x123029, 0x49),
        (0x12302a, 0x2000000),
        (0x12302b, 0x21),
        (0x12302c, 0x2000000),
        (0x12302d, 0x21),
        (0x12302e, 0x2000000),
        (0x12302f, 0x21),
        (0x123030, 0x2000000),
        (0x123031, 0x21),
        (0x123032, 0x2000000),
        (0x123033, 0x21),
        (0x123034, 0x2000000),
        (0x123035, 0x21),
        (0x123036, 0x2000000),
        (0x123037, 0x21),
        (0x123038, 0x2000000),
        (0x123039, 0x21),
        (0x12303a, 0x2000000),
        (0x12303b, 0x21),
        (0x12303c, 0x2000000),
        (0x12303d, 0x21),
        (0x12303e, 0x2000000),
        (0x12303f, 0x21),
        (0x123040, 0x2000000),
        (0x123041, 0x21),
        (0x123042, 0x2000000),
        (0x123043, 0x21),
        (0x123044, 0x2000000),
        (0x123045, 0x21),
        (0x123046, 0x2000000),
        (0x123047, 0x21),
        (0x123048, 0x2000000),
        (0x123049, 0x21),
        (0x12304a, 0x2000000),
        (0x12304b, 0x21),
        (0x12304c, 0x2000000),
        (0x12304d, 0x21),
        (0x12304e, 0x2000000),
        (0x12304f, 0x21),
        (0x123050, 0x2000000),
        (0x123051, 0x49),
        (0x123052, 0x2000000),
        (0x123053, 0x21),
        (0x123054, 0x2000000),
        (0x123055, 0x21),
        (0x123056, 0x2000000),
        (0x123057, 0x21),
        (0x123058, 0x2000000),
        (0x123059, 0x21),
        (0x12305a, 0x2000000),
        (0x12305b, 0x21),
        (0x12305c, 0x2000000),
        (0x12305d, 0x21),
        (0x12305e, 0x2000000),
        (0x12305f, 0x21),
        (0x123060, 0x0),
        (0x123061, 0x0),
        (0x123062, 0x0),
        (0x123063, 0x0),
        (0x123064, 0x0),
        (0x123065, 0x0),
        (0x123066, 0x0),
        (0x123067, 0x0),
        (0x123068, 0x2000000),
        (0x123069, 0x21),
        (0x12306a, 0x2000000),
        (0x12306b, 0x21),
        (0x12306c, 0x2000000),
        (0x12306d, 0x21),
        (0x12306e, 0x2000000),
        (0x12306f, 0x21),
        (0x123070, 0x2000000),
        (0x123071, 0x21),
        (0x123072, 0x2000000),
        (0x123073, 0x21),
        (0x123074, 0x2000000),
        (0x123075, 0x21),
        (0x123076, 0x2000000),
        (0x123077, 0x21),
        (0x123078, 0x2000000),
        (0x123079, 0x21),
        (0x12307a, 0x2000000),
        (0x12307b, 0x21),
        (0x12307c, 0x2000000),
        (0x12307d, 0x21),
        (0x12307e, 0x2000000),
        (0x12307f, 0x21),
        (0x123080, 0x2000000),
        (0x123081, 0x21),
        (0x123082, 0x2000000),
        (0x123083, 0x21),
        (0x123084, 0x2000000),
        (0x123085, 0x21),
        (0x123086, 0x2000000),
        (0x123087, 0x21),
        (0x123088, 0x2000000),
        (0x123089, 0x21),
        (0x12308a, 0x2000000),
        (0x12308b, 0x21),
        (0x12308c, 0x2000000),
        (0x12308d, 0x21),
        (0x12308e, 0x2000000),
        (0x12308f, 0x21),
        (0x123090, 0x2000000),
        (0x123091, 0x21),
        (0x123092, 0x2000000),
        (0x123093, 0x21),
        (0x123094, 0x2000000),
        (0x123095, 0x21),
        (0x123096, 0x2000000),
        (0x123097, 0x21),
    ],
}


def build():
    m = {}
    for _name, (base, w, runs, words) in UNIFORM.items():
        for lo, hi in runs:
            for i in range(lo, hi + 1):
                for k, v in enumerate(words):
                    m[base + i * w + k] = v
    for _name, rows in EXPLICIT.items():
        for a, v in rows:
            m[a] = v
    return m


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
    print("VERIFY PASS" if bad == 0 and missing == 0 else "VERIFY FAIL")
    return 0 if (bad == 0 and missing == 0) else 1



C_HEAD = r'''/* fm6000_mapper.c - MAPPER tables: per-port QoS maps, SRC_PORT_TABLE and friends.
 *
 * GENERATED by asic/fm6000/tools/gen_mapper.py --c. Edit the generator.
 *
 * 5,280 of MAPPER's 6,283 replayed writes are three per-port tables with one
 * value each:
 *
 *     QOS_PER_PORT_VPRI1  0x123f00   identity map  (0x76543210 / 0xfedcba98)
 *     QOS_PER_PORT_VPRI2  0x124000   identity map
 *     QOS_PER_PORT_W4     0x124100   all zero
 *
 * 0x76543210/0xfedcba98 is sixteen nibbles where nibble n holds n -- VLAN
 * priority in, the same priority out. Ports 0, 1, 3, 20-47 and 52-75, which is
 * the same active-port set fm6000_cmwm.c uses.
 *
 * WARNING: classification tables. Wrong values do not fail a transit test; they
 * put traffic in the wrong queue under load. Byte-verified against the image.
 *
 * usage: fm6000_mapper [-n | -a] [-b bdf]
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

static const struct { uint32_t addr, val; } W[] = {
'''

C_TAIL = r'''};

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
		fprintf(stderr, "fm6000_mapper: %d writes\n",
		        (int)(sizeof W / sizeof W[0]));
	return 0;
}
'''


def emit_c(path):
    m = build()
    secs = []
    for nm, (base, w, runs, _v) in UNIFORM.items():
        secs.append((nm, base, base + 0x100))
    for nm, rows in EXPLICIT.items():
        secs.append((nm, min(a for a, _ in rows), max(a for a, _ in rows) + 1))
    secs.sort(key=lambda t: t[1])
    done = set()
    with open(path, "w") as f:
        f.write(C_HEAD)
        for label, lo, hi in secs:
            rows = [a for a in sorted(m) if lo <= a < hi and a not in done]
            if not rows:
                continue
            f.write("\t/* %s */\n" % label)
            for a in rows:
                done.add(a)
                f.write("\t{ 0x%06x, 0x%08x },\n" % (a, m[a]))
        for a in sorted(set(m) - done):
            f.write("\t{ 0x%06x, 0x%08x },\n" % (a, m[a]))
        f.write(C_TAIL)
    print("wrote %s: %d writes" % (path, len(m)), file=sys.stderr)


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
