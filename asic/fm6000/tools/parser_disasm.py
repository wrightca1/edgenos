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

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", help="ucode_l2.raw (or any ADDR VALUE list)")
    ap.add_argument("--slice", type=int, default=None, help="disassemble one slice")
    ap.add_argument("--max", type=int, default=12, help="entries to print per slice")
    ap.add_argument("--summary", action="store_true", help="occupancy only")
    a = ap.parse_args()

    w = read(a.file)
    occ = collections.OrderedDict()
    for s in range(NSLICE):
        n = 0
        for e in range(NENTRY):
            _, c = assemble(w, CAM_BASE, s, e)
            if c: n += 1
        occ[s] = n

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
