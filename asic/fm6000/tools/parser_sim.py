#!/usr/bin/env python3
"""parser_sim.py - run a frame through an FM6000 parser program, in software.

Built after four hypothesis-driven fixes failed on hardware. Guessing mechanisms
had stopped paying: EOS's parser forwards and ours does not, both covering the
same slices, so the difference is a fact about two programs we already hold in
full. This executes them instead of reasoning about them.

Given a frame and a parser program, it walks slices 0..27 exactly as the
hardware does -- match the 64-bit key, take the LAST matching entry (measured,
see docs/PARSER-CONVENTIONS.md), apply the action -- and reports the resulting
FIELDS channels, ACTION_FLAGS and final state. Diffing two runs shows precisely
where two programs disagree about the same frame.

⚠ WHAT THIS IS NOT. A model of our understanding, not of the silicon. If our
reading of the encoding is wrong, the simulator is wrong in the same way and
will agree with itself. It can show that two programs differ; it cannot prove
either is correct. Hardware remains the arbiter.

Usage:
    parser_sim.py --image <fm6000Microcode.raw> --frame ospf-hello --isl
    parser_sim.py --ours --frame ospf-hello --isl
    parser_sim.py --diff <image> --frame ospf-hello --isl
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import (  # noqa: E402
    load, decode_slice, action_value, action_field, next_state,
    NUM_SLICES, FIELDS_CHANNEL,
)

FLAG_NAME = {
    1: "ISL_RX_Tagged", 2: "ISL_Type0", 3: "ISL_Type1", 4: "ISL_FType0",
    5: "ISL_FType1", 6: "VLAN1_Tagged(S)", 7: "VLAN2_Tagged(C)", 8: "L3_IsIPv4",
    9: "L3_IsIPv6", 10: "IDGLORT_NZ", 11: "ISGLORT_NZ", 12: "DIP_V4InV6",
    13: "TTL_Expired", 14: "HeadFrag", 15: "DontFrag", 16: "L3_Options",
    17: "L3_Mcst(DMAC)", 18: "L3_Bcst", 19: "L3_Mcst(DIP)", 22: "IPv6_HopByHop",
    34: "PAUSE", 35: "PAUSE_CBP", 37: "ChecksumError",
}
for _b in range(24, 32):
    FLAG_NAME[_b] = f"HdrOffsets[{_b - 24}]"


def build_frame(kind, isl):
    """Return frame bytes. Layouts are the ones this switch actually sees."""
    dmac = bytes.fromhex("01005e000005")          # OSPF AllSPFRouters
    smac = bytes.fromhex("444ca8315dab")
    tag = bytes.fromhex("010003efff000000")       # F64, per fm6000_portd.c
    if kind == "ospf-hello":
        ip = bytes.fromhex(
            "4500003400010000"    # ver/IHL, TOS, total len, id, flags/frag
            "0159"                # TTL=1, proto=89 (OSPF)
            "0000"                # checksum
            "0a65651a"            # src 10.101.101.26
            "e0000005")           # dst 224.0.0.5
        ether = bytes.fromhex("0800")
    elif kind == "arp":
        ip = bytes.fromhex("0001080006040001") + smac + bytes.fromhex("0a65651a")
        ether = bytes.fromhex("0806")
        dmac = bytes.fromhex("ffffffffffff")
    else:
        raise SystemExit(f"unknown frame {kind}")
    body = dmac + smac + (tag if isl else b"") + ether + ip
    return body + b"\x00" * (64 - len(body) if len(body) < 64 else 0)


def frame_word(frame, slice_, window_shift=0):
    """The slice's 4-byte window as a 32-bit key half.

    Datasheet 5.5: the first byte received occupies the most significant byte of
    the structure, so bytes [0,1] form the low halfword with byte 0 on top.

    ⚠ window_shift is NOT optional, and ignoring it made the first version of
    this simulator lie. Datasheet 5.5.1:

        FRAME_DATA = FRAME_BYTES[4*i + 3 - window_shift : 4*i - window_shift]

    ShiftNextSlice advances window_shift for the NEXT slice, modulo 8. EOS uses
    it: at slice 3 it matches the EtherType in the HIGH halfword, which only
    makes sense if that window is bytes 10-13 rather than 12-15. The reason is
    alignment -- an Ethernet header is 14 bytes, so shifting by 2 puts the IP
    header on a slice boundary.

    With shift ignored, EOS's own program appeared to terminate at slice 3 and
    extract no L3 fields at all, which is plainly false for a switch that
    routes.
    """
    lo = 4 * slice_ - window_shift
    b = bytes(frame[i] if 0 <= i < len(frame) else 0 for i in range(lo, lo + 4))
    # Datasheet 5.5: "the first byte received loaded into the MOST SIGNIFICANT
    # byte of those structures". Applied to the whole 32-bit frame key, so
    # byte0 is key[31:24]. Halfword0 = {byte0,byte1} is therefore the HIGH half
    # of the key, and Halfword1 the low -- which is what EOS's slice-3 rules
    # require: they match the VLAN TPID as 0x81000000/0xffff0000, i.e. at bytes
    # 12-13, the EtherType position.
    return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]


def load_program(mem):
    """{slice: [(entry, value, care, never, action)]} in entry order."""
    prog = {}
    for s in range(NUM_SLICES):
        rows = []
        for entry, key, inv, value, care, never, act in decode_slice(mem, s):
            a = action_value(act)
            if a is None:
                continue
            rows.append((entry, value, care, never, a))
        if rows:
            prog[s] = rows
    return prog


def run(prog, frame, verbose=False, init_state=0):
    state = init_state
    fields = {}
    flags = 0
    shift = 0
    trace = []
    for s in range(NUM_SLICES):
        fw = frame_word(frame, s, shift)
        key = (state << 32) | fw
        hit = None
        for entry, value, care, never, a in prog.get(s, []):
            if never:                       # entry disabled: can never match
                continue
            if (key & care) == value:
                hit = (entry, a)            # last match wins
        if hit is None:
            trace.append((s, None, state, "no match"))
            continue
        entry, a = hit
        for h in (0, 1):
            if action_field(a, f"Byte{2*h}Enable") or action_field(a, f"Byte{2*h+1}Enable"):
                ch = action_field(a, f"Halfword{h}Dest")
                half = ((fw >> 16) & 0xFFFF) if h == 0 else (fw & 0xFFFF)
                fields[ch] = half
        flags |= action_field(a, "SetFlags")
        ns, ok = next_state(state, fw, 0xFFFFFFFF, a)
        term = action_field(a, "Terminate")
        adv = action_field(a, "ShiftNextSlice")
        trace.append((s, entry, state,
                      f"win={shift} -> 0x{ns:08x}" + (f" shift+={adv}" if adv else "")
                      + ("  TERMINATE" if term else "")))
        state = ns
        shift = (shift + adv) % 8
        if term:
            break
    return fields, flags, state, trace


def report(name, fields, flags, state, trace, verbose):
    print(f"=== {name} ===")
    if verbose:
        for s, entry, st, note in trace:
            print(f"  slice {s:>2} state=0x{st:08x} entry={entry if entry is not None else '-':>3}  {note}")
    print("  FIELDS written:")
    for ch in sorted(fields):
        print(f"    ch{ch:<3} = 0x{fields[ch]:04x}   {FIELDS_CHANNEL.get(ch,'(generic)')}")
    print(f"  FLAGS = 0x{flags:010x}")
    for b in sorted(FLAG_NAME):
        if (flags >> b) & 1:
            print(f"    bit {b:>2} {FLAG_NAME[b]}")
    print(f"  final state = 0x{state:08x}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", help="run EOS's program from a microcode image")
    ap.add_argument("--ours", action="store_true", help="run our generated program")
    ap.add_argument("--diff", metavar="IMAGE", help="run both and diff")
    ap.add_argument("--frame", default="ospf-hello", choices=["ospf-hello", "arp"])
    ap.add_argument("--isl", action="store_true", help="prepend the F64 ISL tag")
    ap.add_argument("-v", "--verbose", action="store_true", help="per-slice trace")
    ap.add_argument("--init-state", default="0",
                    help="PARSER_INIT_STATE for the ingress port (hex). ⚠ NOT zero on "
                         "real hardware: the microcode image has zeros there but the "
                         "replay programs per-port values -- port 0 reads 0x6b3f0000 on "
                         "this switch. EOS's IPv4 rules require STATE8[3] & 0x20, and "
                         "0x6b supplies it, so simulating from 0 makes EOS's own program "
                         "appear to terminate at slice 3.")
    args = ap.parse_args()

    init = int(args.init_state, 16)
    frame = build_frame(args.frame, args.isl)
    print(f"frame: {args.frame}{' +ISL' if args.isl else ''}, {len(frame)} bytes")
    print(f"  {frame[:24].hex()}...\n")

    def ours_prog():
        from parser_program import build_program, place, writes
        mem = {}
        for a, v in writes(place(build_program())):
            mem[a] = v
        return load_program(mem)

    if args.diff:
        fe = run(load_program(load(args.diff)), frame, args.verbose, init)
        fo = run(ours_prog(), frame, args.verbose, init)
        report("EOS", *fe, args.verbose)
        print()
        report("OURS", *fo, args.verbose)
        print("\n=== DIFF ===")
        chans = set(fe[0]) | set(fo[0])
        for ch in sorted(chans):
            a, b = fe[0].get(ch), fo[0].get(ch)
            if a != b:
                print(f"  ch{ch:<3} {FIELDS_CHANNEL.get(ch,'(generic)'):<28} "
                      f"EOS={'-' if a is None else f'0x{a:04x}'}  "
                      f"OURS={'-' if b is None else f'0x{b:04x}'}")
        d = fe[1] ^ fo[1]
        for b in range(38):
            if (d >> b) & 1:
                who = "EOS only" if (fe[1] >> b) & 1 else "OURS only"
                print(f"  flag {b:>2} {FLAG_NAME.get(b,'?'):<20} {who}")
        return 0

    if args.image:
        report("EOS", *run(load_program(load(args.image)), frame, args.verbose, init), args.verbose)
    if args.ours:
        report("OURS", *run(ours_prog(), frame, args.verbose, init), args.verbose)
    if not (args.image or args.ours or args.diff):
        ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
