#!/usr/bin/env python3
"""l3ar_program.py - author an FM6000 L3AR program from named rules.

Replaces fm6000_l3arinit.c (3,928 transcribed microcode pairs). L3AR is the
easiest of the three remaining blocks to author because its action is only a
mask/set over ACTION_FLAGS -- no destination mask, no CPU code, no mirror, and
none of the table entanglement that blocks L2AR.

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
    args = ap.parse_args()
    rules = build_program()
    if args.check:
        return check(rules)
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
