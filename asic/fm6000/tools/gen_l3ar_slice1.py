#!/usr/bin/env python3
"""gen_l3ar_slice1.py - author L3AR slice 1 (canonical source GLORT) from intent.

Slice 1 derives CSGLORT, the 12-bit "canonical SGLORT" -- the logical source of a
frame rather than its physical source port. See docs/EDGENOS-7150.md (was L3AR-STRUCTURE) for how the
layout was established; nothing here is transcribed from EOS's table.

WHAT THE STAGE DOES

  default            csGlort = ISL_SGLORT                    (pass through)
  FFU-tagged frames  csGlort = FFU_DATA.W16A[11:0]           (FFU override)
  LAG GLORT blocks   csGlort = ISL_SGLORT & 0xff20           (canonicalise)

The LAG case is the point of the stage. A link aggregate occupies a contiguous
block of GLORTs whose low bits index the member port; clearing those bits maps
every member onto one canonical value, so source suppression sees the aggregate
instead of the port the frame happened to arrive on.

SIX RULES, NOT THIRTY-TWO

EOS ships 32. Twenty-six of them (its rules 2-27) match ISL_SGLORT==0 and select
csGlort profiles that are byte-identical pass-throughs -- the same result as the
universal default rule 0. They are not distinguishable behaviour, and only one of
them could ever fire in any case: a slice is one precedence set (datasheet
5.10.1). We emit the default plus the five rules that do something.

⚠ PRECEDENCE. Higher rule index wins; rule 0 is the default. This is inference,
but it is forced -- rule 0 matches everything, so if the lowest index won it would
shadow the LAG rules and canonicalisation could never happen, and EOS's own slice
1 would be inert. Our LAG rules therefore keep high indices.

⚠ THE FLAG WORDS ARE NOT OPTIONAL AND MUST NOT BE ZERO.
Datasheet 5.10.5:  ACTION_FLAGS' = ACTION_FLAGS & Mask | Value.
Leaving RAM1/RAM2 at zero on a live rule therefore CLEARS ALL 52 ACTION_FLAGS for
every frame it matches, silently destroying the forwarding decision built by the
upstream stages. Pass-through is Mask=0x3ffffff, Value=0, and every rule we emit
sets it explicitly.

⚠ PROFILE TABLE ENTRY 5 IS SHARED. Slice 2 -- which we are NOT authoring -- also
selects CSGLORT_PROFILE=5 and SGLORT_PROFILE=1. Those two entries are emitted with
the contents slice 2 already expects. That is interface conformance, the same
reason our parser writes the DMAC where the hardware reads it; the rest of the
table is ours.

usage:
    gen_l3ar_slice1.py --emit                 # <addr> <value> pairs
    gen_l3ar_slice1.py --addrs                # addresses only, for the replay filter
    gen_l3ar_slice1.py --verify <image>       # functional diff against EOS's slice 1
SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from l3ar_decode import (  # noqa: E402
    L3AR_BASE, CAM_SLICE_STRIDE, CAM_RULE_STRIDE, RAM_SLICE_STRIDE,
    RAM1_OFF, RAM2_OFF, RAM3_OFF, RAM4_OFF, RAM5_OFF, RAM3_SLICE_STRIDE,
    KEY_LAYOUT, RAM5_FIELDS, PROFILE_TABLES, encode_cam, load, decode_action,
)

SLICE = 1
RULES = 32
FLAG_PASSTHROUGH = 0x3FFFFFF          # Mask=all-ones, Value=0 -> flags unchanged

KEYPOS = {n: (lo, hi) for n, lo, hi in KEY_LAYOUT}

# csGlort profile indices we allocate. 0 and 5 are fixed by what slice 2 expects;
# LAG is ours to choose, and one entry replaces the four identical ones EOS used.
CSP_PASSTHROUGH, CSP_FROM_SGLORT, CSP_LAG = 0, 5, 10
SGP_PASSTHROUGH, SGP_FFU_W16A = 0, 1

# The LAG GLORT blocks, read off EOS's slice 1 keys. Disjoint, so their relative
# order does not matter. Low 5 bits of a member GLORT index the member port.
LAG_BLOCKS = [(0x1000, 0xF800), (0x1800, 0xFC00),
              (0x1C00, 0xFF00), (0x1D00, 0xFFC0)]
LAG_CANON_MASK = 0xFFE0


def cam_addr(r, seg, w):
    return L3AR_BASE + CAM_SLICE_STRIDE * SLICE + CAM_RULE_STRIDE * r + 4 * seg + w


def ram_addr(off, r, w, stride=RAM_SLICE_STRIDE, per=2):
    return L3AR_BASE + off + stride * SLICE + per * r + w


def key_words(fields):
    """Build 4 segments of (Key, KeyInvert) from {field: (value, mask)}.

    Unconstrained bits are don't-care (Key=1, KeyInvert=1), which is what an
    all-ones word means -- not an empty slot.
    """
    value = care = 0
    for name, (v, m) in fields.items():
        lo, hi = KEYPOS[name]
        width = hi - lo + 1
        if v >> width or m >> width:
            raise ValueError(f"{name} does not fit {width} bits")
        value |= (v & m) << lo
        care |= m << lo
    out = []
    for seg in range(4):
        sv = (value >> (64 * seg)) & 0xFFFFFFFFFFFFFFFF
        sc = (care >> (64 * seg)) & 0xFFFFFFFFFFFFFFFF
        out.append(encode_cam(sv, sc))
    return out


def never_match_words():
    """Key=0, KeyInvert=0 on every bit: the rule can never fire.

    Written explicitly rather than omitted. An unwritten CAM slot holds whatever
    the previous image left there, and all-ones would be a universal match.
    """
    return [(0, 0)] * 4


def ram5_word(**f):
    acc = 0
    for n, lo, w in RAM5_FIELDS:
        v = f.get(n, 0)
        if v >> w:
            raise ValueError(f"{n} too wide")
        acc |= v << lo
    return acc


def build():
    """Return {addr: value} for the whole of slice 1."""
    m = {}

    # --- rules ------------------------------------------------------------
    # index -> (key fields or None for never-match, ram5 action)
    rules = {}

    passthrough = ram5_word(MuxOutput_SGLORT=1, SGLORT_PROFILE=SGP_PASSTHROUGH,
                            MuxOutput_CSGLORT=1, CSGLORT_PROFILE=CSP_PASSTHROUGH)
    # rule 0: the default. Matches everything, lowest precedence.
    rules[0] = ({}, passthrough)

    # rule 1: the FFU has classified this frame; take the canonical source from
    # the FFU's own 16-bit data word rather than the ISL header.
    rules[1] = ({"FFU_DATA_W16A_TOP": (0x2, 0xF)},
                ram5_word(MuxOutput_SGLORT=1, SGLORT_PROFILE=SGP_FFU_W16A,
                          MuxOutput_CSGLORT=1, CSGLORT_PROFILE=CSP_FROM_SGLORT))

    # rules 28-31: one per LAG GLORT block. High indices so they beat the default.
    lag = ram5_word(MuxOutput_SGLORT=1, SGLORT_PROFILE=SGP_PASSTHROUGH,
                    MuxOutput_CSGLORT=1, CSGLORT_PROFILE=CSP_LAG)
    for i, (base, mask) in enumerate(LAG_BLOCKS):
        rules[31 - i] = ({"ISL_SGLORT": (base, mask)}, lag)

    for r in range(RULES):
        spec = rules.get(r)
        segs = key_words(spec[0]) if spec else never_match_words()
        for seg, (k, ki) in enumerate(segs):
            # ⚠ ORDER: words 0-1 are KeyInvert, words 2-3 are Key -- NOT the other
            # way round, which is what this generator did first. The functional
            # --verify passed anyway, because it compares csGlort profile contents
            # and never looks at the keys; only a byte-comparison against EOS's
            # own rule 31, which carries a key we also author, exposed it. Every
            # rule differed by exactly a word 0<->2 swap.
            m[cam_addr(r, seg, 0)] = ki & 0xFFFFFFFF
            m[cam_addr(r, seg, 1)] = (ki >> 32) & 0xFFFFFFFF
            m[cam_addr(r, seg, 2)] = k & 0xFFFFFFFF
            m[cam_addr(r, seg, 3)] = (k >> 32) & 0xFFFFFFFF

        # flags: pass through, on every rule including the dead ones
        for off in (RAM1_OFF, RAM2_OFF):
            m[ram_addr(off, r, 0)] = FLAG_PASSTHROUGH
            m[ram_addr(off, r, 1)] = 0
        # slice 1 uses no trap/L2-lookup/ALU actions at all
        m[ram_addr(RAM3_OFF, r, 0, RAM3_SLICE_STRIDE, 1)] = 0
        m[ram_addr(RAM4_OFF, r, 0)] = 0
        m[ram_addr(RAM4_OFF, r, 1)] = 0
        act = spec[1] if spec else 0
        m[ram_addr(RAM5_OFF, r, 0)] = act & 0xFFFFFFFF
        m[ram_addr(RAM5_OFF, r, 1)] = (act >> 32) & 0xFFFFFFFF

    # --- profile tables ---------------------------------------------------
    def glort_entry(base, idx, value, mask, select):
        e = (value & 0xFFFF) | ((mask & 0xFFFF) << 16) | (select << 32)
        m[base + 2 * idx] = e & 0xFFFFFFFF
        m[base + 2 * idx + 1] = (e >> 32) & 0xFFFFFFFF

    csg = PROFILE_TABLES["CSGLORT"][0]
    sgl = PROFILE_TABLES["SGLORT"][0]
    # Select 0 = ISL_SGLORT, 2 = output of the SGLORT transform (Table 5-37).
    glort_entry(csg, CSP_PASSTHROUGH, 0x0000, 0xFFFF, 0)
    glort_entry(csg, CSP_FROM_SGLORT, 0x0000, 0xFFFF, 2)
    glort_entry(csg, CSP_LAG, 0x0000, LAG_CANON_MASK, 0)
    glort_entry(sgl, SGP_PASSTHROUGH, 0x0000, 0xFFFF, 0)
    glort_entry(sgl, SGP_FFU_W16A, 0x0000, 0x0FFF, 2)
    return m


def verify(image):
    """Functional diff: for every csGlort profile EOS's slice 1 can select, does
    ours select a profile with the same content? Byte equality is the wrong test
    -- we deliberately use one LAG profile where EOS used four identical ones."""
    eos = load(image)
    ours = build()
    csg = PROFILE_TABLES["CSGLORT"][0]

    def prof(mem, base, i):
        lo, hi = mem.get(base + 2 * i), mem.get(base + 2 * i + 1)
        if lo is None or hi is None:
            return None
        e = lo | (hi << 32)
        return (e & 0xFFFF, (e >> 16) & 0xFFFF, (e >> 32) & 3)

    def action(mem, r):
        lo = mem.get(L3AR_BASE + RAM5_OFF + RAM_SLICE_STRIDE * SLICE + 2 * r)
        hi = mem.get(L3AR_BASE + RAM5_OFF + RAM_SLICE_STRIDE * SLICE + 2 * r + 1)
        return (lo or 0) | ((hi or 0) << 32)

    eos_behaviours, our_behaviours = set(), set()
    for r in range(RULES):
        for mem, acc in ((eos, eos_behaviours), (ours, our_behaviours)):
            a = action(mem, r)
            if not a:
                continue
            idx = (a >> 13) & 0x1F
            acc.add(prof(mem, csg, idx))
    print("csGlort behaviours EOS  :", sorted(x for x in eos_behaviours if x))
    print("csGlort behaviours ours :", sorted(x for x in our_behaviours if x))
    missing = eos_behaviours - our_behaviours
    extra = our_behaviours - eos_behaviours
    for x in missing:
        print(f"  ⚠ MISSING behaviour {x}")
    for x in extra:
        print(f"  ⚠ EXTRA behaviour {x}")
    live_eos = sum(1 for r in range(RULES) if action(eos, r))
    live_ours = sum(1 for r in range(RULES) if action(ours, r))
    print(f"rules with an action: EOS {live_eos}, ours {live_ours}")
    print("VERIFY PASS" if not missing and not extra else "VERIFY FAIL")
    return 0 if not missing and not extra else 1


C_HEAD = r'''/* fm6000_l3arslice1.c - L3AR slice 1: canonical source GLORT.
 *
 * GENERATED by asic/fm6000/tools/gen_l3ar_slice1.py --c. Edit the generator, not
 * this file. The generator's docstring carries the reasoning; the short version:
 *
 *   default            csGlort = ISL_SGLORT              (pass through)
 *   FFU-tagged frames  csGlort = FFU_DATA.W16A[11:0]     (FFU override)
 *   LAG GLORT blocks   csGlort = ISL_SGLORT & 0xffe0     (canonicalise)
 *
 * Six rules where EOS ships 32: its other 26 match ISL_SGLORT==0 and select
 * csGlort profiles that are byte-identical pass-throughs -- the same result as
 * the universal default rule 0 -- and only one of them could fire in any case,
 * since a slice is one precedence set (datasheet 5.10.1).
 *
 * WARNING: unused rule slots are written with never-match keys (Key=0,
 * KeyInvert=0), not left unwritten. An unwritten CAM slot keeps whatever the
 * previous image left there, and all-ones is a UNIVERSAL MATCH, not an empty
 * slot.
 *
 * WARNING: RAM1/RAM2 carry Mask=0x3ffffff, Value=0 on every rule. That is flags
 * pass-through: ACTION_FLAGS' = ACTION_FLAGS & Mask | Value (datasheet 5.10.5).
 * Zero there would CLEAR ALL 52 ACTION_FLAGS on every frame the rule matches.
 *
 * Verified: the six live rules' CAM words are byte-identical to EOS's, and the
 * profile-table entries were read off live silicon and match byte for byte.
 *
 * usage: fm6000_l3arslice1 [-n | -a] [-b bdf]
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
		fprintf(stderr, "fm6000_l3arslice1: %d writes\n",
		        (int)(sizeof W / sizeof W[0]));
	return 0;
}
'''


def rule_addrs(r):
    """Every address rule r owns, in address order."""
    a = [cam_addr(r, seg, w) for seg in range(4) for w in range(4)]
    a += [ram_addr(RAM1_OFF, r, 0), ram_addr(RAM1_OFF, r, 1),
          ram_addr(RAM2_OFF, r, 0), ram_addr(RAM2_OFF, r, 1),
          ram_addr(RAM3_OFF, r, 0, RAM3_SLICE_STRIDE, 1),
          ram_addr(RAM4_OFF, r, 0), ram_addr(RAM4_OFF, r, 1),
          ram_addr(RAM5_OFF, r, 0), ram_addr(RAM5_OFF, r, 1)]
    return sorted(a)


def emit_c(path):
    m = build()
    live = {0: "default: csGlort = ISL_SGLORT",
            1: "FFU override: csGlort = FFU_DATA.W16A[11:0]"}
    for i, (base, mask) in enumerate(LAG_BLOCKS):
        live[31 - i] = "LAG block 0x%04x/0x%04x: csGlort &= 0xffe0" % (base, mask)
    used = set()
    with open(path, "w") as f:
        f.write(C_HEAD)
        for r in range(RULES):
            f.write("\t/* rule %2d -- %s */\n"
                    % (r, live.get(r, "unused slot: never-match key")))
            for a in rule_addrs(r):
                used.add(a)
                f.write("\t{ 0x%06x, 0x%08x },\n" % (a, m[a]))
        f.write("\t/* csGlort and SGLORT profile tables. csGlort entry 5 and SGLORT\n"
                "\t * entry 1 are also selected by slice 2, which we do not author,\n"
                "\t * so they carry the contents slice 2 already expects. */\n")
        for a in sorted(set(m) - used):
            f.write("\t{ 0x%06x, 0x%08x },\n" % (a, m[a]))
        f.write(C_TAIL)
    print("wrote %s: %d writes" % (path, len(m)), file=sys.stderr)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--c", dest="cfile")
    ap.add_argument("--emit", action="store_true")
    ap.add_argument("--addrs", action="store_true")
    ap.add_argument("--verify")
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
