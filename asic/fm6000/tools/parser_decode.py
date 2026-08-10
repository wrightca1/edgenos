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

# Action SRAM field list and widths, datasheet Table 5-3. Sums to 109 bits.
#
# ############################################################################
# ⚠⚠ THE OFFSETS BELOW ARE WRONG. The field LIST and WIDTHS are datasheet
# facts; the assumption that they are packed LSB-first in table order is NOT,
# and it has been REFUTED. Do not trust action_render() output.
#
# Refuted by datasheet Table 5-5 (Parser Fixed Mapping), which fixes in
# hardware which FIELDS channel carries which header:
#
#     FIELDS[5,6,7]    = L2_DMAC[15:0], [31:16], [47:32]
#     FIELDS[12,13,14] = L2_SMAC
#     FIELDS[15]       = L2_TYPE
#
# Any real parser must write those channels. Under the LSB-first layout,
# Halfword0Dest sits at bit 80 and yields {0,1,2,3,4,5,8,60,62} -- never 6, 7,
# 12, 13 or 14, and 60/62 do not exist in a table that stops at 43.
#
# Scanning every offset for a 6-bit field that hits the documented channels
# gives exactly ONE candidate, bit 45, and it is unambiguous:
#
#     ch7  L2_DMAC[47:32]  written in slices 1,3,5,6,7...
#     ch6  L2_DMAC[31:16]  slices 2,4,7,8...
#     ch5  L2_DMAC[15:0]   slices 6,8,9,10...
#     ch14 L2_SMAC[47:32]  slices 7,8,9...
#     ch15 L2_TYPE         slices 10,11,12...
#
# That is a parser walking an Ethernet header in wire order, deeper with each
# slice. So Halfword0Dest is at bit 45 (see HALFWORD0_DEST_OFFSET), and the
# whole LSB-first hypothesis falls with it.
#
# ⚠ WHY gen_parser --verify DID NOT CATCH THIS, which is the lesson worth
# keeping: round-tripping decode->encode only proves the packing is SELF-
# CONSISTENT. Shifted field boundaries re-encode to the identical bits and pass
# perfectly. A round-trip validates an encoder, never an interpretation. Only an
# external fact -- here Table 5-5 -- can do that.
#
# Consequently the earlier "every field lands inside its documented range"
# corroboration was weak evidence and should not have been treated as
# confirmation; several fields are narrow enough that a wrong offset still
# yields plausible values.
#
# Halfword1Dest is NOT located. Bit 65 produces the right kind of pairing with
# bit 45 -- (7,6), (13,12), (11,10), i.e. the two halves of one frame word --
# but on only 2.2% of entries, which is not good enough to claim.
#
# TODO: re-derive the full layout against Table 5-3 field by field, anchored on
# bit 45, before any generated parser is emitted.
# ############################################################################
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

# The one action field located by external evidence rather than assumption:
# datasheet Table 5-5's hardware-fixed channel map. See the block comment above.
HALFWORD0_DEST_OFFSET = 45
HALFWORD0_DEST_WIDTH = 6

# Table 5-5 Parser Fixed Mapping -- which FIELDS channel carries which header.
# Datasheet facts, and the external check that refuted the packing hypothesis.
FIELDS_CHANNEL = {
    0: "ISL_FTYPE/VTYPE/PRI/USER", 1: "L2_VID1 (+L2_VPRI1)", 2: "L2_VID2 (+L2_VPRI2)",
    3: "ISL_SGLORT", 4: "ISL_DGLORT",
    5: "L2_DMAC[15:0]", 6: "L2_DMAC[31:16]", 7: "L2_DMAC[47:32]",
    12: "L2_SMAC[15:0]", 13: "L2_SMAC[31:16]", 14: "L2_SMAC[47:32]",
    15: "L2_TYPE (EtherType)", 16: "L3_FLOW[19:16]/L3_PRI", 17: "L3_FLOW[15:0]",
    18: "L3_LENGTH", 19: "L3_TTL/L3_PROT",
    20: "L3_SIP/DIP[15:0]", 21: "L3_SIP/DIP[31:16]",
    24: "L4_SRC", 25: "L4_DST",
    32: "L3_S/DIP[111:96]", 33: "L3_S/DIP[127:112]",
    36: "L3_S/DIP[47:32]", 37: "L3_S/DIP[63:48]",
    38: "L3_S/DIP[79:64]", 39: "L3_S/DIP[95:80]",
    8: "FIELD16A (LABEL8A/B)", 9: "FIELD16B (LABEL16)", 10: "FIELD16C", 11: "FIELD16D",
    26: "FIELD16A'", 27: "FIELD16B'", 40: "FIELD16G", 41: "FIELD16H", 42: "FIELD16I",
    43: "unused",
}


def halfword0_dest(action):
    """The one dest field we can actually trust. Returns a FIELDS channel index."""
    return (action >> HALFWORD0_DEST_OFFSET) & ((1 << HALFWORD0_DEST_WIDTH) - 1)

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
    """Return (value, care, never) -- match iff (input & care) == value.

    Four states per bit, not three. The fourth is real and EOS uses it:

        Key=1 Inv=0   must be 1
        Key=0 Inv=1   must be 0
        Key=1 Inv=1   don't care
        Key=0 Inv=0   NEVER MATCHES -- the entry can never fire

    'never' was found by round-tripping, not by reading: gen_parser --verify
    reproduced 2,114 of 2,117 entries and the 3 failures all had bit 0 clear in
    both words. Collapsing that into don't-care loses the distinction and
    re-encodes a permanently-disabled entry as a live one. Slice 1 entry 4 and
    slice 3 entries 26 and 37 are disabled this way.
    """
    mask = 0xFFFFFFFFFFFFFFFF
    must_one = key & ~keyinvert & mask
    must_zero = ~key & keyinvert & mask
    never = ~key & ~keyinvert & mask
    return must_one, (must_one | must_zero), never


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
        value, care, never = ternary(key, keyinvert)
        yield entry, key, keyinvert, value, care, never, action_words(mem, slice_, entry)


def action_value(words):
    """Pack the 4 action words into one 128-bit integer, or None."""
    if any(w is None for w in words):
        return None
    return words[0] | (words[1] << 32) | (words[2] << 64) | (words[3] << 96)


def action_field(value, name):
    off, width = ACTION_OFFSET[name]
    return (value >> off) & ((1 << width) - 1)


def action_render(value):
    """Render an action as the non-default fields only.

    ⚠ Everything except the Halfword0Dest line is derived from the REFUTED
    LSB-first offsets and is not to be trusted. See the block comment above.
    """
    if value is None:
        return "(unreadable)"
    ch = halfword0_dest(value)
    trusted = f"[VERIFIED] Halfword0->FIELDS[{ch}] {FIELDS_CHANNEL.get(ch, '(generic)')}" if ch else ""
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
    body = "; ".join(out) if out else "(no-op)"
    return (trusted + " | UNTRUSTED: " + body) if trusted else ("UNTRUSTED: " + body)


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
            avg = sum(bin(r[4]).count("1") for r in rows) / len(rows)
            names = sorted({n for r in rows for n in [describe(r[3], r[4])] if n})
            print(f"  {s:>2}   {len(rows):>6}   {avg:>12.1f}  {', '.join(names)[:60]}")
        print(f"\ntotal populated entries: {total}")

    if args.ethertypes:
        found = collections.Counter()
        for s in range(NUM_SLICES):
            for _, _, _, value, care, _, _ in decode_slice(mem, s):
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
        for entry, key, keyinvert, value, care, never, act in decode_slice(mem, args.slice):
            note = describe(value, care)
            if never:
                note = (note + "; " if note else "") + f"DISABLED (never-match 0x{never:016x})"
            print(f"  entry {entry:>3}  key=0x{key:016x} inv=0x{keyinvert:016x}")
            print(f"             value=0x{value:016x} care=0x{care:016x}"
                  f"  ({bin(care).count('1')} bits){'  <- ' + note if note else ''}")
            print(f"             action: {action_render(action_value(act))}")


if __name__ == "__main__":
    main()
