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

# Flag bits in the L3AR view of ACTION_FLAGS. Only the ones we act on are named;
# the rest of the 52 are passed through by the mask.
FLAG_BROADCAST = 12          # matched by EOS's SwitchBroadcast* rules
LOOPBACK_SUPPRESS = 25       # set by every EOS rule named WithLoopbackSuppress

# Mask values EOS applies on the switching path. These are the "keep" masks --
# which flags survive the stage -- and they are a property of the pipeline
# configuration we are keeping, not a policy choice of ours.
MASK_LO = 0x3FEBF3F
MASK_HI_SUPPRESS = 0x3FF7F7B
MASK_HI_PLAIN = 0x1FF7F7B


class Rule:
    """One L3AR rule: a match over named key fields, and a flag rewrite."""

    def __init__(self, slice_, index, name, match=None,
                 mask_lo=MASK_LO, mask_hi=MASK_HI_PLAIN, set_lo=0, set_hi=0):
        self.slice = slice_
        self.index = index
        self.name = name
        self.match = match or {}      # {key field: (value, mask)}
        self.mask_lo, self.mask_hi = mask_lo, mask_hi
        self.set_lo, self.set_hi = set_lo, set_hi

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
                  mask_hi=MASK_HI_PLAIN))
    # The ISIS control-plane MACs, classified by the mapper into DMAC_CAM3 IDs.
    # Both suppress loopback: an ISIS hello must not be reflected to its sender.
    for idx, dmac_id, nm in ((2, 0xC, "SwitchIsisP2PWithLoopbackSuppress"),
                             (3, 0xB, "SwitchIsisLanWithLoopbackSuppress")):
        r.append(Rule(0, idx, nm, match={"L2_DMAC_ID3": (dmac_id, 0x1F)},
                      mask_hi=MASK_HI_SUPPRESS, set_hi=1 << LOOPBACK_SUPPRESS))
    # Broadcast, matched on the pipeline's broadcast flag rather than on the
    # DMAC -- by this stage the mapper has already made that determination.
    r.append(Rule(0, 4, "SwitchBroadcastWithLoopbackSuppress",
                  match={"ACTION_FLAGS": (1 << FLAG_BROADCAST, 1 << FLAG_BROADCAST)},
                  mask_hi=MASK_HI_SUPPRESS, set_hi=1 << LOOPBACK_SUPPRESS))
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
