#!/usr/bin/env python3
"""parser_program.py - author an FM6000 parser program from a protocol list.

This is the thing the decode work was for: a parser we WRITE, not one we copy.
Output is CAM+RAM register writes, emitted by our own code from a declarative
description. No table from EOS is read, embedded or consulted at runtime --
unlike parser_decode.py, this tool needs no --image at all.

    parser_program.py --emit                  # <addr> <value> writes
    parser_program.py --check                 # structural validation
    parser_program.py --summary               # what the program does

WHAT THE PROGRAM MUST SATISFY, all established in docs/PARSER-CONVENTIONS.md:

  * FIELDS channels are fixed in HARDWARE (datasheet Table 5-5). The DMAC goes
    in channels 7/6/5 because that is where the chip reads it. We do not choose.
  * Parsing starts at STATE8[0] = 0x00, slice 0.
  * STATE8[0] is the state variable; every rule constrains it. Bytes 1-3
    qualify it -- here byte 1 carries VLAN tag depth.
  * A state is NOT a slice. The same header position lands at a different slice
    per tag count, so every rule is emitted at EVERY slice where its state is
    reachable. Emitting once yields a parser that handles untagged frames and
    silently drops tagged ones.

GEOMETRY. window_shift stays 0, so slice i sees frame bytes [4i, 4i+3] and a
position at byte offset B is parsed by slice B//4. The 32-bit frame key holds
those 4 bytes with the first halfword in the low 16 bits -- fixed by the known
IPv4 rule, whose EtherType 0x0800 reads directly as 0x0800 in key[15:0].
Halfword0 is bytes [0,1] of the window, Halfword1 is bytes [2,3].

⚠ UNTESTED ON HARDWARE. Structural checks only. Per SELF-CONTAINED-PLAN.md
nothing counts until a cold boot with a stock-replay control at the same
cadence, and the pre-existing ping/OSPF degradation makes that control
mandatory rather than optional.

⚠ SCOPE. L2 is complete: DMAC, SMAC, EtherType, and 0/1/2 VLAN tags including
VID and priority. L3 is deliberately NOT emitted yet -- Table 5-5 lists
identical channel sets for L3_SIP and L3_DIP, which cannot both be right, and
guessing there would produce a parser that looks finished and forwards wrong.
See --summary for exactly what is and is not covered.
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import (  # noqa: E402
    PARSER_BASE, SLICE_STRIDE, RAM_OFFSET, WORDS_PER_ENTRY,
    ENTRIES_PER_SLICE, NUM_SLICES, FIELDS_CHANNEL,
)
from gen_parser import encode_cam, encode_action  # noqa: E402

# ---------------------------------------------------------------- field map
# Table 5-5, hardware-fixed. Names are ours; the numbers are not negotiable.
CH_VID1 = 1
CH_SGLORT = 3
CH_DMAC_HI, CH_DMAC_MID, CH_DMAC_LO = 7, 6, 5
CH_SMAC_HI, CH_SMAC_MID, CH_SMAC_LO = 14, 13, 12
CH_ETHERTYPE = 15

ET_VLAN_C = 0x8100
ET_VLAN_S = 0x88A8
ET_IPV4 = 0x0800
ET_IPV6 = 0x86DD
ET_ARP = 0x0806

# ---------------------------------------------------------------- states
# Ours to choose: STATE8[0] is internal to the parser and no downstream block
# sees it. Values are picked to be readable in a trace, not to match EOS.
S_START = 0x00       # slice 0, before any bytes
S_DMAC_LO = 0x10     # consumed DMAC[47:16]
S_SMAC_LO = 0x12     # consumed DMAC[15:0] + SMAC[47:32]
S_ETYPE = 0x14       # consumed SMAC[31:0]; next halfword is an EtherType
S_TAGGED = 0x16      # just consumed a VLAN tag; another EtherType follows
S_DONE_IPV4 = 0x20
S_DONE_IPV6 = 0x22
S_DONE_ARP = 0x24
S_DONE_OTHER = 0x26

STATE_NAME = {
    S_START: "start", S_DMAC_LO: "dmac-hi-consumed", S_SMAC_LO: "smac-hi-consumed",
    S_ETYPE: "at-ethertype", S_TAGGED: "at-ethertype(after tag)",
    S_DONE_IPV4: "ipv4", S_DONE_IPV6: "ipv6", S_DONE_ARP: "arp",
    S_DONE_OTHER: "other-l3",
}

MAX_TAGS = 2


class Rule:
    """One CAM+RAM entry, before slice placement."""

    def __init__(self, state, next_state, tag_depth=0, next_tag_depth=None,
                 match_halfword0=None, dest0=None, dest1=None,
                 terminate=False, note=""):
        self.state = state
        self.next_state = next_state
        self.tag_depth = tag_depth
        self.next_tag_depth = tag_depth if next_tag_depth is None else next_tag_depth
        self.match_halfword0 = match_halfword0   # exact 16-bit match on bytes[0:1]
        self.dest0 = dest0                       # FIELDS channel for bytes[0:1]
        self.dest1 = dest1                       # FIELDS channel for bytes[2:3]
        self.terminate = terminate
        self.note = note

    def cam(self):
        """Return (value, care) for the 64-bit key."""
        value = (self.state | (self.tag_depth << 8)) << 32
        care = 0x0000FFFF << 32                  # pin STATE8[0] and STATE8[1]
        if self.match_halfword0 is not None:
            value |= self.match_halfword0 & 0xFFFF
            care |= 0xFFFF
        return value, care

    def action(self):
        f = {}
        # StateOp1 := literal, for both the state byte and the tag-depth byte.
        f["StateOp0"], f["StateValue0"] = 1, self.next_state
        f["StateOp1"], f["StateValue1"] = 1, self.next_tag_depth
        if self.dest0 is not None:
            f["Halfword0Dest"] = self.dest0
            f["Byte0Enable"] = f["Byte1Enable"] = 1
        if self.dest1 is not None:
            f["Halfword1Dest"] = self.dest1
            f["Byte2Enable"] = f["Byte3Enable"] = 1
        if self.terminate:
            f["Terminate"] = 1
        return f


def build_program():
    """The protocol description. Everything below is a choice we are making."""
    rules = []

    # --- Ethernet header, identical regardless of tags (it precedes them) ---
    rules.append(Rule(S_START, S_DMAC_LO, dest0=CH_DMAC_HI, dest1=CH_DMAC_MID,
                      note="bytes 0-3: DMAC[47:32], DMAC[31:16]"))
    rules.append(Rule(S_DMAC_LO, S_SMAC_LO, dest0=CH_DMAC_LO, dest1=CH_SMAC_HI,
                      note="bytes 4-7: DMAC[15:0], SMAC[47:32]"))
    rules.append(Rule(S_SMAC_LO, S_ETYPE, dest0=CH_SMAC_MID, dest1=CH_SMAC_LO,
                      note="bytes 8-11: SMAC[31:16], SMAC[15:0]"))

    # --- EtherType dispatch, once per reachable tag depth ---
    for depth in range(MAX_TAGS + 1):
        here = S_ETYPE if depth == 0 else S_TAGGED
        # A VLAN tag: capture the EtherType, and the TCI lands in L2_VID1,
        # whose top nibble is the priority -- exactly the TCI layout.
        if depth < MAX_TAGS:
            for et in (ET_VLAN_C, ET_VLAN_S):
                rules.append(Rule(here, S_TAGGED, tag_depth=depth,
                                  next_tag_depth=depth + 1,
                                  match_halfword0=et,
                                  dest0=CH_ETHERTYPE, dest1=CH_VID1,
                                  note=f"VLAN 0x{et:04x} at depth {depth}"))
        for et, nxt in ((ET_IPV4, S_DONE_IPV4), (ET_IPV6, S_DONE_IPV6),
                        (ET_ARP, S_DONE_ARP)):
            rules.append(Rule(here, nxt, tag_depth=depth, match_halfword0=et,
                              dest0=CH_ETHERTYPE,
                              note=f"0x{et:04x} at depth {depth}"))
        # Anything else: record the EtherType and stop.
        rules.append(Rule(here, S_DONE_OTHER, tag_depth=depth,
                          dest0=CH_ETHERTYPE, terminate=True,
                          note=f"unknown EtherType at depth {depth}"))
    return rules


def reachable_slices(rule):
    """Every slice at which this rule's state can occur.

    A position sits at byte offset 4*n + 4*tag_depth, so the slice is fixed by
    the state AND the tag depth. This is the step that makes tagged frames work.
    """
    base = {S_START: 0, S_DMAC_LO: 1, S_SMAC_LO: 2, S_ETYPE: 3, S_TAGGED: 3}
    if rule.state not in base:
        return []
    return [base[rule.state] + rule.tag_depth]


def place(rules):
    """Assign rules to (slice, entry). Returns {slice: [(entry, rule)]}."""
    out = {}
    for r in rules:
        for s in reachable_slices(r):
            out.setdefault(s, []).append(r)
    placed = {}
    for s, rs in sorted(out.items()):
        placed[s] = list(enumerate(rs))
    return placed


def writes(placed):
    """Emit (address, value) pairs for CAM and RAM."""
    out = []
    for s, entries in sorted(placed.items()):
        for entry, rule in entries:
            value, care = rule.cam()
            key, keyinvert = encode_cam(value, care)
            cam_base = PARSER_BASE + SLICE_STRIDE * s + WORDS_PER_ENTRY * entry
            for i, w in enumerate([keyinvert & 0xFFFFFFFF, (keyinvert >> 32) & 0xFFFFFFFF,
                                   key & 0xFFFFFFFF, (key >> 32) & 0xFFFFFFFF]):
                out.append((cam_base + i, w))
            ram_base = cam_base + RAM_OFFSET
            for i, w in enumerate(encode_action(rule.action())):
                out.append((ram_base + i, w))
    return out


def check(placed):
    problems = []
    for s, entries in placed.items():
        if len(entries) > ENTRIES_PER_SLICE:
            problems.append(f"slice {s}: {len(entries)} entries exceeds {ENTRIES_PER_SLICE}")
        if s >= NUM_SLICES:
            problems.append(f"slice {s} is beyond the {NUM_SLICES}-slice array")
    # every non-terminal next_state must be matched by some rule
    produced = {r.next_state for _, rs in placed.items() for _, r in rs if not r.terminate}
    consumed = {r.state for _, rs in placed.items() for _, r in rs}
    terminal = {S_DONE_IPV4, S_DONE_IPV6, S_DONE_ARP, S_DONE_OTHER}
    for st in produced - consumed - terminal:
        problems.append(f"state 0x{st:02x} ({STATE_NAME.get(st,'?')}) is produced but never consumed")
    # channels must exist in Table 5-5
    for _, rs in placed.items():
        for _, r in rs:
            for ch in (r.dest0, r.dest1):
                if ch is not None and ch not in FIELDS_CHANNEL:
                    problems.append(f"channel {ch} is not in Table 5-5")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emit", action="store_true", help="print <addr> <value> writes")
    ap.add_argument("--check", action="store_true", help="structural validation")
    ap.add_argument("--summary", action="store_true", help="what the program does")
    args = ap.parse_args()

    rules = build_program()
    placed = place(rules)

    if args.summary:
        print(f"rules authored: {len(rules)}")
        print(f"slices used:    {sorted(placed)}")
        total = sum(len(v) for v in placed.values())
        print(f"entries placed: {total}  (CAM+RAM writes: {total * 8})\n")
        for s, entries in sorted(placed.items()):
            print(f"  slice {s} ({len(entries)} entries)")
            for entry, r in entries:
                dest = []
                if r.dest0 is not None:
                    dest.append(f"hw0->{FIELDS_CHANNEL.get(r.dest0)}")
                if r.dest1 is not None:
                    dest.append(f"hw1->{FIELDS_CHANNEL.get(r.dest1)}")
                print(f"    [{entry:>2}] {STATE_NAME.get(r.state,'?'):<24}"
                      f" -> {STATE_NAME.get(r.next_state,'?'):<24} {r.note}")
                if dest:
                    print(f"         {', '.join(dest)}")
        print("\nNOT COVERED: L3 field extraction (IPv4/IPv6 addresses, TTL, protocol,")
        print("length) and L4 ports. Table 5-5 lists identical channels for L3_SIP and")
        print("L3_DIP, which cannot both be right; guessing would produce a parser that")
        print("looks finished and forwards wrong.")

    if args.check:
        problems = check(placed)
        for p in problems:
            print("  PROBLEM:", p)
        print("check " + ("PASS" if not problems else "FAIL"))
        return 1 if problems else 0

    if args.emit:
        for addr, val in writes(placed):
            print(f"{addr:08x} {val:08x}")

    if not (args.emit or args.check or args.summary):
        ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
