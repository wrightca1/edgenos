#!/usr/bin/env python3
"""mod_decode.py - decode the FM6000 MOD (egress modify) tables.

MOD is fm6000_modinit.c, 3,626 transcribed microcode pairs (PROVENANCE.md 2.5)
and the smallest of the four remaining blocks. Structure from the register
header, not inferred -- the fourth time that route has been the fast one.

    MOD_CAM(profile, step, word)  = 0x150000 + 0x8000 + 0x80*profile + 4*step
        32 profiles x 32 steps x 4 words   (KeyInvert[63:0], Key[127:64])
    MOD_COMMAND_RAM(profile, step) = 0x150000 + 0x9000 + 0x20*profile + step
        Command[7:0], Jitter[13:8], Valid(14)
    MOD_VALUE_RAM(profile, step, word) = 0x150000 + 0x9400 + 0x40*profile + 2*step
        Val{A,B,C,D}_Constant (8 bits each), Val{A,B,C,D}_DataSelect (5) + Type (3)

So a MOD "routine" is a profile: up to 32 steps, each a CAM match plus a command
and four operands. That matches the "~50 small egress-edit routines" the earlier
docs describe from write-count analysis alone.

★ ADDRESS CORRECTION. Earlier work in this repo placed L3AR at 0x158000-0x159fff
and called 0x010000 "unnamed". Both wrong: FM6000_L3AR_BASE is 0x10000, and
0x158000 is MOD_CAM. The 0x010000 page that triage group 3 could not name is
L3AR.

MOD_CAM key, 48 bits of the 64 (FM6000_MOD_CAM_KEYS):

    PAUSE(0) MIR_RX(1) MIR_TX(2) MIR_NUM(4:3) MAP_PRI(5) TRUNC(6) L2_TAG(7)
    L2_VID_EQUAL(8) MCAST_TAG(9) TX_PORT_Tag(11:10) DST_PORT_Tag(21:12)
    L2_VLAN1_TX_Tagged(22) L2_VLAN2_TX_Tagged(23) MOD_FLAGS(47:24)

PROVENANCE. Reads the image at runtime, embeds nothing.

Usage:
    mod_decode.py --image <fm6000Microcode.raw> --summary
    mod_decode.py --image <img> --profile 0
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import load, ternary  # noqa: E402

MOD_BASE = 0x150000
CAM_OFF, CMD_OFF, VAL_OFF = 0x8000, 0x9000, 0x9400
PROFILES, STEPS = 32, 32

KEY_LAYOUT = [
    ("PAUSE", 0, 0), ("MIR_RX", 1, 1), ("MIR_TX", 2, 2), ("MIR_NUM", 3, 4),
    ("MAP_PRI", 5, 5), ("TRUNC", 6, 6), ("L2_TAG", 7, 7),
    ("L2_VID_EQUAL", 8, 8), ("MCAST_TAG", 9, 9), ("TX_PORT_Tag", 10, 11),
    ("DST_PORT_Tag", 12, 21), ("L2_VLAN1_TX_Tagged", 22, 22),
    ("L2_VLAN2_TX_Tagged", 23, 23), ("MOD_FLAGS", 24, 47),
]
CMD_LAYOUT = [("Command", 0, 7), ("Jitter", 8, 13), ("Valid", 14, 14)]
VAL_LAYOUT = [
    ("ValD_Constant", 0, 7), ("ValC_Constant", 8, 15),
    ("ValB_Constant", 16, 23), ("ValA_Constant", 24, 31),
    ("ValD_DataSelect", 32, 36), ("ValD_Type", 37, 39),
    ("ValC_DataSelect", 40, 44), ("ValC_Type", 45, 47),
    # ⚠ These four were missing from the first transcription of the header,
    # which stopped at bit 47. VALUE_RAM is a full 64 bits: every operand has a
    # Constant, a DataSelect and a Type, and truncating after ValC silently
    # dropped ValB's and ValA's selectors. mod_gen --verify caught it -- 262 of
    # 329 VALUE_RAM words failed to round-trip.
    ("ValB_DataSelect", 48, 52), ("ValB_Type", 53, 55),
    ("ValA_DataSelect", 56, 60), ("ValA_Type", 61, 63),
]


def cam(mem, p, s):
    w = [mem.get(MOD_BASE + CAM_OFF + 0x80 * p + 4 * s + i) for i in range(4)]
    if any(x is None for x in w):
        return None
    if all(x in (0, 0xFFFFFFFF) for x in w):
        return None
    keyinvert = (w[1] << 32) | w[0]
    key = (w[3] << 32) | w[2]
    return (key, keyinvert) + ternary(key, keyinvert)


def command(mem, p, s):
    return mem.get(MOD_BASE + CMD_OFF + 0x20 * p + s)


def value(mem, p, s):
    w = [mem.get(MOD_BASE + VAL_OFF + 0x40 * p + 2 * s + i) for i in range(2)]
    if any(x is None for x in w):
        return None
    return w[0] | (w[1] << 32)


def fields(raw, layout):
    out = {}
    if raw is None:
        return out
    for n, lo, hi in layout:
        v = (raw >> lo) & ((1 << (hi - lo + 1)) - 1)
        if v:
            out[n] = v
    return out


def key_fields(value_, care):
    out = []
    for n, lo, hi in KEY_LAYOUT:
        w = (1 << (hi - lo + 1)) - 1
        c = (care >> lo) & w
        if c:
            out.append(f"{n}={(value_ >> lo) & w:#x}/mask{c:#x}")
    return ", ".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", required=True)
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--profile", type=int)
    args = ap.parse_args()
    mem = load(args.image)

    if args.summary:
        print("profile  steps  valid-cmds  with-value")
        tot = 0
        for p in range(PROFILES):
            steps = [s for s in range(STEPS) if cam(mem, p, s)]
            v = sum(1 for s in range(STEPS)
                    if (command(mem, p, s) or 0) & (1 << 14))
            vv = sum(1 for s in range(STEPS) if value(mem, p, s))
            if steps or v:
                tot += len(steps)
                print(f"   {p:>2}    {len(steps):>4}   {v:>8}   {vv:>8}")
        print(f"\ntotal populated CAM steps: {tot}")

    if args.profile is not None:
        p = args.profile
        print(f"=== MOD profile {p} ===")
        for s in range(STEPS):
            c = cam(mem, p, s)
            cmd = fields(command(mem, p, s), CMD_LAYOUT)
            val = fields(value(mem, p, s), VAL_LAYOUT)
            if not c and not cmd:
                continue
            print(f"  step {s:>2}")
            if c:
                _, _, v, care, never = c
                kf = key_fields(v, care)
                print(f"      match: {kf or '(any)'}" + ("  DISABLED" if never else ""))
            if cmd:
                print("      cmd:   " + ", ".join(f"{k}={v}" for k, v in cmd.items()))
            if val:
                print("      val:   " + ", ".join(f"{k}={v:#x}" for k, v in val.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
