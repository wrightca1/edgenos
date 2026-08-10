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
            if any(a is not None for a in act):
                print("             action=" + " ".join(
                    f"0x{a:08x}" if a is not None else "----" for a in act))


if __name__ == "__main__":
    main()
