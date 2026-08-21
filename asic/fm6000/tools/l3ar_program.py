#!/usr/bin/env python3
"""l3ar_program.py - author an FM6000 L3AR program from named rules.

Replaces fm6000_l3arinit.c (3,928 transcribed microcode pairs), for SLICE 0.

⚠⚠ RETRACTION (2026-08-20). This file used to open by saying L3AR "is the easiest
of the three remaining blocks to author because its action is only a mask/set over
ACTION_FLAGS". That is FALSE, and it is the premise this whole tool was designed
around. There are FIVE RAM banks; the old decoder read two. Besides SetFlags,
datasheet Table 5-31 lists 6 sequential actions and 21 output mux actions whose
operands live in RAM3/RAM4/RAM5 and index 19 profile tables. See
docs/L3AR-STRUCTURE.md for the verified map.

⚠ CONSEQUENCE FOR --emit: what this tool emits covers RAM1/RAM2 only. For slice 0,
EOS writes a nonzero RAM3, RAM4 and RAM5 word for all 32 rules. Emitting this
program in place of EOS's slice 0 therefore DROPS those 96 action words. That is
not currently a live hazard -- nothing in fm6000-fullseq.sh runs this tool -- but
--emit must not be spliced until RAM3/4/5 are covered.

    l3ar_program.py --check
    l3ar_program.py --emit            # <addr> <value> writes
    l3ar_program.py --c FILE          # drop-in fm6000_l3arinit.c
    l3ar_program.py --diff <image>    # our rules vs EOS's, per named rule

⚠ WHAT "OURS" MEANS HERE, because it is a fair question. We are replacing only
L3AR and keeping EOS's L2AR, FFU and mapper configuration, so our flag rewrites
must satisfy what those stages already expect. Some emitted bits therefore
coincide with EOS's. That is interface conformance, not transcription -- the same
reason our parser writes the DMAC to channel 7: the hardware reads it there. What
makes this ours is that each rule is authored from its documented behaviour and a
named intent, not copied from a table.

⚠ SCOPE. This authors the switching rules EdgeNOS actually needs -- normal
switching with and without loopback suppression, broadcast, and the ISIS
control-plane MACs. EOS's 153 rules also cover VXLAN, MPLS, tap aggregation and
routed-multicast cases that EdgeNOS does not implement (see the 7150S datasheet
feature list in PARSER-CONVENTIONS.md). Rules we do not author are simply absent,
not stubbed.

⚠ UNTESTED ON HARDWARE. --diff compares against EOS rule by rule; that is the
strongest local check available and it is not the same as forwarding.

⚠⚠ READ THIS BEFORE QUOTING "12 of 12 rules match EOS exactly". That number is
weaker evidence of independence than the equivalent parser number, and the
difference should be stated rather than glossed:

    parser   1 of 1,568 non-trivial writes coincide with EOS
    L3AR    61 of    61 non-trivial writes coincide with EOS

The reason is that L3AR has almost no encoding freedom. A rule is a 252-bit
ternary match plus a 52-bit mask/set; once the intent is fixed ("routed, not
multicast, next-hop resolved") and the header gives the field positions, the
words are FORCED. 179 of our 240 writes are structural zeros. Two programs that
agree on intent cannot disagree on bits here, so agreement is not the
achievement -- with the parser it was, because there the same intent had many
legal encodings and ours differed from EOS's in nearly all of them.

What IS ours: each rule is stated as a named intent (loopback_suppress=False,
default_dglort=False, forces={AF_MOD_DO_ROUTE: True}) and the masks fall out of
that policy rather than being copied. But honesty requires the other half: which
bits carry that intent -- AF51, AF45, key bit 167, key bit 174, the next-hop
group at 40/43/46/49 -- was learned by reading EOS's image and naming the bits
from the rule-name file and the header. That is fact-extraction about a hardware
interface, the same move as the parser writing DMAC to channel 7, but it is a
larger share of the information content here than it was there. Anyone auditing
this block should weigh it on the process, not on the diff.
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import load  # noqa: E402
from l3ar_decode import (  # noqa: E402
    KEY_LAYOUT, RAM_LAYOUT, RAM2_LAYOUT, cam_addr, ram_addr, encode_cam,
    encode_ram, read_rule, rule_key, fields, SEGMENTS, NUM_SLICES,
    RULES_PER_SLICE,
)

KEY_POS = {n: (lo, hi) for n, lo, hi in KEY_LAYOUT}

# ★ THE 52-BIT ACTION_FLAGS MAP, datasheet Table 5-35. This is what makes the
# rules authorable from intent instead of copied as bit patterns:
#
#   23:0    from HEADER_FLAGS; these BECOME MOD_FLAGS, so anything egress needs
#           to do must be flagged here
#   32:24   scratch, become FORWARD_FLAGS (scheduler fixed-function)
#   33      StrictDestGlort
#   35:34   PAUSE reception
#   39:36   parser error status (39 ParityError, fixed pipeline-wide)
#   51:40   set by fixed-function circuitry in the mapper, FFU or next-hop
#
# The action is  ACTION_FLAGS' = ACTION_FLAGS & Mask | Value  (datasheet 5.10.5),
# rippling stage to stage -- so anything not overwritten survives.
#
# RAM1 carries bits 25:0 (Mask_LO/Set_LO), RAM2 bits 51:26 (Mask_HI/Set_HI).
#
# Read that way EOS's rules explain themselves:
#   UnicastRoutingWithoutLoopbackSuppress sets Set_LO=0x4000 -- bit 14, inside
#   the MOD_FLAGS range -- i.e. it tells the egress modifier to perform the
#   routing edits (TTL decrement, MAC rewrite). That is the L3AR->MOD handoff.
#   Loopback suppress is Set_HI bit 25 = ACTION_FLAGS bit 51, in the
#   fixed-function range the mapper/FFU/next-hop stages own.
#
# Flag bits in the L3AR view of ACTION_FLAGS. Only the ones we act on are named;
# the rest of the 52 are passed through by the mask.
# Bit names are read off the rules that use them: a rule called
# "...AndDefaultDglort" matching bit 45 tells us bit 45 means "the DGLORT is the
# default one". That is the same move as naming a parser state from the channels
# it writes, and it is why the rule-name file matters as much as the header.
FLAG_BROADCAST = 12          # matched by SwitchBroadcast*
FLAG_SPECIAL_DELIVERY = 0    # matched by SpecialDelivery
FLAG_ISL_FTYPE = 3           # matched by IslF64FtypeNormal (0x408/0x409)
FLAG_ISL_NORMAL = 10         # ditto
FLAG_ROUTED = 8              # in the Unicast/Multicast routing match (0x100)
FLAG_DEFAULT_DGLORT = 45     # ...AndDefaultDglort; also SET by IslF64FtypeNormal
                             # (HI bit 19), which my first guess of
                             # StrictDestGlort got wrong
FLAG_MCAST = 13              # in the multicast-route match (0x2000)

LOOPBACK_SUPPRESS = 25       # HI bit 25 = ACTION_FLAGS 51; set by every
                             # ...WithLoopbackSuppress rule
MOD_DO_ROUTE = 14            # LO bit 14, inside the MOD_FLAGS range: tells the
                             # egress modifier to do the routing edits -- TTL
                             # decrement and MAC rewrite. The L3AR -> MOD handoff.
MOD_TUNNEL = 6               # LO bit 6, set by the IP-tunnel rules (not authored)
STRICT_DEST_GLORT = 7        # HI bit 7 = ACTION_FLAGS 33, Table 5-35

# ★ THE KEEP-MASKS ARE PER-RULE, AND THEY ARE DERIVABLE FROM INTENT.
#
# The first four rules authored here matched EOS using three fixed mask
# constants, which made those look like pipeline-wide settings. They are not.
# Since ACTION_FLAGS' = FLAGS & Mask | Value, each rule has two ways to force a
# flag and one way to leave it alone, and EOS uses all three deliberately:
#
#     force ON    Set bit = 1, mask bit kept    (the OR wins)
#     force OFF   Set bit = 0, mask bit CLEARED (the AND wins)
#     undecided   Set bit = 0, mask bit kept    (upstream's value stands)
#
# So a rule's mask is not boilerplate: it records which flags the rule decides,
# and it is recoverable from the rule's own name. Measured over all 149 rules in
# EOS's L3AR (see --policy):
#
#   AF51 LoopbackSuppress   every ...WithoutLoopbackSuppress rule forces it off;
#                           every ...WithLoopbackSuppress rule forces it on.
#                           6 and 14 rules, one anomaly (see below).
#   AF45 DefaultDglort      all 5 ...AndDefaultDglort rules force it off; one
#                           further rule does (AndMcastNoRoute, s0r9).
#   AF48 Boundary           the single ...Boundary rule forces it off. A 1:1
#                           correlation with n=1 is a hint, not a fact.
#
# ⚠ ANOMALY, recorded rather than smoothed over: s0r24
# MulticastRouteWithLoopbackSuppress forces AF51 OFF, contradicting its name.
# s0r25 MulticastRoutingWithoutLoopbackSuppress does the same and is named for
# it. Either the name is wrong or EOS has a bug here; we do not author s0r24, so
# nothing here depends on the answer.
#
# ⚠ AF33 is NOT the tunnel flag. 25 rules whose names lack "Tunnel" decide it
# off, and 19 rules that HAVE "Tunnel" pass it through. What distinguishes the
# IPTUN rules is that they PRESERVE AF33 and AF41 where the baseline clears
# them. An earlier note in this repo called AF33 a tunnel flag; that was
# backwards, and STRICT_DEST_GLORT below is retained only as the header's name
# for the bit, not as a claim about who sets it.

# Flags this stage always resolves, whatever the rule -- the baseline clear-set,
# read off the mask value common to all 149 rules. A rule may opt back out of any
# of these via preserves=.
BASELINE_DECIDES = frozenset({6, 7, 14, 16, 28, 33, 41})

AF_LOOPBACK_SUPPRESS = 51
AF_DEFAULT_DGLORT = 45
AF_BOUNDARY = 48
AF_MOD_DO_ROUTE = 14
AF_ARP_RESOLVED = 2          # unnamed in the header; the bit that distinguishes
                             # UnicastNoARP (forces it off) from UnicastRouting
                             # (leaves it). Named from that contrast alone, so
                             # treated as provisional.
ALL_FLAGS = (1 << 52) - 1


class Rule:
    """One L3AR rule: a match over named key fields, and a flag rewrite.

    Flag policy is stated as intent, not as mask constants. Each keyword takes
    True (force on), False (force off) or None (this rule does not decide it),
    and the encoder picks the spelling the hardware needs.
    """

    def __init__(self, slice_, index, name, match=None,
                 loopback_suppress=None, default_dglort=None, boundary=None,
                 forces=(), preserves=()):
        self.slice = slice_
        self.index = index
        self.name = name
        self.match = match or {}      # {key field: (value, mask)}
        self.policy = dict(forces)
        for af, want in ((AF_LOOPBACK_SUPPRESS, loopback_suppress),
                         (AF_DEFAULT_DGLORT, default_dglort),
                         (AF_BOUNDARY, boundary)):
            if want is not None:
                self.policy[af] = want
        self.preserves = frozenset(preserves)

    def flags(self):
        """(mask, set) over the 52-bit ACTION_FLAGS, from the policy above."""
        decides = (BASELINE_DECIDES - self.preserves) | {
            af for af, want in self.policy.items() if want is False}
        mask = ALL_FLAGS & ~sum(1 << af for af in decides)
        setv = sum(1 << af for af, want in self.policy.items() if want)
        return mask, setv

    @property
    def mask_lo(self):
        return self.flags()[0] & 0x3FFFFFF

    @property
    def mask_hi(self):
        return (self.flags()[0] >> 26) & 0x3FFFFFF

    @property
    def set_lo(self):
        return self.flags()[1] & 0x3FFFFFF

    @property
    def set_hi(self):
        return (self.flags()[1] >> 26) & 0x3FFFFFF

    def key(self):
        value = care = 0
        for field, (v, m) in self.match.items():
            lo, hi = KEY_POS[field]
            width = hi - lo + 1
            if v >> width or m >> width:
                raise ValueError(f"{field} value/mask exceeds {width} bits")
            value |= (v & m) << lo
            care |= m << lo
        return value, care

    def rams(self):
        return (encode_ram({"Mask_LO": self.mask_lo, "Set_LO": self.set_lo}, RAM_LAYOUT),
                encode_ram({"Mask_HI": self.mask_hi, "Set_HI": self.set_hi}, RAM2_LAYOUT))


def build_program():
    """The rules EdgeNOS needs. Each is authored from its documented intent."""
    r = []
    # ★ THE DEFAULT RULE, AND IT IS LOAD-BEARING. Rule 0 matches EVERY frame --
    # all 252 key bits don't-care -- and applies this stage's baseline flag
    # resolution plus LoopbackSuppress. Every other rule is an override of it,
    # resolved by last-match-wins.
    #
    # Hardware proved it necessary: with slice 0 replaced but this rule absent,
    # et1 went to 100% packet loss; leave-one-out across all 20 unauthored rules
    # showed rule 0 was the ONLY one whose removal broke forwarding. It is also
    # why the mask "baseline" exists at all -- it is not a statistical mode over
    # 149 rules, it is this rule's action applied to everything.
    r.append(Rule(0, 0, "SwitchNormalWithLoopbackSuppress",
                  loopback_suppress=True))
    # Normal switching without loopback suppression: the default path. EOS
    # qualifies it on the mapper's per-port and per-VLAN tags rather than on
    # frame content, which is what SRC_PORT_ID4 and MAP_VID2 are for.
    r.append(Rule(0, 1, "SwitchNormalWithoutLoopbackSuppress",
                  match={"SRC_PORT_ID4": (1, 1), "MAP_VID2": (1, 1)},
                  loopback_suppress=False))
    # The ISIS control-plane MACs, classified by the mapper into DMAC_CAM3 IDs.
    # Both suppress loopback: an ISIS hello must not be reflected to its sender.
    for idx, dmac_id, nm in ((2, 0xC, "SwitchIsisP2PWithLoopbackSuppress"),
                             (3, 0xB, "SwitchIsisLanWithLoopbackSuppress")):
        r.append(Rule(0, idx, nm, match={"L2_DMAC_ID3": (dmac_id, 0x1F)},
                      loopback_suppress=True))
    # Broadcast, matched on the pipeline's broadcast flag rather than on the
    # DMAC -- by this stage the mapper has already made that determination.
    r.append(Rule(0, 4, "SwitchBroadcastWithLoopbackSuppress",
                  match={"ACTION_FLAGS": (1 << FLAG_BROADCAST, 1 << FLAG_BROADCAST)},
                  loopback_suppress=True))

    # --- the ISL/F64 path: frames arriving with the tag the CPU port uses ---
    isl = (1 << FLAG_ISL_NORMAL) | (1 << FLAG_ISL_FTYPE)
    r.append(Rule(0, 5, "IslF64FtypeNormal",
                  match={"ACTION_FLAGS": (isl, isl | (1 << FLAG_SPECIAL_DELIVERY))},
                  loopback_suppress=True, default_dglort=True))

    # --- switched unicast where the DGLORT is the default for the VLAN ---
    # The DGLORT pair is discriminated by FFU_DATA_W24_TOP bit 3, not by an
    # ACTION_FLAGS bit: the FFU has already classified the frame and hands the
    # answer down in its 24-bit data word. Default = 1, directed = 0.
    dg = 1 << FLAG_DEFAULT_DGLORT
    FFU_W24_DEFAULT_DGLORT = 0x8
    # "AndDefaultDglort" in the name is the rule RESOLVING that flag, and it
    # resolves it off -- the DGLORT has been chosen by now, so the "use the
    # default" hint must not survive into the next stage.
    r.append(Rule(0, 7, "SwitchNormalWithLoopbackSuppressAndDefaultDglort",
                  match={"ACTION_FLAGS": (dg, dg),
                         "FFU_DATA_W24_TOP": (FFU_W24_DEFAULT_DGLORT,
                                              FFU_W24_DEFAULT_DGLORT)},
                  loopback_suppress=True, default_dglort=False))
    # Directed, not default: this one leaves AF45 alone.
    r.append(Rule(0, 11, "SwitchNormalWithLoopbackSuppressAndDirectedDglort",
                  match={"ACTION_FLAGS": (dg, dg),
                         "FFU_DATA_W24_TOP": (0, FFU_W24_DEFAULT_DGLORT)},
                  loopback_suppress=True))

    # --- routing. Set_LO bit 14 is the instruction to MOD: decrement the TTL
    # and rewrite the MAC. Without it a routed frame egresses unmodified. ---
    # Routed, NOT multicast, and the three fixed-function flags the next-hop
    # stage sets on a resolved L3 lookup -- with bit 49 required clear.
    route_v = (1 << FLAG_ROUTED) | 0x490000000000
    route_m = (1 << FLAG_ROUTED) | (1 << FLAG_MCAST) | 0x2490000000000
    # Same per-port and per-VLAN enables rule 1 uses, one bit up; and the frame
    # must not be carrying the FFU trap flag, which rules 29/30 handle instead.
    route_gate = {"SRC_PORT_ID4": (2, 2), "MAP_VID2": (2, 2),
                  "FFU_DATA_W8A": (0, 0x20)}
    # NEXTHOP_TAG bit 2 is the next-hop lookup reporting "no ARP entry". That is
    # the ONLY thing separating rules 20 and 23 -- both match identically
    # otherwise -- and it is exactly where an unresolved next hop should be
    # reported from. Rule 20 leaves it unconstrained; 23 requires it.
    NEXTHOP_NO_ARP = 0x4
    r.append(Rule(0, 20, "UnicastRoutingWithoutLoopbackSuppress",
                  match={"ACTION_FLAGS": (route_v, route_m), **route_gate},
                  loopback_suppress=False,
                  forces={AF_MOD_DO_ROUTE: True}, preserves={AF_MOD_DO_ROUTE}))
    # Same match, no egress edit: the next hop is unresolved, so the frame is
    # punted for ARP rather than routed.
    r.append(Rule(0, 23, "UnicastNoARPWithoutLoopbackSuppress",
                  match={"ACTION_FLAGS": (route_v, route_m), **route_gate,
                         "NEXTHOP_TAG": (NEXTHOP_NO_ARP, NEXTHOP_NO_ARP)},
                  loopback_suppress=False,
                  forces={AF_ARP_RESOLVED: False}))

    # --- FFU-driven trap and drop, matched on the FFU's 8-bit action data ---
    r.append(Rule(0, 29, "ffuFlagTrapAlwaysFrame",
                  match={"FFU_DATA_W8A": (0xC0, 0xC0)}, loopback_suppress=False))
    r.append(Rule(0, 30, "ffuFlagTrapFrame",
                  match={"FFU_DATA_W8A": (0x40, 0xC0)}, loopback_suppress=False))
    # A dropped frame is never egressed, so loopback suppression is moot and this
    # rule declines to decide it -- which is exactly what EOS's mask says.
    r.append(Rule(0, 31, "ffuFlagDropFrame",
                  match={"FFU_DATA_W8A": (0x10, 0x10)}))
    return r


C_TEMPLATE = r"""/* fm6000_l3arinit.c - program the L3AR block ourselves.
 *
 * GENERATED by asic/fm6000/tools/l3ar_program.py --c -- do not edit by hand.
 * Regenerate rather than patching; the rules live in that file as named intents.
 *
 * L3AR: L3 action resolution. Replaces %(eos)d transcribed microcode pairs from
 * the previous generator with %(ours)d writes, of which %(nonzero)d are non-zero.
 *
 * SCOPE: SLICE 0 ONLY -- the forwarding rules.
 *
 * ⚠ The line that used to stand here -- "slices 1-4 are csGlort assignment,
 * policers, storm control and L3 QoS" -- was UNCITED, and reading it as fact is
 * what produced alpha30, which deleted those slices and killed the dataplane.
 * Decoded 2026-08-20 with the real RAM3/4/5 field layout (docs/L3AR-STRUCTURE.md):
 * slice 1 IS csGlort assignment -- every rule is MuxOutput_SGLORT +
 * MuxOutput_CSGLORT with a distinct CSGLORT_PROFILE -- so the claim was right for
 * slice 1, but it was uncited when it was acted on, and it is wrong for the rest:
 * slice 2 is VID assignment and trap header, slice 3 is the ALU13/ALU46 command
 * and operand profiles, slice 4 is ALU46 alone. Policers and QoS are RAM4 fields
 * spread across slices, not a slice of their own. alpha30 deleted all four.
 * Separate
 * functions EdgeNOS
 * does not author, left in the replay exactly as EOS wrote them. `-a` therefore
 * lists all %(ours)d slice-0 addresses and nothing else, so the boot script
 * strips EOS's slice 0 and only that.
 *
 * The %(zeros)d zero writes are not padding. Key=0,KeyInvert=0 is the FM6000
 * never-match state, so they explicitly disable the %(unauth)d rules we do not
 * author. Omitting them would leave EOS's rules resident whenever the chip is
 * programmed without a reset, and L3AR resolves LAST match wins -- EOS's
 * higher-index rules would beat ours.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

static const uint32_t R[][2] = {
%(rows)s};

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	char p[256]; int fd, i, dry = 0, list = 0;
	volatile uint32_t *M = NULL;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n")) dry = 1;
		else if (!strcmp(argv[i], "-a")) list = 1;
		else bdf = argv[i];
	}
	if (list) {
		for (i = 0; i < (int)(sizeof R / sizeof R[0]); i++)
			printf("%%08x\n", R[i][0]);
		return 0;
	}
	if (!dry) {
		snprintf(p, sizeof p, "/sys/bus/pci/devices/%%s/resource0", bdf);
		fd = open(p, O_RDWR | O_SYNC);
		if (fd < 0) { perror(p); return 1; }
		M = mmap(NULL, 32u*1024*1024, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
		if (M == MAP_FAILED) { perror("mmap"); return 1; }
	}
	for (i = 0; i < (int)(sizeof R / sizeof R[0]); i++) {
		if (dry) printf("%%08x %%08x\n", R[i][0], R[i][1]);
		else { M[R[i][0]] = R[i][1]; __sync_synchronize(); }
	}
	if (!dry)
		printf("fm6000_l3arinit: %%d writes\n",
		       (int)(sizeof R / sizeof R[0]));
	return 0;
}
"""


def emit_c(out_path):
    rules = build_program()
    ws = slice_writes(rules)
    rows = "".join(f"\t{{0x{a:08x},0x{v:08x}}},\n" for a, v in ws)
    nonzero = sum(1 for _, v in ws if v)
    src = C_TEMPLATE % dict(eos=3928, ours=len(ws), nonzero=nonzero,
                            zeros=len(ws) - nonzero,
                            unauth=RULES_PER_SLICE - len(rules), rows=rows)
    with open(out_path, "w") as fh:
        fh.write(src)
    print(f"wrote {out_path}: {len(ws)} writes ({nonzero} non-zero)")
    return 0


def slice_writes(rules, slice_=0):
    """Every address of a slice: our rules, and never-match for the rest.

    ⚠ OMITTING A RULE IS NOT THE SAME AS DISABLING IT. fm6000load programs the
    chip without resetting it, so EOS's slice-0 rules stay resident unless they
    are overwritten -- and L3AR resolves LAST match wins, so EOS's higher-index
    rules would beat ours and the test would silently measure EOS. An all-zero
    CAM word pair is Key=0,KeyInvert=0, the never-match state, so the 20 rules we
    do not author are written off explicitly rather than left behind.

    This replaces slice 0 only. Slices 1-4 are functions we do not author and do
    not claim to have identified (see the retraction above), left in the
    replay exactly as EOS wrote them.
    """
    authored = {r.index for r in rules if r.slice == slice_}
    out = list(writes([r for r in rules if r.slice == slice_]))
    for idx in range(RULES_PER_SLICE):
        if idx in authored:
            continue
        for seg in range(SEGMENTS):
            base = cam_addr(slice_, idx, seg, 0)
            out += [(base + i, 0) for i in range(4)]
        for second in (False, True):
            out += [(ram_addr(slice_, idx, i, second), 0) for i in range(2)]
    return sorted(out)


def writes(rules):
    out = []
    for rule in rules:
        value, care = rule.key()
        for seg in range(SEGMENTS):
            v = (value >> (64 * seg)) & 0xFFFFFFFFFFFFFFFF
            c = (care >> (64 * seg)) & 0xFFFFFFFFFFFFFFFF
            key, keyinvert = encode_cam(v, c)
            base = cam_addr(rule.slice, rule.index, seg, 0)
            for i, w in enumerate([keyinvert & 0xFFFFFFFF, (keyinvert >> 32) & 0xFFFFFFFF,
                                   key & 0xFFFFFFFF, (key >> 32) & 0xFFFFFFFF]):
                out.append((base + i, w))
        ram1, ram2 = rule.rams()
        for second, raw in ((False, ram1), (True, ram2)):
            for i in range(2):
                out.append((ram_addr(rule.slice, rule.index, i, second),
                            (raw >> (32 * i)) & 0xFFFFFFFF))
    return sorted(out)


def check(rules):
    problems = []
    seen = set()
    for rule in rules:
        if (rule.slice, rule.index) in seen:
            problems.append(f"duplicate slot slice{rule.slice} rule{rule.index}")
        seen.add((rule.slice, rule.index))
        if rule.slice >= NUM_SLICES:
            problems.append(f"{rule.name}: slice {rule.slice} out of range")
        if rule.index >= RULES_PER_SLICE:
            problems.append(f"{rule.name}: index {rule.index} out of range")
        try:
            rule.key()
            rule.rams()
        except ValueError as e:
            problems.append(f"{rule.name}: {e}")
    for p in problems:
        print("  PROBLEM:", p)
    print("check " + ("PASS" if not problems else "FAIL"))
    return 1 if problems else 0


def diff(rules, image):
    """Compare our rules against EOS's, slot by slot."""
    mem = load(image)
    print("rule                                    match          flags")
    bad = 0
    for rule in rules:
        res = read_rule(mem, rule.slice, rule.index)
        ours_v, ours_c = rule.key()
        our_r1, our_r2 = rule.rams()
        if not res:
            print(f"  {rule.name:<38} EOS slot empty")
            bad += 1
            continue
        segs, r1, r2 = res
        eos_v, eos_c = rule_key(segs)
        m = "same" if (eos_v, eos_c) == (ours_v, ours_c) else "DIFFER"
        f = "same" if (r1, r2) == (our_r1, our_r2) else "DIFFER"
        if "DIFFER" in (m, f):
            bad += 1
        print(f"  {rule.name:<38} {m:<14} {f}")
        if f == "DIFFER":
            print(f"      ours  {fields(our_r1, RAM_LAYOUT)} {fields(our_r2, RAM2_LAYOUT)}")
            print(f"      EOS   {fields(r1, RAM_LAYOUT)} {fields(r2, RAM2_LAYOUT)}")
    print(f"\n{len(rules) - bad} of {len(rules)} rules match EOS exactly")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--emit", action="store_true")
    ap.add_argument("--diff", metavar="IMAGE")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--c", metavar="FILE", help="emit fm6000_l3arinit.c")
    args = ap.parse_args()
    rules = build_program()
    if args.check:
        return check(rules)
    if args.c:
        return emit_c(args.c)

    if args.summary:
        print(f"rules authored: {len(rules)}")
        for r in rules:
            print(f"  slice{r.slice} rule{r.index:>2}  {r.name}")
        print(f"writes: {len(writes(rules))}")
    if args.emit:
        for a, v in writes(rules):
            print(f"{a:08x} {v:08x}")
    if args.diff:
        return diff(rules, args.diff)
    if not (args.check or args.emit or args.diff or args.summary):
        ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
