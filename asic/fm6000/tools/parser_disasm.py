#!/usr/bin/env python3
# parser_disasm.py - disassemble the FM6000 packet parser program.
#
# What we call "microcode" in ucode_l2.raw is not opaque: it is the same
# "ADDR VALUE" register-write text as the replay, and 100% of its writes are
# write-once, so it is pure table content with no embedded sequence.
#
# The parser is a 28-slice x 128-entry TCAM state machine. Each slice occupies
# 0x400 words: 128 CAM entries of 4 words at +0x000, and the 128 RAM entries
# that answer them, also 4 words each, at +0x200.
#
#     PARSER_CAM  128 bits   Key[64] @64, KeyInvert[64] @0     (ternary match)
#     PARSER_RAM  110 bits   see FIELDS below                  (the action)
#
# Field offsets come from the SDK's own field-name table via sdk_fieldmap.py.
# Nothing about the layout is guessed here.
#
# SPDX-License-Identifier: GPL-2.0-or-later
import sys, argparse, collections

CAM_BASE, RAM_BASE, SLICE_STRIDE, ENTRY_PITCH = 0x100000, 0x100200, 0x400, 4
NSLICE, NENTRY = 28, 128

# name -> (bit offset, width), exactly as the SDK reports them
FIELDS = {
    "SetFlags": (0, 38),
    "Halfword0Dest": (38, 6), "Halfword1Dest": (44, 6),
    "Halfword0Rot": (50, 2), "Halfword1Rot": (52, 2),
    "Byte0Enable": (54, 1), "Byte1Enable": (55, 1),
    "Byte2Enable": (56, 1), "Byte3Enable": (57, 1),
    "Halfword0Add": (58, 1), "Halfword1Add": (59, 1),
    "StateOp0": (60, 2), "StateValue0": (62, 8),
    "StateOp1": (70, 2), "StateValue1": (72, 8),
    "StateOp2": (80, 2), "StateValue2": (82, 8),
    "StateOp3": (90, 2), "StateValue3": (92, 8),
    "StateFrameRot": (100, 2), "LegalPadding": (102, 2),
    "TerminateAllowed": (104, 1), "Terminate": (105, 1),
    "ShiftNextSlice": (106, 3),
}
# StateOp is 2 bits. The encoding is not named in the SDK's field table, so it
# is printed numerically rather than guessed at.
STATEOP = {0: "op0", 1: "op1", 2: "op2", 3: "op3"}

def fld(v, name):
    off, w = FIELDS[name]
    return (v >> off) & ((1 << w) - 1)

def read(path):
    d = {}
    for line in open(path):
        f = line.split()
        if len(f) == 2:
            try: d[int(f[0], 16)] = int(f[1], 16)
            except ValueError: pass
    return d

def assemble(words, base, slice_, entry):
    """Join ENTRY_PITCH consecutive words into one wide value. Returns
    (value, nwords_present) so partially-written entries are visible."""
    a0 = base + slice_ * SLICE_STRIDE + entry * ENTRY_PITCH
    v, n = 0, 0
    for i in range(ENTRY_PITCH):
        w = words.get(a0 + i)
        if w is not None:
            v |= w << (32 * i); n += 1
    return v, n

# EtherTypes whose 16-bit field is fully cared-for. Only 16-bit matches are used:
# an 8-bit scan reports any cared byte equal to 6 as "TCP" and is not evidence.
ETH = {0x0800: "IPv4", 0x0806: "ARP", 0x8100: "C-VLAN", 0x88a8: "S-VLAN",
       0x86dd: "IPv6", 0x8847: "MPLS", 0x8906: "FCoE", 0x88f7: "PTP",
       0x6558: "GRE-TEB", 0x8808: "PAUSE"}

def regs_report(w):
    """Profile the parser's extracted-field register file.

    Halfword0Dest/Halfword1Dest are 6-bit destination register numbers. That
    register file is the parser's output and the input to every lookup stage,
    so knowing which register holds which protocol field is what lets an L2AR
    or FFU ternary key be read as protocol instead of as bit patterns.

    There is no field table for it in the SDK, so this reports only what the
    program itself shows: how many rules write each register, at which parse
    depths, and what EtherType the writing rules were matching."""
    info = collections.defaultdict(
        lambda: {"slices": set(), "eth": collections.Counter(), "n": 0})
    for s in range(NSLICE):
        for e in range(NENTRY):
            cam, c = assemble(w, CAM_BASE, s, e)
            if not c: continue
            ram, rn = assemble(w, RAM_BASE, s, e)
            if not rn: continue
            key = (cam >> 64) & ((1 << 64) - 1)
            care = key ^ (cam & ((1 << 64) - 1))
            tags = set()
            for sh in range(0, 64, 8):
                if ((care >> sh) & 0xffff) == 0xffff and ((key >> sh) & 0xffff) in ETH:
                    tags.add(ETH[(key >> sh) & 0xffff])
            for hw in (0, 1):
                d = fld(ram, "Halfword%dDest" % hw)
                if not d: continue
                i = info[d]; i["slices"].add(s); i["n"] += 1
                for t in tags: i["eth"][t] += 1
    print("extracted-field register file: %d registers in use, r%d..r%d\n"
          % (len(info), min(info), max(info)))
    print("%-5s %-6s %-8s %s" % ("reg", "rules", "slices", "EtherType context"))
    for d in sorted(info):
        i = info[d]; sl = sorted(i["slices"])
        ctx = ", ".join("%s x%d" % (k, v) for k, v in i["eth"].most_common(3)) or "-"
        print("r%-4d %-6d %-8s %s" % (d, i["n"], "%d-%d" % (sl[0], sl[-1]), ctx))
    pairs = [(d, d + 1) for d in sorted(info)
             if d + 1 in info and info[d]["n"] == info[d + 1]["n"]]
    print("\nadjacent registers written by an equal number of rules (a 32-bit field "
          "is two halfwords):")
    print("   " + "  ".join("r%d/r%d" % p for p in pairs))


# The window model, derived from the program and checked against it.
#
# The 64-bit CAM window is BIG-ENDIAN -- window byte 0 is the most significant
# byte -- and at slice s it covers packet bytes [4s-4 .. 4s+3]. So the window
# advances 4 bytes per slice and the four NEWLY arrived bytes are [4s .. 4s+3];
# hw0 takes [4s, 4s+1] and hw1 takes [4s+2, 4s+3].
#
# This is not assumed. It was fixed by four rules that must hold simultaneously:
#
#   slice 0 rule 6   care ..ff ff ff ff ff  key ..00 01 80 c2 00
#                    -> DMAC 01:80:c2 at packet bytes 0,1,2
#   slice 3 rule 6   window bytes 4,5 = 08 00   -> EtherType at packet bytes 12,13
#   slice 3 rule 9   window bytes 4,5 = 81 00   -> same position, C-VLAN
#   slice 3 rule 5   window bytes 4,5 = 86 dd   -> same position, IPv6
#
# and corroborated by those same slice-3 rules caring for the HIGH NIBBLE of
# window byte 6 with values 4 and 6 -- the IP version, at packet byte 14, which
# is exactly where the L3 header begins.
BASE_OFFSET = -4        # packet byte covered by window byte 0 at slice 0
NEW_BYTES = 4           # bytes the window advances per slice

def offsets_report(w):
    """Map each extracted-field register to the packet byte offset it receives."""
    off = collections.defaultdict(collections.Counter)
    for s in range(NSLICE):
        for e in range(NENTRY):
            cam, c = assemble(w, CAM_BASE, s, e)
            if not c: continue
            ram, rn = assemble(w, RAM_BASE, s, e)
            if not rn: continue
            for hw in (0, 1):
                d = fld(ram, "Halfword%dDest" % hw)
                if d: off[d][NEW_BYTES * s + 2 * hw] += 1
    ETH_HDR = {0: "DMAC[0:1]", 2: "DMAC[2:3]", 4: "DMAC[4:5]",
               6: "SMAC[0:1]", 8: "SMAC[2:3]", 10: "SMAC[4:5]", 12: "EtherType"}
    print("register -> packet byte offset (most common across the program)\n")
    print("%-6s %-8s %-14s %s" % ("reg", "offset", "rules", "field"))
    for d in sorted(off):
        o, n = off[d].most_common(1)[0]
        tot = sum(off[d].values())
        print("r%-5d %-8d %-14s %s" % (d, o, "%d/%d" % (n, tot), ETH_HDR.get(o, "")))
    print("\nThe 14-byte Ethernet header resolves completely and in the right order:")
    for o in sorted(ETH_HDR):
        who = [d for d in off if off[d].most_common(1)[0][0] == o]
        print("   byte %-3d %-12s %s" % (o, ETH_HDR[o], ", ".join("r%d" % d for d in who)))
    print("\n⚠ Registers written past the Ethernet header are NOT resolved by this")
    print("   report. Their offset depends on how deeply the frame is encapsulated,")
    print("   and a single 'most common' value across every path conflates VLAN,")
    print("   QinQ, MPLS and tunnel cases. Naming those needs a path-aware walk of")
    print("   the state machine, which this does not do.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", help="ucode_l2.raw (or any ADDR VALUE list)")
    ap.add_argument("--slice", type=int, default=None, help="disassemble one slice")
    ap.add_argument("--max", type=int, default=12, help="entries to print per slice")
    ap.add_argument("--summary", action="store_true", help="occupancy only")
    ap.add_argument("--regs", action="store_true",
                    help="profile the extracted-field register file instead")
    ap.add_argument("--offsets", action="store_true",
                    help="map extracted-field registers to packet byte offsets")
    a = ap.parse_args()

    w = read(a.file)
    occ = collections.OrderedDict()
    for s in range(NSLICE):
        n = 0
        for e in range(NENTRY):
            _, c = assemble(w, CAM_BASE, s, e)
            if c: n += 1
        occ[s] = n

    if a.regs:
        return regs_report(w)
    if a.offsets:
        return offsets_report(w)
    used = [s for s, n in occ.items() if n]
    print("parser program: %d of %d slices used, %d CAM entries populated"
          % (len(used), NSLICE, sum(occ.values())))
    print("occupancy by slice: %s" % ", ".join("%d:%d" % (s, occ[s]) for s in used))
    if a.summary:
        return

    for s in (used if a.slice is None else [a.slice]):
        print("\n=== slice %d (%d entries) ===" % (s, occ[s]))
        shown = 0
        for e in range(NENTRY):
            cam, cn = assemble(w, CAM_BASE, s, e)
            if not cn: continue
            ram, _ = assemble(w, RAM_BASE, s, e)
            key = (cam >> 64) & ((1 << 64) - 1)
            inv = cam & ((1 << 64) - 1)
            # care mask: a bit is matched where Key and KeyInvert disagree
            # Ternary semantics: a bit is DON'T CARE when Key and KeyInvert are
            # both set (which is why an all-ones entry is the universal default
            # rule, not an empty slot), and NEVER-MATCH when both are clear.
            # So the cared-for bits are exactly where the two disagree, and the
            # value being matched is Key restricted to those bits.
            care = key ^ inv
            acts = []
            for hw in (0, 1):
                if fld(ram, "Halfword%dDest" % hw):
                    acts.append("hw%d->r%d%s%s" % (
                        hw, fld(ram, "Halfword%dDest" % hw),
                        (" rot%d" % fld(ram, "Halfword%dRot" % hw)) if fld(ram, "Halfword%dRot" % hw) else "",
                        " add" if fld(ram, "Halfword%dAdd" % hw) else ""))
            be = "".join(str(b) for b in range(4) if fld(ram, "Byte%dEnable" % b))
            if be: acts.append("bytes=%s" % be)
            for i in range(4):
                op, val = fld(ram, "StateOp%d" % i), fld(ram, "StateValue%d" % i)
                if op or val: acts.append("st%d=%s(%#04x)" % (i, STATEOP[op], val))
            if fld(ram, "SetFlags"): acts.append("flags=%#x" % fld(ram, "SetFlags"))
            if fld(ram, "ShiftNextSlice"): acts.append("shift=%d" % fld(ram, "ShiftNextSlice"))
            if fld(ram, "Terminate"): acts.append("TERMINATE")
            elif fld(ram, "TerminateAllowed"): acts.append("term-ok")
            print("  [%3d] key=%016x care=%016x | %s" % (e, key, care, "  ".join(acts) or "-"))
            shown += 1
            if shown >= a.max:
                print("  ... %d more" % (occ[s] - shown)); break

if __name__ == "__main__":
    main()
