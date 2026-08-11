#!/usr/bin/env python3
"""ffu_decode.py - decode the FM6000 FFU (filter/forward unit) tables.

The FFU is the last big block of fwd4.txt: 5,364 replay writes, every one of
which belongs to exactly one of 59 `0x3f0000` strobe groups (CHECKLIST B1). This
decodes what those writes actually build.

★ GEOMETRY, from the register header. FFU_BASE = 0x300000.

    SLICE_CAM(slice, entry, word)   = 0x380000 + 0x4000*slice + 4*entry + word
        24 slices x 1024 entries x 4 words -- a 38-BIT ternary key (see below;
        four words look like 128 bits and are not)
    SLICE_SCENARIO_CAM(slice, e, w) = 0x381800 + 0x4000*slice + 2*e + word
    SLICE_ACTION(slice, entry, word) = 0x381000 + 0x4000*slice + 2*entry + word
        24 x 1024 x 2 words
    BST_KEY(grp, sub, entry)        = 0x308000 + 0x10000*grp + 0x400*sub + entry
        4 groups x 16 x 1024, a bare 32-bit Value
    BST_ACTION(grp, sub, entry, w)  = 0x300000 + 0x10000*grp + 0x800*sub + 2*entry
    BST_SCENARIO_CAM(grp, ent, w)   = 0x30C000 + 0x10000*grp + 2*ent
        4 x 32, 32-bit Key + KeyInvert
    ATOMIC_APPLY                    = 0x3F0000   bit0 CAM_Slices, bit1 BST_Slices

★ MEASURED, from the replay (fwd4.txt), not from the microcode image -- the FFU
is configured entirely by the replay and does not appear in fm6000Microcode.raw:

    14,490 writes in the FFU region: 5,960 CAM half, 8,450 BST half, 80 scenario
    59 ATOMIC_APPLY strobes: 43 with CAM_Slices=1, 16 with BST_Slices=1
    127 live CAM entries across 24 slices, 113 action words, 67 BST keys

⚠ The final value at 0x3f0000 is 0x2 (BST only) and a last-write-wins load makes
the CAM half look uncommitted. It is not: 43 of the 59 strobes commit the CAM.
Read the strobe HISTORY, never the final value -- a commit register's last value
says nothing about what it committed.

★ TWO INDEPENDENT COMMIT DOMAINS. ATOMIC_APPLY has separate CAM_Slices and
BST_Slices bits, so the CAM half and the BST half are committed separately. The
replay's `0x3f0000 <- 0x00000002` writes recorded in ROUTING-FIB.md are BST
commits, not CAM commits. A generator that collapses the 59 strobe groups into
one commit destroys that separation -- which is exactly the failure mode
EOS-SOURCES.md warned about for the CPU-punt traps.

★ THE BST IS THE ROUTE TABLE, and this closes a loop. BST_ACTION carries `LPM`
(31:24) and a `Route` bit, and BST_KEY is a bare 32-bit value with no mask --
i.e. a sorted-prefix binary search, not a ternary match. That is precisely the
"sorted prefix array 0x33bxxx + shadow, action array 0x337xxx" that
ROUTING-FIB.md decoded empirically and `fm6000_route` already drives: group 3
puts BST_KEY at 0x338000 and BST_ACTION at 0x330000. The FIB work and the FFU
work were the same table seen from two directions.

    BST_ACTION   ActionData 23:0, LPM 31:24, TagData 43:32, TagCmd 45:44,
                 Route 46, Precedence 49:47
    SLICE_ACTION ActionData 23:0, TagData 35:24, TagCmd 37:36, Route 38,
                 Precedence 41:39, Parity 43:42

ActionData 23:0 is the FFU's output to the rest of the pipeline -- it lands in
the L3AR key as FFU_DATA_W8A/W8B/W24_TOP (see l3ar_decode.KEY_LAYOUT), which is
how `ffuFlagTrapFrame` and `ffuFlagDropFrame` are matched. The two blocks join
here.

⚠ NOT YET ESTABLISHED. The 128-bit SLICE_CAM key has no field breakdown in the
header -- unlike L3AR's FFU_..._CAM_KEYS, there is no FM6000_FFU_SLICE_CAM_l_*
list. What each byte of the key means is set at runtime by the scenario ByteMux
registers (SCENARIO_CFG1 ByteMux_0..3, 6 bits each, plus Top4Mux), so the key
layout is CONFIGURED, not fixed. Any generator must program the muxes and the
key together; reading one without the other is meaningless.

PROVENANCE. Reads an image at runtime, embeds nothing. Field names, widths and
positions are register-header facts.

Usage:
    ffu_decode.py --image <fm6000Microcode.raw> --summary
    ffu_decode.py --image <img> --slice 0
    ffu_decode.py --image <img> --bst
    ffu_decode.py --image <img> --verify
"""
import argparse
import collections
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import load, ternary, FIELDS_CHANNEL  # noqa: E402

FFU_BASE = 0x300000
ATOMIC_APPLY = FFU_BASE + 0xF0000
CAM_SLICES, CAM_ENTRIES = 24, 1024
BST_GROUPS, BST_SUBS, BST_ENTRIES = 4, 16, 1024
SCEN_ENTRIES = 32
MASK64 = 0xFFFFFFFFFFFFFFFF

SLICE_ACTION_LAYOUT = [
    ("ActionData", 0, 23), ("TagData", 24, 35), ("TagCmd", 36, 37),
    ("Route", 38, 38), ("Precedence", 39, 41), ("Parity", 42, 43),
]
BST_ACTION_LAYOUT = [
    ("ActionData", 0, 23), ("LPM", 24, 31), ("TagData", 32, 43),
    ("TagCmd", 44, 45), ("Route", 46, 46), ("Precedence", 47, 49),
]
# FM6000_FFU_SLICE_SCENARIO_CFG -- 32 scenarios x 24 slices, 2 words.
# ★ THIS EXPLAINS THE 38-BIT KEY. ByteMux_0..3 select four source bytes (4 x 8 =
# 32 bits) and Top4Mux selects a further 6, giving exactly the 38 bits measured
# off EOS's entries. The header's field structure predicts the width that had to
# be discovered empirically -- had this been read first, the 64-bit assumption
# would never have been made.
SLICE_SCEN_CFG_LAYOUT = [
    ("ByteMux_0", 0, 5), ("ByteMux_1", 6, 11), ("ByteMux_2", 12, 17),
    ("ByteMux_3", 18, 23), ("Top4Mux", 24, 28), ("StartCompare", 29, 29),
    ("StartSet", 30, 30), ("reserved0", 31, 31), ("ActionLength", 32, 33),
    ("ValidLow", 34, 34), ("ValidHigh", 35, 35), ("Case", 36, 37),
]

SCEN_CFG1_LAYOUT = [
    ("ByteMux_0", 0, 5), ("ByteMux_1", 6, 11), ("ByteMux_2", 12, 17),
    ("ByteMux_3", 18, 23), ("Top4Mux", 24, 28), ("Reserved", 29, 31),
]


def slice_cam(s, e, w):
    return FFU_BASE + 0x80000 + 0x4000 * s + 4 * e + w


def slice_action(s, e, w):
    return FFU_BASE + 0x81000 + 0x4000 * s + 2 * e + w


def bst_key(g, sub, e):
    return FFU_BASE + 0x08000 + 0x10000 * g + 0x400 * sub + e


def bst_action(g, sub, e, w):
    return FFU_BASE + 0x10000 * g + 0x800 * sub + 2 * e + w


def scenario_cam(g, e, w):
    return FFU_BASE + 0x0C000 + 0x10000 * g + 2 * e + w


def scenario_cfg1(g, e):
    return FFU_BASE + 0x0C040 + 0x10000 * g + e


def slice_scen_cfg(s, e, w):
    return FFU_BASE + 0x81840 + 0x4000 * s + 2 * e + w


def slice_master_valid(s):
    return FFU_BASE + 0x81880 + 0x4000 * s


def mux_name(v):
    """Name a ByteMux source, if the parser channel map knows it.

    ⚠ HYPOTHESIS, not established. ByteMux values come in PAIRS in every
    configured scenario -- [0,0,17,17], [60,24,18,18], [21,21,8,58] -- the same
    pattern MOD's value slices show, where a DataSelect feeds two consecutive
    bytes. That is consistent with one halfword-channel bus running parser
    (writes) -> FFU (selects) -> MOD (reads), and the named values fit: slices
    select L3_SIP, L4_SRC, L3_LENGTH, L3_DIP -- what an ACL matches on.

    ⚠ AND IT IS NOT SETTLED. EOS uses ByteMux 53, 58 and 60, and a scan of all
    2,145 of EOS's parser actions shows the parser writes channels 0..42 ONLY
    (max destination 42, across both Halfword0Dest and Halfword1Dest). So those
    three values cannot be parser channels, and two readings survive:

      (a) DIRECT CHANNEL INDEX into a 64-channel space, where 44..63 are written
          by some other block (mapper, next-hop, or FFU remap) rather than the
          parser. Favoured by the pairing: [17,17] is then channel 17 feeding
          both bytes of a halfword, exactly like MOD's "A:t2/d23 B:t2/d23".
      (b) BYTE ADDRESS into the 32 halfword channels: value v -> channel v//2,
          byte v%2. Then 53 -> ch26 byte1, 58 -> ch29 byte0, 60 -> ch30 byte0,
          all of which the parser DOES write. Favoured by nothing else, and it
          makes [17,17] select the same byte twice.

    Neither reading is refuted by the configurations EOS ships: both explain
    [32,32,32,32] as redundant, so that pattern discriminates nothing. Values
    above 42 are reported unmapped rather than guessed at, and a generator must
    not assume either reading. The decisive test is to program one scenario with
    a known ByteMux and observe which frame bytes change the match -- hardware,
    which is currently blocked on the same missing capture path as MOD (A4).
    """
    n = FIELDS_CHANNEL.get(v)
    return f"{v}={n}" if n else f"{v}=?"


def show_scenarios(mem):
    """Which bytes each CAM slice actually composes its key from."""
    print("=== FFU slice scenario configuration (the key composition) ===")
    print("ByteMux_n selects source byte n of the 32-bit lower key;"
          " Top4Mux the upper 6 bits.\n")
    any_ = False
    for s in range(CAM_SLICES):
        mv = mem.get(slice_master_valid(s))
        rows = []
        for e in range(32):
            w0 = mem.get(slice_scen_cfg(s, e, 0))
            w1 = mem.get(slice_scen_cfg(s, e, 1))
            if w0 is None and w1 is None:
                continue
            raw = (w0 or 0) | ((w1 or 0) << 32)
            if not raw:
                continue
            rows.append((e, fields(raw, SLICE_SCEN_CFG_LAYOUT)))
        if not rows and mv is None:
            continue
        any_ = True
        print(f"--- slice {s}  MASTER_VALID="
              + ("absent" if mv is None else f"0x{mv:x}")
              + f"  {len(rows)} scenarios configured ---")
        for e, f in rows[:6]:
            mux = [f.get(f"ByteMux_{i}", 0) for i in range(4)]
            extra = {k: v for k, v in f.items() if not k.startswith("ByteMux_")}
            print(f"   scen{e:<3} " + " | ".join(mux_name(m) for m in mux))
            print("          " + ", ".join(f"{k}={v}" for k, v in extra.items()))
        if len(rows) > 6:
            print(f"   ... {len(rows) - 6} more")
    if not any_:
        print("  (no slice scenario configuration present in this input)")


def load_any(path):
    """Load either a raw microcode image or an "<addr> <value>" replay.

    The FFU lives in the replay, not in fm6000Microcode.raw, so this tool has to
    read both formats. A replay may write an address more than once (strobes);
    the LAST write wins, which is what the hardware sees.
    """
    with open(path, "rb") as fh:
        head = fh.read(64)
    if b"\n" in head and all(c in b"0123456789abcdefABCDEF \t\r\n" for c in head):
        mem = {}
        with open(path) as fh:
            for line in fh:
                p = line.split()
                if len(p) == 2:
                    try:
                        mem[int(p[0], 16)] = int(p[1], 16)
                    except ValueError:
                        pass
        return mem
    return load(path)


def fields(raw, layout):
    out = {}
    if raw is None:
        return out
    for n, lo, hi in layout:
        v = (raw >> lo) & ((1 << (hi - lo + 1)) - 1)
        if v:
            out[n] = v
    return out


# ⚠ THE SLICE CAM IS 38 BITS PER HALF, NOT 64. Four words per entry look like a
# 128-bit key -- two 64-bit halves -- and that is wrong. EOS's entries read
# ['0xffffffff','0x0000003f','0xffffffff','0x0000003f']: the high word of each
# half only ever reaches 0x3f, so each half is 32 + 6 = 38 bits and bits 38..63
# do not exist.
#
# Masking to 64 made every one of the 127 entries decode as never-match (bits
# 38-63 read as Key=0,KeyInvert=0), and the tool cheerfully reported "0 live CAM
# entries" for a CAM that ATOMIC_APPLY commits 43 times. Measure the width from
# the data; do not infer it from the word count.
CAM_KEY_BITS = 38
CAM_KEY_MASK = (1 << CAM_KEY_BITS) - 1


def read_cam_entry(mem, s, e):
    """One 38-bit ternary entry, or None if absent or never-match."""
    w = [mem.get(slice_cam(s, e, i)) for i in range(4)]
    if any(x is None for x in w):
        return None
    keyinvert = (((w[1] << 32) | w[0]) & CAM_KEY_MASK)
    key = (((w[3] << 32) | w[2]) & CAM_KEY_MASK)
    # Use the project's shared ternary decode rather than re-deriving it here --
    # a hand-rolled "value = key & keyinvert" got the convention backwards and
    # failed 124 of 127 round-trips. One decoder, one convention.
    value, care, never = ternary(key, keyinvert)
    never &= CAM_KEY_MASK
    if never:
        return None
    return key, keyinvert, value & CAM_KEY_MASK, care & CAM_KEY_MASK


def read_slice_action(mem, s, e):
    a = [mem.get(slice_action(s, e, i)) for i in range(2)]
    if any(x is None for x in a):
        return None
    return a[0] | (a[1] << 32)


def read_bst_action(mem, g, sub, e):
    a = [mem.get(bst_action(g, sub, e, i)) for i in range(2)]
    if any(x is None for x in a):
        return None
    return a[0] | (a[1] << 32)


def summary(mem):
    print("FFU CAM slices (24 x 1024 x 38-bit ternary):")
    tot = 0
    for s in range(CAM_SLICES):
        n = sum(1 for e in range(CAM_ENTRIES) if read_cam_entry(mem, s, e))
        a = sum(1 for e in range(CAM_ENTRIES) if read_slice_action(mem, s, e))
        if n or a:
            print(f"   slice {s:>2}: {n:>4} live CAM entries, {a:>4} action words")
        tot += n
    print(f"   total live CAM entries: {tot}")

    print("\nFFU BST (4 groups x 16 x 1024, sorted-prefix search):")
    btot = 0
    for g in range(BST_GROUPS):
        keys = acts = 0
        for sub in range(BST_SUBS):
            keys += sum(1 for e in range(BST_ENTRIES)
                        if mem.get(bst_key(g, sub, e)) not in (None, 0))
            acts += sum(1 for e in range(BST_ENTRIES)
                        if read_bst_action(mem, g, sub, e))
        if keys or acts:
            print(f"   group {g}: {keys:>5} keys, {acts:>5} actions"
                  f"   (BST_KEY at 0x{bst_key(g, 0, 0):06x},"
                  f" BST_ACTION at 0x{bst_action(g, 0, 0, 0):06x})")
        btot += keys
    print(f"   total BST keys: {btot}")

    print("\nscenario CAM (4 x 32) and ByteMux configuration:")
    for g in range(BST_GROUPS):
        live = []
        for e in range(SCEN_ENTRIES):
            w0 = mem.get(scenario_cam(g, e, 0))
            w1 = mem.get(scenario_cam(g, e, 1))
            if w0 is None or w1 is None:
                continue
            val, care, never = ternary(w1, w0)
            if not never and care:
                live.append((e, val, care))
        cfg = [(e, mem.get(scenario_cfg1(g, e))) for e in range(SCEN_ENTRIES)]
        cfg = [(e, v) for e, v in cfg if v]
        if live or cfg:
            print(f"   group {g}: {len(live)} scenario entries, {len(cfg)} ByteMux configs")
            for e, v in cfg[:3]:
                f = fields(v, SCEN_CFG1_LAYOUT)
                print(f"      cfg{e}: " + ", ".join(f"{k}={x}" for k, x in f.items()))

    st = mem.get(ATOMIC_APPLY)
    print(f"\nATOMIC_APPLY (0x{ATOMIC_APPLY:06x}) in image: "
          + ("absent" if st is None else
             f"0x{st:08x} (CAM_Slices={st & 1}, BST_Slices={(st >> 1) & 1})"))


def show_slice(mem, s):
    print(f"=== FFU CAM slice {s} ===")
    n = 0
    for e in range(CAM_ENTRIES):
        c = read_cam_entry(mem, s, e)
        if not c:
            continue
        _, _, value, care = c
        act = read_slice_action(mem, s, e)
        f = fields(act, SLICE_ACTION_LAYOUT)
        n += 1
        if n <= 24:
            fs = ", ".join(f"{k}={v:#x}" for k, v in f.items()) or "(no action)"
            print(f"  e{e:<4} key={value:#034x}/care={care:#034x}")
            print(f"        {fs}")
    print(f"  ... {n} live entries total" if n > 24 else f"  {n} live entries")


def show_bst(mem):
    print("=== FFU BST: the route table ===")
    for g in range(BST_GROUPS):
        for sub in range(BST_SUBS):
            ents = []
            for e in range(BST_ENTRIES):
                k = mem.get(bst_key(g, sub, e))
                a = read_bst_action(mem, g, sub, e)
                if k or a:
                    ents.append((e, k, a))
            if not ents:
                continue
            print(f"--- group {g} sub {sub}: {len(ents)} entries ---")
            for e, k, a in ents[:8]:
                f = fields(a, BST_ACTION_LAYOUT)
                pfx = f.get("LPM", 0)
                ip = f"{(k or 0) >> 24 & 255}.{(k or 0) >> 16 & 255}." \
                     f"{(k or 0) >> 8 & 255}.{(k or 0) & 255}"
                print(f"   e{e:<4} key=0x{k or 0:08x} ({ip}/{pfx})  "
                      + ", ".join(f"{n}={v:#x}" for n, v in f.items()))
            if len(ents) > 8:
                print(f"   ... {len(ents) - 8} more")


def verify(mem):
    """Round-trip every populated entry through the encoders."""
    ok = bad = 0
    aok = abad = 0
    for s in range(CAM_SLICES):
        for e in range(CAM_ENTRIES):
            c = read_cam_entry(mem, s, e)
            if c:
                key, keyinvert, value, care = c
                rk = (value | ~care) & CAM_KEY_MASK
                ri = (~value | ~care) & CAM_KEY_MASK
                if (rk, ri) == (key, keyinvert):
                    ok += 1
                else:
                    bad += 1
            a = read_slice_action(mem, s, e)
            if a is not None:
                acc = 0
                for n, lo, hi in SLICE_ACTION_LAYOUT:
                    acc |= ((a >> lo) & ((1 << (hi - lo + 1)) - 1)) << lo
                if acc == a:
                    aok += 1
                else:
                    abad += 1
    print(f"SLICE_CAM entries round-tripped:   {ok} ok, {bad} mismatched")
    print(f"SLICE_ACTION words round-tripped:  {aok} ok, {abad} mismatched")
    prob = []
    for name, lay, width in (("SLICE_ACTION", SLICE_ACTION_LAYOUT, 44),
                             ("BST_ACTION", BST_ACTION_LAYOUT, 50),
                             ("SCEN_CFG1", SCEN_CFG1_LAYOUT, 32)):
        pos = 0
        for n, lo, hi in lay:
            if lo != pos:
                prob.append(f"{name} gap/overlap at {n}: expected {pos}, got {lo}")
            pos = hi + 1
        if pos != width:
            prob.append(f"{name} covers {pos} bits, expected {width}")
    for p in prob:
        print("  " + p)
    # ⚠ A PASS THAT CHECKED NOTHING IS A FAILURE. The first run of this tool
    # printed "VERIFY PASS" against 0 entries, because the FFU is configured by
    # fwd4.txt and is not present in fm6000Microcode.raw at all. That is the same
    # vacuous-invariant trap that has now bitten this project five times; an
    # empty round-trip proves only that the loop body never ran.
    if ok + aok == 0:
        print("\nVERIFY FAIL - checked 0 entries. The FFU is not in this image;"
              " point --image at a replay (fwd4.txt) instead.")
        return 1
    good = bad == 0 and abad == 0 and not prob
    print("\nVERIFY " + (f"PASS - {ok + aok} entries reproduced from a program"
                         " we did not write" if good else "FAIL"))
    return 0 if good else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", required=True)
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--bst", action="store_true")
    ap.add_argument("--scenarios", action="store_true")
    ap.add_argument("--slice", type=int)
    args = ap.parse_args()
    mem = load_any(args.image)
    rc = 0
    if args.summary:
        summary(mem)
    if args.slice is not None:
        show_slice(mem, args.slice)
    if args.bst:
        show_bst(mem)
    if args.scenarios:
        show_scenarios(mem)
    if args.verify:
        rc = verify(mem)
    if not (args.summary or args.verify or args.bst or args.scenarios
            or args.slice is not None):
        ap.print_help()
    return rc


if __name__ == "__main__":
    sys.exit(main())
