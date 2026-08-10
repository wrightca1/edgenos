#!/usr/bin/env python3
"""parser_decode.py - decode the FM6000 parser TCAM into readable match rules.

The parser is not an opaque instruction stream. Datasheet 5.5.1: it is an
iterative state machine unrolled into 28 slices. Each slice takes the next
4-byte frame word plus the previous slice's 32-bit state, forms a 64-bit key,
looks it up in a TCAM, and applies the action from a parallel SRAM.

    PARSER_CAM(slice, entry, word) = 0x100000 + 0x400*slice + 4*entry
    PARSER_RAM(slice, entry, word) = same + 0x200

Each CAM entry is 4 words = 128 bits, confirmed against a live read:

    words[1:0] = KeyInvert[63:0]
    words[3:2] = Key[63:0]

and the ternary match those two encode is

    bit must be 1     Key & ~KeyInvert
    bit must be 0    ~Key &  KeyInvert
    don't care        Key &  KeyInvert
    never matches    ~Key & ~KeyInvert

Cross-check that fixes the convention: slice 4 entry 3 is
Key=0xff00ff3a00010800 KeyInvert=0xffffffc5fffef7ff, whose low 16 bits reduce
to an exact match on 0x0800 -- IPv4 -- which is what that slice should be
testing. Recovering 2,117 populated entries also reproduces the count reached
independently by earlier hand analysis.

WHY THIS EXISTS. asic/fm6000/fm6000_parserinit.c currently carries the parser
microcode verbatim (see docs/PROVENANCE.md 2.5). Decoding the tables into
match rules is the first half of replacing that file with a generator that
emits our own parser program from a declarative protocol list; this tool is the
decoder, not the generator.

PROVENANCE. Reads the microcode image at runtime and never embeds it. The image
is Arista/Intel material and does not belong in this repository -- supply it
with --image. Nothing this tool prints is copied into the tree; it exists to
let us understand the format well enough to write our own.

Usage:
    parser_decode.py --image <fm6000Microcode.raw> --summary
    parser_decode.py --image <img> --slice 4
    parser_decode.py --image <img> --ethertypes
"""
import argparse
import collections
import sys

PARSER_BASE = 0x100000
SLICE_STRIDE = 0x400
RAM_OFFSET = 0x200
ENTRIES_PER_SLICE = 128
WORDS_PER_ENTRY = 4
NUM_SLICES = 28

# Action SRAM layout, datasheet Table 5-3, in table order packed LSB-first.
# Sums to 109 bits and the entry is 128, so 19 bits are spare.
#
# The order is the datasheet's; that it is packed LSB-first is OUR finding, and
# it is corroborated rather than assumed -- decoding all 2,117 entries under it
# puts every field inside its documented range:
#
#   Halfword0Dest  {0,1,2,3,4,5,8,60,62}   valid 6-bit FIELDS channel indices
#   Halfword1Dest  {0,1,15}
#   ShiftNextSlice {0,4}                   documented 0..7
#   LegalPadding   {0,1}                   documented 0..3
#   StateFrameRot  {0,1,2,3}               exactly the full 2-bit range
#   Byte0-3Enable  mostly 0000, then 0111 / 1111
#
# and no bit above 107 is ever set anywhere in the image.
#
# ⚠ Terminate (bit 108) is never set in EOS's program. That is not evidence the
# layout is wrong -- it was briefly read that way here and the reading was
# mistaken. The parser has 28 slices and can end by exhausting them, so a
# forced terminate is simply unused. TerminateAllowed (bit 107) IS used, and
# with a very regular shape: exactly 5 entries per slice for slices 3-14, then
# exactly 2 for slices 15-20, none elsewhere.
ACTION_FIELDS = [
    ("StateOp0", 2), ("StateOp1", 2), ("StateOp2", 2), ("StateOp3", 2),
    ("StateValue0", 8), ("StateValue1", 8), ("StateValue2", 8), ("StateValue3", 8),
    ("StateFrameRot", 2), ("SetFlags", 38),
    ("Halfword0Dest", 6), ("Halfword1Dest", 6),
    ("Halfword0Rot", 2), ("Halfword1Rot", 2),
    ("Byte0Enable", 1), ("Byte1Enable", 1), ("Byte2Enable", 1), ("Byte3Enable", 1),
    ("Halfword0Add", 1), ("Halfword1Add", 1),
    ("ShiftNextSlice", 3), ("LegalPadding", 2),
    ("TerminateAllowed", 1), ("Terminate", 1),
]

ACTION_OFFSET = {}
_p = 0
for _n, _w in ACTION_FIELDS:
    ACTION_OFFSET[_n] = (_p, _w)
    _p += _w
ACTION_WIDTH = _p

# StateOpN encoding, Table 5-3. M = (N + StateFrameRot) % 4.
STATE_OP = {
    0: "STATE8 += ValueN",
    1: "STATE8 := ValueN",
    2: "STATE8 := FRAME_DATA[M] + ValueN",
    3: "STATE8 := FRAME_DATA[M]*2 + ValueN",
}

# Fixed-function HEADER.FLAGS bits, Table 5-4.
HEADER_FLAG = {37: "ChecksumError", 38: "IncompleteHeader", 39: "ParityError"}

# EtherTypes worth naming when they turn up in a key. Ours, for reporting only.
ETHERTYPE = {
    0x0800: "IPv4", 0x0806: "ARP", 0x86dd: "IPv6", 0x8100: "VLAN C-tag",
    0x88a8: "VLAN S-tag", 0x9100: "VLAN QinQ", 0x8906: "FCoE", 0x88f7: "PTP",
    0x8847: "MPLS unicast", 0x8848: "MPLS multicast", 0x8809: "LACP/slow",
    0x88cc: "LLDP", 0x8914: "FCoE init",
}


def load(path):
    """Read an <addr> <value> hex image into {addr: value}."""
    mem = {}
    with open(path, errors="ignore") as fh:
        for line in fh:
            parts = line.split()
            if len(parts) != 2:
                continue
            try:
                mem[int(parts[0], 16)] = int(parts[1], 16)
            except ValueError:
                continue
    if not mem:
        sys.exit(f"{path}: no <addr> <value> pairs found")
    return mem


def entry_words(mem, slice_, entry):
    base = PARSER_BASE + SLICE_STRIDE * slice_ + WORDS_PER_ENTRY * entry
    return [mem.get(base + w) for w in range(WORDS_PER_ENTRY)]


def action_words(mem, slice_, entry):
    base = PARSER_BASE + SLICE_STRIDE * slice_ + RAM_OFFSET + WORDS_PER_ENTRY * entry
    return [mem.get(base + w) for w in range(WORDS_PER_ENTRY)]


def ternary(key, keyinvert):
    """Return (value, care) -- match iff (input & care) == value."""
    must_one = key & ~keyinvert & 0xFFFFFFFFFFFFFFFF
    must_zero = ~key & keyinvert & 0xFFFFFFFFFFFFFFFF
    return must_one, (must_one | must_zero)


def populated(words):
    return any(w is not None and w not in (0, 0xFFFFFFFF) for w in words)


def decode_slice(mem, slice_):
    """Yield (entry, key, keyinvert, value, care, action) for populated entries."""
    for entry in range(ENTRIES_PER_SLICE):
        w = entry_words(mem, slice_, entry)
        if not populated(w):
            continue
        if any(x is None for x in w):
            continue
        keyinvert = (w[1] << 32) | w[0]
        key = (w[3] << 32) | w[2]
        value, care = ternary(key, keyinvert)
        yield entry, key, keyinvert, value, care, action_words(mem, slice_, entry)


def action_value(words):
    """Pack the 4 action words into one 128-bit integer, or None."""
    if any(w is None for w in words):
        return None
    return words[0] | (words[1] << 32) | (words[2] << 64) | (words[3] << 96)


def action_field(value, name):
    off, width = ACTION_OFFSET[name]
    return (value >> off) & ((1 << width) - 1)


def action_render(value):
    """Render an action as the non-default fields only."""
    if value is None:
        return "(unreadable)"
    out = []
    for n in range(4):
        op = action_field(value, f"StateOp{n}")
        val = action_field(value, f"StateValue{n}")
        if op or val:
            out.append(f"State{n}: {STATE_OP[op].replace('ValueN', f'0x{val:02x}').replace('N', str(n))}")
    rot = action_field(value, "StateFrameRot")
    if rot:
        out.append(f"StateFrameRot={rot}")
    flags = action_field(value, "SetFlags")
    if flags:
        named = [HEADER_FLAG[b] for b in HEADER_FLAG if (flags >> b) & 1]
        out.append(f"SetFlags=0x{flags:010x}" + (f" ({', '.join(named)})" if named else ""))
    for half in (0, 1):
        dest = action_field(value, f"Halfword{half}Dest")
        rot = action_field(value, f"Halfword{half}Rot")
        add = action_field(value, f"Halfword{half}Add")
        if dest or rot or add:
            bits = f"Halfword{half}->FIELDS[{dest}]"
            if rot:
                bits += f" rot{rot}"
            if add:
                bits += " +CHECKSUM"
            out.append(bits)
    en = "".join(str(action_field(value, f"Byte{i}Enable")) for i in range(4))
    if en != "0000":
        out.append(f"ByteEnable={en}")
    shift = action_field(value, "ShiftNextSlice")
    if shift:
        out.append(f"ShiftNextSlice={shift}")
    pad = action_field(value, "LegalPadding")
    if pad:
        out.append(f"LegalPadding={pad}")
    if action_field(value, "TerminateAllowed"):
        out.append("TerminateAllowed")
    if action_field(value, "Terminate"):
        out.append("TERMINATE")
    return "; ".join(out) if out else "(no-op)"


def describe(value, care):
    """Name any exactly-matched 16-bit EtherType-shaped field in the low word."""
    hits = []
    for shift in (0, 16, 32, 48):
        if (care >> shift) & 0xFFFF == 0xFFFF:
            v = (value >> shift) & 0xFFFF
            if v in ETHERTYPE:
                hits.append(f"{ETHERTYPE[v]}(0x{v:04x})@bit{shift}")
    return ", ".join(hits)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", required=True, help="fm6000Microcode.raw (operator-supplied)")
    ap.add_argument("--slice", type=int, help="dump one slice in full")
    ap.add_argument("--summary", action="store_true", help="per-slice entry counts")
    ap.add_argument("--ethertypes", action="store_true", help="protocol set recovered from keys")
    args = ap.parse_args()

    mem = load(args.image)

    if args.summary:
        total = 0
        print("slice  entries  care-bits(avg)  named protocols")
        for s in range(NUM_SLICES):
            rows = list(decode_slice(mem, s))
            if not rows:
                continue
            total += len(rows)
            avg = sum(bin(c).count("1") for *_, c, _ in rows) / len(rows)
            names = sorted({n for *_, v, c, _ in rows for n in [describe(v, c)] if n})
            print(f"  {s:>2}   {len(rows):>6}   {avg:>12.1f}  {', '.join(names)[:60]}")
        print(f"\ntotal populated entries: {total}")

    if args.ethertypes:
        found = collections.Counter()
        for s in range(NUM_SLICES):
            for _, _, _, value, care, _ in decode_slice(mem, s):
                for shift in (0, 16, 32, 48):
                    if (care >> shift) & 0xFFFF == 0xFFFF:
                        v = (value >> shift) & 0xFFFF
                        if v in ETHERTYPE:
                            found[(v, ETHERTYPE[v])] += 1
        print("protocol set matched by the parser TCAM:")
        for (v, name), n in sorted(found.items(), key=lambda t: -t[1]):
            print(f"  0x{v:04x}  {name:<16} {n:>4} entries")

    if args.slice is not None:
        print(f"=== slice {args.slice} ===")
        for entry, key, keyinvert, value, care, act in decode_slice(mem, args.slice):
            note = describe(value, care)
            print(f"  entry {entry:>3}  key=0x{key:016x} inv=0x{keyinvert:016x}")
            print(f"             value=0x{value:016x} care=0x{care:016x}"
                  f"  ({bin(care).count('1')} bits){'  <- ' + note if note else ''}")
            print(f"             action: {action_render(action_value(act))}")


if __name__ == "__main__":
    main()
