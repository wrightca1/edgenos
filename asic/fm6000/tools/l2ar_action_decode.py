#!/usr/bin/env python3
"""l2ar_action_decode.py - decode the three tables L2AR actions index into.

THE POINT. FEATURE-COMPLETE-CHECKLIST A2 records the L2AR generator as "blocked
on a second decode, not on encoding": l2ar_gen.py can already encode rules and
round-trips EOS's own, but a rule's action indexes DMT_PROFILE, SetCpuCode and
SetMirror -- tables configured elsewhere -- so "trap to CPU" could not be
authored without knowing which entry that is. This decodes those tables.

★ ALL THREE ARE FULLY SPECIFIED IN fm6000_api_regs_int.h AND ALWAYS WERE. The
block was never a decode problem; nobody had looked. Same lesson as the six
wrong bit-order inferences in PARSER-CONVENTIONS.md, and it cost longer than any
of them.

    L2AR_ACTION_DMT       0x146000   3 banks x 32 entries x 3 words
                          ACTION_DROP_CODE [7:0]  CmdA [11:8]  CmdB [14:12]
                          ACTION_DMASK   [95:20]  76-bit destination port mask
    L2AR_ACTION_CPU_CODE  0x146200   128 entries, four 8-bit codes each
    L2AR_ACTION_MIRROR    0x146400   4 banks x 128, four 4-bit fields

CmdA CORRELATES STRONGLY WITH THE DESTINATION SHAPE, but the two are
independent fields and EOS does not couple them perfectly. Over its 32
populated entries:

    CmdA=0   76 ports  x2
    CmdA=1   empty     x15      port 0  x2      <- 2 exceptions
    CmdA=2   76 ports  x1       75 ports x4     21 ports x2
    CmdA=3   port 0    x5       empty    x1     <- 1 exception

⚠ So the obvious reading -- CmdA=1 drop, =2 flood, =3 redirect, =0 forward --
describes the common case and is NOT a decode. Three of 32 entries contradict
it. The verb names in VERB below are a convenience for reading a dump; do not
author actions on the strength of them without confirming on hardware.

★ PORT 0 IS THE CPU, on two independent signatures and neither needs a
datasheet:

  1. Every flood mask that is not all-76 is missing exactly bit 0 and nothing
     else. The port you exclude when flooding is the CPU.
  2. EVERY single-port mask in the whole table is port 0 -- all 7 of them,
     across both CmdA=1 and CmdA=3. No entry anywhere singles out any other
     port. A table of forwarding actions that can name exactly one port
     individually is naming the CPU.

  Note this evidence does NOT depend on the CmdA verb reading above, which has
  exceptions; it rests only on the masks, which have none.

  It also agrees with the port numbering derived independently from
  PARSER_INIT_FIELDS, where the three cabled front ports are 20 (Et2), 40 (Et1)
  and 41 (Et3) -- none of them port 0.

So: TRAP TO CPU is a DMT entry with CmdA=3 and ACTION_DMASK = 1<<0, with the
CPU code in ACTION_DROP_CODE. DROP is CmdA=1 with an empty mask and a reason.

⚠ WHAT THIS DOES NOT ESTABLISH.
  - The dual role of ACTION_DROP_CODE. The header names it a drop code; that it
    also serves as a CPU code on CmdA=3 entries is inferred from the value
    overlap above, not documented. An authoring path that sets it should be
    confirmed on hardware before being relied on.
  - CmdB. Zero in every populated entry EOS ships, so its meaning is untested
    and this file does not guess.
  - The 21-port mask in bank 1. It excludes all three known front ports, so it
    is some other port group -- do NOT assume it is a VLAN member list.
  - The CPU-port identification is an inference from configuration, however
    strong. The cheap confirmation is live, not static: program a CmdA=3 entry
    and see whether the frame arrives at portd.

PROVENANCE. Reads an image at runtime, embeds nothing. Field names, widths and
positions are register-header facts.

Usage:
    l2ar_action_decode.py --image <replay-or-microcode> --summary
    l2ar_action_decode.py --image <img> --dmt
    l2ar_action_decode.py --image <img> --cpu-codes
    l2ar_action_decode.py --image <img> --mirror
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import load  # noqa: E402

L2AR_BASE = 0x140000
DMT_OFF, CPU_OFF, MIR_OFF = 0x6000, 0x6200, 0x6400

DMT_BANKS, DMT_ENTRIES, DMT_WORDS = 3, 32, 3
CPU_ENTRIES = 128
MIR_BANKS, MIR_ENTRIES = 4, 128

DMASK_BITS = 76
CPU_PORT = 0

# CmdA, as read off EOS's shipped configuration. See the header.
VERB = {0: "forward", 1: "DROP", 2: "flood", 3: "redirect"}


def dmt_entry(mem, bank, ent):
    """Decode one DMT entry, or None if unpopulated."""
    w = [mem.get(L2AR_BASE + DMT_OFF + 0x80 * bank + 4 * ent + i)
         for i in range(DMT_WORDS)]
    if any(x is None for x in w) or not any(w):
        return None
    raw = w[0] | (w[1] << 32) | (w[2] << 64)
    dmask = (raw >> 20) & ((1 << DMASK_BITS) - 1)
    return {
        "drop_code": raw & 0xFF,
        "CmdA": (raw >> 8) & 0xF,
        "CmdB": (raw >> 12) & 0x7,
        "dmask": dmask,
        "ports": [i for i in range(DMASK_BITS) if (dmask >> i) & 1],
    }


def intent(e):
    """Name what an entry does, in the vocabulary A2 needs to author."""
    verb = VERB.get(e["CmdA"], f"CmdA={e['CmdA']}")
    n = len(e["ports"])
    if e["CmdA"] == 3 and e["ports"] == [CPU_PORT]:
        return f"TRAP TO CPU (code {e['drop_code']:#04x})"
    if e["CmdA"] == 1 and n == 0:
        return f"DROP (reason {e['drop_code']:#04x})"
    if e["CmdA"] == 2:
        missing = [i for i in range(DMASK_BITS) if not (e["dmask"] >> i) & 1]
        if missing == [CPU_PORT]:
            return "FLOOD (all ports except the CPU)"
        return f"flood, {n} ports"
    if n == 1:
        return f"{verb} -> port {e['ports'][0]}"
    return f"{verb}, {n} ports"


def cpu_codes(mem):
    out = {}
    for i in range(CPU_ENTRIES):
        v = mem.get(L2AR_BASE + CPU_OFF + i)
        if not v:
            continue
        codes = [(v >> (8 * k)) & 0xFF for k in range(4)]
        if any(codes):
            out[i] = codes
    return out


def mirror(mem):
    out = {}
    for bank in range(MIR_BANKS):
        for i in range(MIR_ENTRIES):
            v = mem.get(L2AR_BASE + MIR_OFF + 0x80 * bank + i)
            if not v:
                continue
            out[(bank, i)] = {
                "MIR_RX": v & 0xF, "MIR_TX": (v >> 4) & 0xF,
                "MIR_TRUNC": (v >> 8) & 0xF, "MIR_MAP_PRI": (v >> 12) & 0xF,
            }
    return out


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", required=True)
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--dmt", action="store_true")
    ap.add_argument("--cpu-codes", action="store_true")
    ap.add_argument("--mirror", action="store_true")
    a = ap.parse_args()
    mem = load(a.image)

    entries = [(b, e, dmt_entry(mem, b, e))
               for b in range(DMT_BANKS) for e in range(DMT_ENTRIES)]
    entries = [(b, e, d) for b, e, d in entries if d]

    if a.dmt or a.summary:
        print(f"=== L2AR_ACTION_DMT: {len(entries)} populated entries ===")
        for b, e, d in entries:
            print(f"  bank{b} e{e:<2d} CmdA={d['CmdA']} CmdB={d['CmdB']} "
                  f"code={d['drop_code']:#04x}  {intent(d)}")
        cpu = [(b, e) for b, e, d in entries if d["CmdA"] == 3 and d["ports"] == [CPU_PORT]]
        print(f"\n  trap-to-CPU entries: {cpu}")

    if a.cpu_codes or a.summary:
        cc = cpu_codes(mem)
        print(f"\n=== L2AR_ACTION_CPU_CODE: {len(cc)} populated of {CPU_ENTRIES} ===")
        for i, c in sorted(cc.items()):
            print(f"  [{i:3d}] " + " ".join(f"{x:02x}" for x in c))

    if a.mirror or a.summary:
        mr = mirror(mem)
        print(f"\n=== L2AR_ACTION_MIRROR: {len(mr)} populated ===")
        for (b, i), f in sorted(mr.items())[:24]:
            print(f"  bank{b} [{i:3d}] " +
                  " ".join(f"{k}={v}" for k, v in f.items() if v))

    if not (a.summary or a.dmt or a.cpu_codes or a.mirror):
        ap.error("pick one of --summary --dmt --cpu-codes --mirror")


if __name__ == "__main__":
    sys.exit(main())
