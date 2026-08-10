#!/usr/bin/env python3
"""gen_parser.py - emit FM6000 parser CAM+RAM entries from rules we author.

This is the encoder half of replacing asic/fm6000/fm6000_parserinit.c, which
today carries Intel's parser tables verbatim (docs/PROVENANCE.md 2.5).
parser_decode.py reads their program; this one writes ours.

    encode_cam(value, care, never) -> (Key, KeyInvert)
    encode_action({...})      -> 4 action words

CAM canonical encoding, from the decode:

    must be 1     Key=1 KeyInvert=0        Key       = value | ~care
    must be 0     Key=0 KeyInvert=1        KeyInvert = ~value | ~care
    don't care    Key=1 KeyInvert=1

    never match   Key=0 KeyInvert=0        permanently disables the entry

The fourth state is not theoretical -- EOS disables 3 of its 2,117 entries with
it (slice 1 entry 4, slice 3 entries 26 and 37). An encoder that folds it into
don't-care silently revives a disabled rule. It was found by --verify failing,
which is the whole reason that mode exists.

★ THE POINT OF --verify. An encoder is only trustworthy if it reproduces a
program we did not write. --verify decodes every populated entry of an EOS
image, re-encodes it from the decoded fields, and requires the result to be
bit-identical. 2,117 CAM entries and 2,117 actions, no mismatches, is what
makes it safe to author our own rules with this.

⚠ WHAT THIS DOES NOT YET DO. Encoding is not authorship. A working parser must
also agree with the rest of the pipeline about *conventions* -- which FIELDS
channel carries which extracted header, what the 32-bit inter-slice state means,
which slice a protocol is expected to land in. Those are choices EOS's program
embodies, and L2AR/L3AR/FFU downstream are built around them. Emitting valid
words is mechanical; emitting a program the rest of the chip agrees with is not.
Deriving those conventions from the decode is the next job.

PROVENANCE. Reads an image at runtime only for --verify, and embeds nothing.
Field names, widths and semantics are datasheet facts (Table 5-3). No table from
EOS enters this repository.

Usage:
    gen_parser.py --verify --image <fm6000Microcode.raw>
    gen_parser.py --self-test
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import (  # noqa: E402
    ACTION_FIELDS, ACTION_OFFSET, ACTION_WIDTH, PARSER_BASE, SLICE_STRIDE,
    RAM_OFFSET, WORDS_PER_ENTRY, ENTRIES_PER_SLICE, NUM_SLICES,
    load, decode_slice, action_value, action_field, ternary,
)

MASK64 = 0xFFFFFFFFFFFFFFFF


def encode_cam(value, care, never=0):
    """Inverse of parser_decode.ternary(). Returns (Key, KeyInvert).

    `never` marks bits encoded Key=0/Inv=0, which can never match and so
    permanently disable the entry. EOS uses it on 3 of its 2,117 entries; an
    encoder that cannot express it silently revives a disabled rule.
    """
    if value & ~care:
        raise ValueError("value has bits set outside care mask")
    if never & care:
        raise ValueError("never-match bits overlap the care mask")
    key = (value | ~care) & MASK64 & ~never
    keyinvert = (~value | ~care) & MASK64 & ~never
    return key, keyinvert


def encode_action(fields):
    """Pack a {field: value} dict into 4 action words."""
    acc = 0
    for name, width in ACTION_FIELDS:
        v = fields.get(name, 0)
        if v >> width:
            raise ValueError(f"{name}=0x{v:x} exceeds {width} bits")
        off, _ = ACTION_OFFSET[name]
        acc |= v << off
    return [(acc >> (32 * i)) & 0xFFFFFFFF for i in range(4)]


def decode_action_fields(value):
    return {n: action_field(value, n) for n, _ in ACTION_FIELDS}


def cam_words(key, keyinvert):
    return [keyinvert & 0xFFFFFFFF, (keyinvert >> 32) & 0xFFFFFFFF,
            key & 0xFFFFFFFF, (key >> 32) & 0xFFFFFFFF]


def verify(image):
    """Round-trip every populated entry of a real image through the encoder."""
    mem = load(image)
    cam_ok = cam_bad = act_ok = act_bad = 0
    failures = []

    for s in range(NUM_SLICES):
        for entry, key, keyinvert, value, care, never, act in decode_slice(mem, s):
            rk, ri = encode_cam(value, care, never)
            if (rk, ri) == (key, keyinvert):
                cam_ok += 1
            else:
                cam_bad += 1
                if len(failures) < 5:
                    failures.append(
                        f"  CAM s{s} e{entry}: got key=0x{rk:016x} inv=0x{ri:016x}, "
                        f"want key=0x{key:016x} inv=0x{keyinvert:016x}")

            av = action_value(act)
            if av is None:
                continue
            re_words = encode_action(decode_action_fields(av))
            if re_words == [a & 0xFFFFFFFF for a in act]:
                act_ok += 1
            else:
                act_bad += 1
                if len(failures) < 10:
                    failures.append(
                        f"  ACT s{s} e{entry}: got {[hex(w) for w in re_words]}, "
                        f"want {[hex(a) for a in act]}")

    print(f"CAM entries round-tripped: {cam_ok} ok, {cam_bad} mismatched")
    print(f"actions  round-tripped:    {act_ok} ok, {act_bad} mismatched")
    for f in failures:
        print(f)

    # An action packs 109 of 128 bits. Bits outside the layout would be silently
    # dropped by the re-encode, so check none are set anywhere in the image.
    stray = 0
    for s in range(NUM_SLICES):
        for entry, *_, act in decode_slice(mem, s):
            av = action_value(act)
            if av is not None and av >> ACTION_WIDTH:
                stray += 1
    print(f"entries with bits set above the {ACTION_WIDTH}-bit layout: {stray}")

    good = cam_bad == 0 and act_bad == 0 and stray == 0
    print("\nVERIFY " + ("PASS - the encoder reproduces a program it did not write"
                         if good else "FAIL"))
    return 0 if good else 1


def self_test():
    """Encoder identities that need no image."""
    fails = []

    k, i = encode_cam(0x0800, 0xFFFF)
    v, c, n = ternary(k, i)
    if (v, c, n) != (0x0800, 0xFFFF, 0):
        fails.append(f"exact match round-trip: {v:#x}/{c:#x}/{n:#x}")

    k, i = encode_cam(0, 0)
    if (k, i) != (MASK64, MASK64):
        fails.append("full don't-care must encode as all-ones/all-ones")

    # a never-match bit must survive the round trip (outside the care mask)
    k, i = encode_cam(0x0800, 0xFFFF, never=1 << 16)
    v, c, n = ternary(k, i)
    if n != 1 << 16:
        fails.append(f"never-match bit lost: got {n:#x}")

    try:
        encode_cam(0x1, 0x1, never=0x1)
        fails.append("never/care overlap not rejected")
    except ValueError:
        pass

    w = encode_action({"Terminate": 1})
    if w[3] >> (108 - 96) & 1 != 1:
        fails.append("Terminate must land at bit 108")

    w = encode_action({"Halfword0Dest": 0x3F})
    off = ACTION_OFFSET["Halfword0Dest"][0]
    acc = w[0] | (w[1] << 32) | (w[2] << 64) | (w[3] << 96)
    if (acc >> off) & 0x3F != 0x3F:
        fails.append("Halfword0Dest misplaced")

    try:
        encode_action({"LegalPadding": 4})
        fails.append("over-wide field not rejected")
    except ValueError:
        pass

    try:
        encode_cam(0xFF, 0x0F)
        fails.append("value outside care mask not rejected")
    except ValueError:
        pass

    for f in fails:
        print("  FAIL:", f)
    print("self-test " + ("PASS" if not fails else "FAIL"))
    return 0 if not fails else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verify", action="store_true",
                    help="round-trip every entry of --image through the encoder")
    ap.add_argument("--image", help="fm6000Microcode.raw (operator-supplied)")
    ap.add_argument("--self-test", action="store_true", help="encoder identities, no image")
    args = ap.parse_args()

    rc = 0
    if args.self_test:
        rc |= self_test()
    if args.verify:
        if not args.image:
            sys.exit("--verify needs --image")
        rc |= verify(args.image)
    if not (args.self_test or args.verify):
        ap.print_help()
    return rc


if __name__ == "__main__":
    sys.exit(main())
