#!/usr/bin/env python3
"""mod_gen.py - encode FM6000 MOD tables, and prove the encoder on EOS's own.

The encoder half of replacing fm6000_modinit.c (3,626 transcribed microcode
pairs). mod_decode.py reads EOS's egress-modify program; this writes ours.

MOD is the smallest of the four blocks and the least entangled with the rest of
the pipeline, which is why it is the next one after the parser. Its three tables
are independent of the DMT-profile and CPU-code semantics that block L2AR.

    encode_cam(value, care, never) -> (Key, KeyInvert)   64-bit key, 48 used
    encode_command({...})          -> 1 word    Command, Jitter, Valid
    encode_value({...})            -> 2 words   Val{A..D} Constant/DataSelect/Type

★ --verify decodes every populated step of an EOS image, re-encodes it, and
demands bit-identical output across all three tables. Same check that exposed the
never-match encoding on the parser.

⚠ It proves SELF-CONSISTENCY, not interpretation. Two different wrong parser
action layouts both round-tripped perfectly. What settles MOD's interpretation is
FM6000_MOD_CAM_KEYS / _COMMAND_RAM_ / _VALUE_RAM_ in the register header.

PROVENANCE. Reads an image at runtime for --verify only; embeds nothing. Field
names, widths and positions are register-header facts.

Usage:
    mod_gen.py --verify --image <fm6000Microcode.raw>
    mod_gen.py --self-test
    mod_gen.py --keymap --image <img>
"""
import argparse
import collections
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import load, ternary  # noqa: E402
from mod_decode import (  # noqa: E402
    cam, command, value, fields, KEY_LAYOUT, CMD_LAYOUT, VAL_LAYOUT,
    PROFILES, STEPS,
)

MASK64 = 0xFFFFFFFFFFFFFFFF


def encode_cam(value_, care, never=0):
    if value_ & ~care:
        raise ValueError("value has bits set outside care mask")
    if never & care:
        raise ValueError("never-match bits overlap the care mask")
    key = (value_ | ~care) & MASK64 & ~never
    keyinvert = (~value_ | ~care) & MASK64 & ~never
    return key, keyinvert


def _pack(fields_, layout, nbits):
    acc = 0
    for name, lo, hi in layout:
        v = fields_.get(name, 0)
        width = hi - lo + 1
        if v >> width:
            raise ValueError(f"{name}=0x{v:x} exceeds {width} bits")
        acc |= v << lo
    if acc >> nbits:
        raise ValueError("packed value exceeds table width")
    return acc


def encode_command(fields_):
    return _pack(fields_, CMD_LAYOUT, 32) & 0xFFFFFFFF


def encode_value(fields_):
    acc = _pack(fields_, VAL_LAYOUT, 64)
    return [acc & 0xFFFFFFFF, (acc >> 32) & 0xFFFFFFFF]


def verify(image):
    mem = load(image)
    ok = bad = 0
    cok = cbad = vok = vbad = 0
    failures = []
    for p in range(PROFILES):
        for s in range(STEPS):
            c = cam(mem, p, s)
            if c:
                key, keyinvert, val, care, never = c
                rk, ri = encode_cam(val, care, never)
                if (rk, ri) == (key, keyinvert):
                    ok += 1
                else:
                    bad += 1
                    if len(failures) < 5:
                        failures.append(
                            f"  CAM p{p} s{s}: got 0x{rk:016x}/0x{ri:016x}, "
                            f"want 0x{key:016x}/0x{keyinvert:016x}")
            raw = command(mem, p, s)
            if raw is not None:
                if encode_command(fields(raw, CMD_LAYOUT)) == raw:
                    cok += 1
                else:
                    cbad += 1
                    if len(failures) < 10:
                        failures.append(f"  CMD p{p} s{s}: 0x{raw:08x}")
            v = value(mem, p, s, raw_bank=True)   # every bank, regardless of role
            if v is not None:
                want = [v & 0xFFFFFFFF, (v >> 32) & 0xFFFFFFFF]
                if encode_value(fields(v, VAL_LAYOUT)) == want:
                    vok += 1
                else:
                    vbad += 1
                    if len(failures) < 15:
                        failures.append(f"  VAL p{p} s{s}: 0x{v:016x}")
    print(f"CAM steps round-tripped:     {ok} ok, {bad} mismatched")
    print(f"COMMAND_RAM round-tripped:   {cok} ok, {cbad} mismatched")
    print(f"VALUE_RAM round-tripped:     {vok} ok, {vbad} mismatched")
    for f in failures:
        print(f)
    # A round-trip over nothing is not a pass. Pointed at an image that holds no
    # MOD tables -- ucode_l2.raw rather than ucode_tail.raw, say -- every counter
    # is zero, no comparison ever runs, and the old verdict reported PASS. This
    # project has been bitten by exactly that shape before ("0 of 65,792 changed"
    # from a silently broken join), so require evidence, not absence of failure.
    empty = [n for n, c in (("CAM", ok + bad), ("COMMAND_RAM", cok + cbad),
                            ("VALUE_RAM", vok + vbad)) if c == 0]
    if empty:
        print("\nVERIFY INCONCLUSIVE - no entries found in: %s" % ", ".join(empty))
        print("  Nothing was compared, so this is not a pass. Wrong image?")
        return 2
    good = bad == cbad == vbad == 0
    print("\nVERIFY " + ("PASS - the encoder reproduces a program it did not write"
                         if good else "FAIL"))
    return 0 if good else 1


def keymap(image):
    mem = load(image)
    used = collections.Counter()
    cmds = collections.Counter()
    for p in range(PROFILES):
        for s in range(STEPS):
            c = cam(mem, p, s)
            if c:
                _, _, _, care, _ = c
                for n, lo, hi in KEY_LAYOUT:
                    w = (1 << (hi - lo + 1)) - 1
                    if (care >> lo) & w:
                        used[n] += 1
            raw = command(mem, p, s)
            if raw and (raw >> 14) & 1:
                cmds[raw & 0xFF] += 1
    print("MOD key fields constrained by EOS's steps:")
    for n, c in used.most_common():
        print(f"  {n:<22} {c:>4} steps")
    unused = [n for n, _, _ in KEY_LAYOUT if n not in used]
    print(f"\nnever constrained: {', '.join(unused) if unused else '(none)'}")
    print(f"\ndistinct Commands in valid steps: {len(cmds)}")
    print("  " + ", ".join(f"{c}x{n}" for c, n in cmds.most_common(12)))
    return 0


def self_test():
    fails = []
    k, i = encode_cam(0x0800, 0xFFFF)
    v, c, n = ternary(k, i)
    if (v, c, n) != (0x0800, 0xFFFF, 0):
        fails.append("exact match round-trip")
    if not (encode_command({"Valid": 1}) >> 14) & 1:
        fails.append("Valid must land at bit 14")
    w = encode_value({"ValA_Constant": 0xFF})
    if (w[0] >> 24) & 0xFF != 0xFF:
        fails.append("ValA_Constant must land at bits 24-31")
    try:
        encode_command({"Command": 256})
        fails.append("over-wide Command not rejected")
    except ValueError:
        pass
    # the key layout must tile without gap or overlap up to 48 bits
    pos = 0
    for n, lo, hi in KEY_LAYOUT:
        if lo != pos:
            fails.append(f"key layout gap/overlap at {n}: expected {pos}, got {lo}")
        pos = hi + 1
    if pos != 48:
        fails.append(f"key layout covers {pos} bits, expected 48")
    # ⚠ Added after --verify caught a truncated VAL_LAYOUT that the self-test
    # had passed: it checked the KEY layout tiles but not the value layout, so a
    # header transcription that stopped at bit 47 went unnoticed until 262
    # round-trips failed. Every packed layout gets a tiling check.
    pos = 0
    for n, lo, hi in VAL_LAYOUT:
        if lo != pos:
            fails.append(f"VAL_LAYOUT gap/overlap at {n}: expected {pos}, got {lo}")
        pos = hi + 1
    if pos != 64:
        fails.append(f"VAL_LAYOUT covers {pos} bits, expected 64")
    pos = 0
    for n, lo, hi in CMD_LAYOUT:
        if lo != pos:
            fails.append(f"CMD_LAYOUT gap/overlap at {n}: expected {pos}, got {lo}")
        pos = hi + 1
    for f in fails:
        print("  FAIL:", f)
    print("self-test " + ("PASS" if not fails else "FAIL"))
    return 0 if not fails else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--keymap", action="store_true")
    ap.add_argument("--image")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    rc = 0
    if args.self_test:
        rc |= self_test()
    if args.verify or args.keymap:
        if not args.image:
            sys.exit("--verify/--keymap need --image")
        if args.verify:
            rc |= verify(args.image)
        if args.keymap:
            rc |= keymap(args.image)
    if not (args.self_test or args.verify or args.keymap):
        ap.print_help()
    return rc


if __name__ == "__main__":
    sys.exit(main())
