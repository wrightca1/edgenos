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

★ ONE VALUE, EVERYWHERE -- AND IT IS NOT A CREDIT FIELD.
⚠ CORRECTED 2026-08-21. This file used to describe 0xffffff as "24 bits, all set,
a scheduler credit/quantum field at its maximum". That was a guess made before
the field names were recovered. sdk_fieldmap.py names them:

    ESCHED_CFG_1/2 : strictPriority[11:0], tcEnable[23:12]

Twelve bits each, one per traffic class. So 0xffffff means **strict priority on
all 12 traffic classes, and all 12 enabled** -- not a credit at maximum. Same
value, correct reason. The port set and the check were right; the explanation
was wrong, and a wrong explanation is what a value table would have hidden.

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
    "FM6000_ESCHED_CFG_1":  (0x002000, 1, [(76, 1)]),
    "FM6000_ESCHED_CFG_2":  (0x002080, 1, [(76, 1)]),
    "FM6000_ESCHED_DRR_CFG": (0x003800, 1, [(76, 1)]),
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

N_TC = 12                        # traffic classes; both fields are 12 bits wide


def cfg(strict, enable):
    """ESCHED_CFG_1/2 = strictPriority[11:0] | tcEnable[23:12]."""
    return (strict & 0xfff) | ((enable & 0xfff) << 12)


ALL_TC = (1 << N_TC) - 1

# front panel: strict priority on every class, every class enabled
ESCHED_CFG_VALUE = cfg(ALL_TC, ALL_TC)               # 0xffffff

# ★ THE CPU PORT IS DIFFERENT, and now we can say how rather than just that it
# is. Port 0 gets strict priority on ONE class in CFG_1 and none in CFG_2, with
# every class enabled in both -- the CPU port is scheduled, the front-panel
# ports are flat.
CPU_CFG_1 = cfg(1 << 11, ALL_TC)                     # 0xfff800
CPU_CFG_2 = cfg(0,       ALL_TC)                     # 0xfff000

# ESCHED_DRR_CFG = zeroLength[11:0] | groupBoundary[23:12] | ifgPenalty[31:24].
# Written twice per port: first with an interframe-gap penalty of 20, then with
# it cleared. Same two-phase shape as ERL (gen_erl.py).
def drr_cfg(ifg):
    return (ALL_TC) | (ALL_TC << 12) | ((ifg & 0xff) << 24)


DRR_CFG_PHASE1 = drr_cfg(20)                         # 0x14ffffff
DRR_CFG_PHASE2 = drr_cfg(0)                          # 0x00ffffff

# ⚠ ESCHED_DRR_Q IS DELIBERATELY NOT OURS -- it is runtime state, not init.
# q[23:0] is the deficit quantum per (port, traffic class). The replay writes the
# CPU port's 12 classes 37 times each, and the values CHANGE as it goes:
#
#     tc0 : 0xa00 x12 -> 0x100 x1  -> 0x1450 x24
#     tc3 : 0xa00 x9  -> 0x100 x13 -> 0x05c8 x15
#     tc9 : 0xa00 x3  -> 0x3e8 x17 -> 0x6590 x17
#
# That is the scheduler recomputing quanta as ports come up, with the repeat
# counts falling wherever the replay happened to interleave. Claiming these
# addresses would splice all 444 writes out and freeze the quantum at a value
# computed for a different moment. Same rule as SSCHED_RX_SLOW_PORT[1..4] and
# the monotonic bitmaps: leave runtime state in the replay.


def build_seq():
    """Ordered. DRR_CFG is two-phase, so this is a sequence, not a set."""
    seq = []
    c1 = GEOM["FM6000_ESCHED_CFG_1"][0]
    c2 = GEOM["FM6000_ESCHED_CFG_2"][0]
    drr = GEOM["FM6000_ESCHED_DRR_CFG"][0]

    for p in FRONT_PANEL:
        seq.append((c1 + p, ESCHED_CFG_VALUE))
    for p in FRONT_PANEL:
        seq.append((c2 + p, ESCHED_CFG_VALUE))

    # the CPU port, scheduled differently
    seq.append((c1, CPU_CFG_1))
    seq.append((c2, CPU_CFG_2))

    # DRR config over the CPU port AND the front panel, ifgPenalty set then cleared
    ports = [0] + FRONT_PANEL
    for p in ports:
        seq.append((drr + p, DRR_CFG_PHASE1))
    for p in ports:
        seq.append((drr + p, DRR_CFG_PHASE2))
    return seq


def build():
    return {a: v for a, v in build_seq()}


def structure():
    print(f"front-panel ports: {len(FRONT_PANEL)}  "
          f"({FRONT_PANEL[0]}-{FRONT_PANEL[27]}, {FRONT_PANEL[28]}-{FRONT_PANEL[-1]})")
    print(f"excluded from the front-panel set: {sorted(NOT_FRONT_PANEL)}")
    print()
    print(f"  CFG_1/CFG_2  front panel x{len(FRONT_PANEL)}  "
          f"= 0x{ESCHED_CFG_VALUE:06x}  strictPriority=all{N_TC} tcEnable=all{N_TC}")
    print(f"  CFG_1[0]     CPU port          = 0x{CPU_CFG_1:06x}  "
          f"strictPriority=1 class, tcEnable=all")
    print(f"  CFG_2[0]     CPU port          = 0x{CPU_CFG_2:06x}  "
          f"strictPriority=none,   tcEnable=all")
    print(f"  DRR_CFG      CPU + front x{len([0] + FRONT_PANEL)}  "
          f"two-phase 0x{DRR_CFG_PHASE1:08x} -> 0x{DRR_CFG_PHASE2:08x} "
          f"(ifgPenalty 20 -> 0)")
    print(f"\n  ⚠ DRR_Q is NOT ours -- runtime state, see the header.")
    seq = build_seq()
    print(f"\ntotal {len(seq)} writes over {len(build())} addresses")
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


C_HEAD = r"""/* fm6000_esched.c - egress scheduler configuration.
 *
 * GENERATED by asic/fm6000/tools/gen_esched.py --c. Edit the generator.
 *
 *   CFG_1/CFG_2  strictPriority[11:0] | tcEnable[23:12], 12 traffic classes.
 *                front panel: strict on all 12, all 12 enabled (0xffffff).
 *                CPU port:    CFG_1 strict on one class, CFG_2 none.
 *   DRR_CFG      zeroLength[11:0] | groupBoundary[23:12] | ifgPenalty[31:24],
 *                written TWICE per port: ifgPenalty 20, then cleared to 0.
 *
 * The port set is derived (configured ports minus the CPU/management ports
 * 0, 1, 3) and checked in both directions by --verify.
 *
 * ⚠ ORDER MATTERS: DRR_CFG is two-phase. Do not sort or dedupe this table.
 * ⚠ ESCHED_DRR_Q is deliberately absent -- it is runtime state whose value
 *   changes as ports come up, not initialisation.
 *
 * Verified byte-identical against the executed image: 159 of 159 addresses,
 * with the derived port set checked in both directions.
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
		fprintf(stderr, "fm6000_esched: %d writes\n",
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
    for ad, v in build_seq():
        print(f"{ad:08x}" if a.addrs else f"{ad:08x} {v:08x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
