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

★ THE 32 CAM SLICES HAVE FIXED ROLES -- they are not interchangeable profiles.
Datasheet 5.21.4:

    slices 0..15   one command each        -> MOD_COMMAND_RAM[0..15]
    slice  16      commands 16..20         -> MOD_COMMAND_RAM[16..19]
    slices 17..30  egress data 0..56       -> MOD_VALUE_RAM[0..13]
    slice  31      stats data 0..3         -> MOD_VALUE_RAM[14]

This tool calls the first index "profile" for continuity with the addressing,
but a generator MUST respect the roles: emitting a command into a data slice, or
data into a command slice, produces a table that decodes cleanly and does the
wrong thing. The earlier "~50 small egress-edit routines" reading was write-count
analysis and did not see this structure at all.

⚠ Table 5-94 gives MOD_FLAGS as bits 47:23, which overlaps L2_VLAN2_TX_Tagged at
bit 23. The register header says 24:47 and is authoritative; the datasheet has a
typo. Same class of defect as Table 5-5's duplicated L3_SIP/L3_DIP rows.

★ ADDRESS CORRECTION. Earlier work in this repo placed L3AR at 0x158000-0x159fff
and called 0x010000 "unnamed". Both wrong: FM6000_L3AR_BASE is 0x10000, and
0x158000 is MOD_CAM. The 0x010000 page that triage group 3 could not name is
L3AR.

MOD_CAM key, 48 bits of the 64 (FM6000_MOD_CAM_KEYS):

    PAUSE(0) MIR_RX(1) MIR_TX(2) MIR_NUM(4:3) MAP_PRI(5) TRUNC(6) L2_TAG(7)
    L2_VID_EQUAL(8) MCAST_TAG(9) TX_PORT_Tag(11:10) DST_PORT_Tag(21:12)
    L2_VLAN1_TX_Tagged(22) L2_VLAN2_TX_Tagged(23) MOD_FLAGS(47:24)

SEMANTICS, datasheet 5.21.4.2 / 5.21.4.3:

  * 16 command slices, each a 32x48-bit CAM and a 32x16-bit RAM, producing at
    most one command. Fields: Command, Jitter, Valid, and a Drop flag.
  * 15 value slices producing up to 60 bytes of frame data -- 4 bytes per slice,
    56 to the egress modifier and 4 to stats. Each byte has DataSelect (one of 24
    source channels), Type (validity/transformation) and Constant.
  * Bytes are A..D with A sent FIRST and D last. Type zero means invalid and the
    byte is omitted from the stream -- so ordering is load-bearing, not cosmetic.
  * Jitter accumulates across all 16 slices.
  * Drop: the frame is forwarded if and ONLY if every matching entry's drop flag
    is zero. Any slice can veto.
  * ⚠ Commands are not guaranteed to run: the modifier skips trailing commands
    if the frame ends first. "Skip to byte 72 then insert 4 bytes" does nothing
    on a 64-byte frame.

⚠ The datasheet lists a Drop flag that the register header does not define --
the header stops at Valid (bit 14). Bit 15 is never set in any of EOS's 369
command words, and nothing above 15 is either, so Drop is most likely bit 15 and
simply unused here. Left out of CMD_LAYOUT rather than guessed at; a generator
that needs drop behaviour must confirm the position first.

★ Match priority is STATED here, not inferred: "the highest matching entry is
selected". That is the same last-match-wins rule we had to establish for the
parser CAM by measuring 2,349 overlapping rule pairs. It was in the datasheet
all along, in a different section.

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


def value_bank(cam_slice):
    """MOD_VALUE_RAM bank for a CAM slice, or None if the slice carries no data.

    ⚠ The bank index is NOT the CAM slice index. Datasheet 5.21.4: data slices
    17..30 use MOD_VALUE_RAM[0..13] and slice 31 uses [14]. Pairing them by the
    same index -- as the first version of this file did -- reports every data
    slice as having no value words, and every command slice as owning value
    words that belong to a data slice 17 places away.

    The --verify round-trip passed throughout, because it encodes and decodes raw
    words and never checks which CAM step a word belongs to. Third time this
    session a check has passed on something wrong by testing the wrong invariant.
    """
    if 17 <= cam_slice <= 30:
        return cam_slice - 17
    if cam_slice == 31:
        return 14
    return None


def value(mem, p, s, raw_bank=False):
    """Value words for CAM slice p, step s. raw_bank=True indexes banks directly."""
    bank = p if raw_bank else value_bank(p)
    if bank is None:
        return None
    w = [mem.get(MOD_BASE + VAL_OFF + 0x40 * bank + 2 * s + i) for i in range(2)]
    if any(x is None for x in w):
        return None
    return w[0] | (w[1] << 32)


def command_bank(cam_slice):
    """MOD_COMMAND_RAM bank(s) for a CAM slice. Slices 0..15 map 1:1; slice 16
    drives banks 16..19; data slices carry no command."""
    if cam_slice <= 15:
        return [cam_slice]
    if cam_slice == 16:
        return [16, 17, 18, 19]
    return []


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
