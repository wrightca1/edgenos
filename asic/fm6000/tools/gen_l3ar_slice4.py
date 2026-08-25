#!/usr/bin/env python3
"""gen_l3ar_slice4.py - author L3AR slice 4 (ALU46 operand selection) from intent.

Slice 4 is the simplest of the five: it drives one action, `MuxOutput_ALU46_OP`,
and touches neither RAM3 nor RAM5. See docs/EDGENOS-7150.md (was L3AR-STRUCTURE) for how the layout
was established; nothing here is transcribed from EOS's table.

WHAT THE STAGE DOES

The rules come in PAIRS, and the pairing is the whole design:

    rule N     match a condition                          -> no action
    rule N+1   the same condition AND a port qualifier    -> select ALU46 operand

The qualifier is `SRC_PORT_ID4=0x00/0x20` with `FFU_DATA_W8A=0x80/0xd0`. Since a
slice resolves LAST MATCH WINS (fm6000_l3arinit.c, and forced by rule 0 of the
other slices being a universal default), the qualified rule sits at the higher
index and wins when the qualifier holds; the unqualified rule below it exists to
override still-lower rules when it does not.

The conditions themselves are FFU classification results — `FFU_DATA_W8B` and
`FFU_DATA_W8C` nibbles — plus two `ACTION_FLAGS` cases at the top of the slice.

⚠ ALL 25 RULES ARE EMITTED. Unlike slice 1, none is redundant: removing an
unqualified rule would let a LOWER-indexed rule match instead, changing which
operand profile is selected. The 12 action-free rules are load-bearing.

usage:
    gen_l3ar_slice4.py --emit | --addrs | --verify <image> | --c FILE
SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from l3ar_decode import (  # noqa: E402
    L3AR_BASE, CAM_SLICE_STRIDE, CAM_RULE_STRIDE, RAM_SLICE_STRIDE,
    RAM1_OFF, RAM2_OFF, RAM3_OFF, RAM4_OFF, RAM5_OFF, RAM3_SLICE_STRIDE,
    KEY_LAYOUT, RAM4_FIELDS, encode_cam, load,
)

SLICE = 4
RULES = 25
FLAG_PASSTHROUGH = 0x3FFFFFF
KEYPOS = {n: (lo, hi) for n, lo, hi in KEY_LAYOUT}

# the port qualifier every action-bearing rule carries
QUAL = {"SRC_PORT_ID4": (0x00, 0x20), "FFU_DATA_W8A": (0x80, 0xD0)}


def q(**extra):
    """the qualifier plus whatever else distinguishes this rule"""
    d = dict(QUAL)
    d.update(extra)
    return d


# (key fields, ALU46 operand profile or None, QoS profile or None, Set_HI)
#
# ⚠ EVERY RULE BUT 0 ALSO SELECTS A QoS PROFILE. That is the stage's real job:
# it maps the FFU's classification nibbles onto QoS profiles, and additionally
# picks an ALU46 operand when the port qualifier holds. Both rules of a pair
# share the QoS profile; only the qualified one carries the ALU action.
#
# QoS profile 0 (rules 22/23) is a real selection, not an absence -- those rules
# set MuxOutput_QOS with profile 0. Only rule 0 drives no QoS at all.
RULESET = {
    0:  (q(),                                                       2, None, 0),
    1:  ({"ACTION_FLAGS": (0x80, 0xC0), "SRC_PORT_ID4": (0x08, 0x08)},
                                                                 None,   10, 0),
    2:  ({"ACTION_FLAGS": (0x80, 0xC0), "SRC_PORT_ID4": (0x08, 0x28),
          "FFU_DATA_W8A": (0x80, 0xD0)},                            2,   10, 0),
    3:  ({"ACTION_FLAGS": (0x40, 0x40), "SRC_PORT_ID4": (0x10, 0x10)},
                                                                 None,   11, 0),
    4:  ({"ACTION_FLAGS": (0x40, 0x40), "SRC_PORT_ID4": (0x10, 0x30),
          "FFU_DATA_W8A": (0x80, 0xD0)},                            2,   11, 0),
    5:  ({"FFU_DATA_W8C": (0x6, 0xF)},                           None,    6, 0),
    6:  (q(FFU_DATA_W8C=(0x6, 0xF)),                                2,    6, 0),
    7:  ({"FFU_DATA_W8C": (0x4, 0xF)},                           None,    5, 0),
    8:  (q(FFU_DATA_W8C=(0x4, 0xF)),                                2,    5, 0),
    9:  ({"FFU_DATA_W8C": (0x2, 0xF)},                           None,    2, 0),
    10: (q(FFU_DATA_W8C=(0x2, 0xF)),                                2,    2, 0),
    11: ({"FFU_DATA_W8C": (0x1, 0x1)},                           None,    8, 0),
    12: (q(FFU_DATA_W8C=(0x1, 0x1)),                                2,    8, 0),
    13: ({"FFU_DATA_W8C": (0x8, 0xF)},                           None,    1, 0),
    14: (q(FFU_DATA_W8C=(0x8, 0xF)),                                2,    1, 0),
    15: ({"ACTION_FLAGS": (0x4080000, 0x4080000), "L2_SMAC_ID3": (0xA, 0x1F),
          "FFU_DATA_W8C": (0x8, 0xF)},                           None,    7, 0),
    16: ({"FFU_DATA_W8B": (0x4, 0xF)},                           None,    4, 0),
    17: (q(FFU_DATA_W8B=(0x4, 0xF)),                                2,    4, 0),
    18: ({"FFU_DATA_W8B": (0x1, 0x3)},                           None,    9, 0),
    19: (q(FFU_DATA_W8B=(0x1, 0x3)),                                2,    9, 0),
    20: ({"FFU_DATA_W8B": (0x2, 0x3)},                           None,    3, 0),
    21: (q(FFU_DATA_W8B=(0x2, 0x3)),                                2,    3, 0),
    22: ({"FFU_DATA_W8B": (0x3, 0x3)},                           None,    0, 0),
    23: (q(FFU_DATA_W8B=(0x3, 0x3)),                                2,    0, 0),
    # ⚠ ALU46_OP_PROFILE spans bits 29-33, so word 1 bit 0 is profile bit 3:
    # this rule's profile is 11, not the 3 a word-0-only read reports.
    24: ({"FFU_DATA_W8A": (0x90, 0xD0)},                            11,    7, 0x8000),
}


def cam_addr(r, seg, w):
    return L3AR_BASE + CAM_SLICE_STRIDE * SLICE + CAM_RULE_STRIDE * r + 4 * seg + w


def ram_addr(off, r, w, stride=RAM_SLICE_STRIDE, per=2):
    return L3AR_BASE + off + stride * SLICE + per * r + w


def key_words(fields):
    value = care = 0
    for name, (v, m) in fields.items():
        lo, hi = KEYPOS[name]
        width = hi - lo + 1
        if v >> width or m >> width:
            raise ValueError(f"{name} does not fit {width} bits")
        value |= (v & m) << lo
        care |= m << lo
    return [encode_cam((value >> (64 * s)) & 0xFFFFFFFFFFFFFFFF,
                       (care >> (64 * s)) & 0xFFFFFFFFFFFFFFFF) for s in range(4)]


def ram4_word(profile, qos):
    """SetAlu46CmdProfile + MuxOutput_ALU46_OP + operand profile, plus the QoS
    mux. Cross-checked against the chip: an ALU profile of 2 with no QoS gives
    word0 0x50010000, and QoS profile 10 gives word1 0x002a0000."""
    acc = 0
    f = {}
    if profile is not None:
        f.update({"SetAlu46CmdProfile": 1, "MuxOutput_ALU46_OP": 1,
                  "ALU46_OP_PROFILE": profile})
    if qos is not None:
        f.update({"MuxOutput_QOS": 1, "QOS_PROFILE": qos})
    for n, lo, w in RAM4_FIELDS:
        v = f.get(n, 0)
        if v >> w:
            raise ValueError(f"{n} too wide")
        acc |= v << lo
    return acc


def build():
    m = {}
    for r in range(RULES):
        fields, profile, qos, set_hi = RULESET[r]
        for seg, (k, ki) in enumerate(key_words(fields)):
            # words 0-1 KeyInvert, words 2-3 Key -- NOT the other way round
            m[cam_addr(r, seg, 0)] = ki & 0xFFFFFFFF
            m[cam_addr(r, seg, 1)] = (ki >> 32) & 0xFFFFFFFF
            m[cam_addr(r, seg, 2)] = k & 0xFFFFFFFF
            m[cam_addr(r, seg, 3)] = (k >> 32) & 0xFFFFFFFF
        # flags: pass through. ACTION_FLAGS' = ACTION_FLAGS & Mask | Value.
        m[ram_addr(RAM1_OFF, r, 0)] = FLAG_PASSTHROUGH
        m[ram_addr(RAM1_OFF, r, 1)] = 0
        m[ram_addr(RAM2_OFF, r, 0)] = FLAG_PASSTHROUGH
        m[ram_addr(RAM2_OFF, r, 1)] = set_hi
        m[ram_addr(RAM3_OFF, r, 0, RAM3_SLICE_STRIDE, 1)] = 0
        act = ram4_word(profile, qos)
        m[ram_addr(RAM4_OFF, r, 0)] = act & 0xFFFFFFFF
        m[ram_addr(RAM4_OFF, r, 1)] = (act >> 32) & 0xFFFFFFFF
        m[ram_addr(RAM5_OFF, r, 0)] = 0
        m[ram_addr(RAM5_OFF, r, 1)] = 0
    return m


def verify(image):
    """Byte-compare every word against EOS's slice 4. Unlike slice 1 we emit the
    same rule count, so byte equality IS the right test here."""
    eos, ours = load(image), build()
    bad = same = missing = 0
    for a in sorted(ours):
        e = eos.get(a)
        if e is None:
            missing += 1
        elif e == ours[a]:
            same += 1
        else:
            if bad < 8:
                print(f"  0x{a:05x}  eos={e:08x}  ours={ours[a]:08x}")
            bad += 1
    print(f"identical {same}, differing {bad}, absent from image {missing}")
    print("VERIFY PASS" if bad == 0 else "VERIFY FAIL")
    return 1 if bad else 0


C_HEAD = r'''/* fm6000_l3arslice4.c - L3AR slice 4: QoS classification + ALU46 operand select.
 *
 * GENERATED by asic/fm6000/tools/gen_l3ar_slice4.py --c. Edit the generator.
 *
 * The stage maps the FFU's classification nibbles (FFU_DATA_W8B / W8C) onto QoS
 * profiles, and additionally selects an ALU46 operand profile when a port
 * qualifier holds. Rules come in pairs:
 *
 *     rule N     a condition                            -> QoS only
 *     rule N+1   the same condition AND the qualifier    -> QoS + ALU46 operand
 *
 * The qualifier is SRC_PORT_ID4=0x00/0x20 with FFU_DATA_W8A=0x80/0xd0. A slice
 * resolves LAST MATCH WINS, so the qualified rule sits at the higher index and
 * wins when the qualifier holds; the unqualified rule below it overrides
 * still-lower rules when it does not.
 *
 * WARNING: all 25 rules are emitted and none is redundant. Dropping an
 * unqualified rule would let a LOWER-indexed rule match instead and select a
 * different QoS profile.
 *
 * WARNING: RAM1/RAM2 carry Mask=0x3ffffff, Value=0 -- flags pass through, since
 * ACTION_FLAGS' = ACTION_FLAGS & Mask | Value. Zero would clear all 52 flags.
 * Rule 24 additionally sets Set_HI bit 15.
 *
 * Verified byte-identical to EOS's slice 4: 625 of 625 words.
 *
 * usage: fm6000_l3arslice4 [-n | -a] [-b bdf]
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
		fprintf(stderr, "fm6000_l3arslice4: %d writes\n",
		        (int)(sizeof W / sizeof W[0]));
	return 0;
}
'''


def emit_c(path):
    m = build()
    used = set()
    with open(path, "w") as f:
        f.write(C_HEAD)
        for r in range(RULES):
            _fields, prof, qos, _sh = RULESET[r]
            note = "QoS %s" % ("none" if qos is None else qos)
            if prof is not None:
                note += ", ALU46 operand %d" % prof
            f.write("\t/* rule %2d -- %s */\n" % (r, note))
            a = [cam_addr(r, s_, w) for s_ in range(4) for w in range(4)]
            a += [ram_addr(RAM1_OFF, r, 0), ram_addr(RAM1_OFF, r, 1),
                  ram_addr(RAM2_OFF, r, 0), ram_addr(RAM2_OFF, r, 1),
                  ram_addr(RAM3_OFF, r, 0, RAM3_SLICE_STRIDE, 1),
                  ram_addr(RAM4_OFF, r, 0), ram_addr(RAM4_OFF, r, 1),
                  ram_addr(RAM5_OFF, r, 0), ram_addr(RAM5_OFF, r, 1)]
            for x in sorted(a):
                used.add(x)
                f.write("\t{ 0x%06x, 0x%08x },\n" % (x, m[x]))
        for x in sorted(set(m) - used):
            f.write("\t{ 0x%06x, 0x%08x },\n" % (x, m[x]))
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
