#!/usr/bin/env python3
# parser_walk.py - run the FM6000 parser program over a frame and report which
# extracted-field register receives which packet byte.
#
# parser_disasm.py --offsets can only give the offset a register MOST OFTEN
# receives across the whole program, which conflates the untagged, VLAN, QinQ,
# MPLS and tunnel paths and is therefore not an answer. This walks ONE path.
#
# Three things had to be established first, and each is checked rather than
# assumed:
#
#  1. THE KEY IS STATE + PACKET.  key byte 0..3 = State3,State2,State1,State0
#     and key byte 4..7 = packet bytes [4s .. 4s+3].
#
#  2. HIGHEST MATCHING INDEX WINS.  Rule 0 of every slice is the all-ones
#     universal default with an empty action, so lowest-index-wins would make
#     the parser do nothing. Checked on slice 0: broadcast picks rule 7,
#     01:80:c2:00 picks rule 6, 01:1b:19:00 picks rule 5, an ordinary unicast
#     picks rule 1, and an IPv4 multicast picks rule 4 on the multicast bit
#     alone -- each the semantically correct classification.
#
#  3. THE INITIAL STATE IS PER-PORT AND COMES FROM THE REPLAY.  ucode_l2.raw
#     writes PARSER_INIT_STATE as ZERO for all 76 ports; fwd4.txt then programs
#     it. Ports 20 and 40 (et2 and et1) get 0x61c70000 -> State3=0x61,
#     State2=0xc7. That matters: slice 3's IPv4/IPv6/ARP rules are guarded on
#     st3&20, and 0x61 has bit 5 set. Walking with an all-zero state instead
#     terminates at slice 3 and extracts nothing past the MAC header.
#
# ⚠ ShiftNextSlice is NOT modelled. The parse advances a flat 4 bytes per slice
# and that reproduces the whole IPv4 header correctly, so on this path shift is
# either zero or does not affect the advance. Do not assume that holds for
# tunnelled paths.
#
# ⚠ Only StateOp==1 is treated as "assign". Slice 1 uses op0 on State3 and the
# encoding of ops 0, 2 and 3 is unknown. No path here depends on them.
#
# SPDX-License-Identifier: GPL-2.0-or-later
import argparse, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from parser_disasm import read, assemble, CAM_BASE, RAM_BASE, NENTRY, NSLICE, fld

INIT_STATE = 0x108000       # [76] w=2, State0@0 State1@8 State2@16 State3@24

IPV4_TCP = (
    bytes([0x44,0x4c,0xa8,0x31,0x5d,0xab])      # DMAC
  + bytes([0x80,0xa2,0x35,0x81,0xca,0xb4])      # SMAC
  + bytes([0x08,0x00])                          # EtherType IPv4
  + bytes([0x45,0x00,0x00,0x54,0xed,0x6a,0x40,0x00,0x3f,0x06,0xd3,0x51])
  + bytes([10,101,101,33]) + bytes([10,102,1,1])
  + bytes([0x1f,0x90,0x00,0x50,0,0,0,1,0,0,0,2,0x50,0x02,0x20,0x00,0,0,0,0])
  + bytes(60))

IPV4_NAMES = {
    0:"DMAC[0:1]", 2:"DMAC[2:3]", 4:"DMAC[4:5]",
    6:"SMAC[0:1]", 8:"SMAC[2:3]", 10:"SMAC[4:5]", 12:"EtherType",
    14:"IP ver/IHL + TOS", 16:"IP total length", 18:"IP id", 20:"IP flags/frag",
    22:"IP TTL + protocol", 24:"IP checksum",
    26:"SIP[0:1]", 28:"SIP[2:3]", 30:"DIP[0:1]", 32:"DIP[2:3]",
    34:"L4 source port", 36:"L4 dest port",
}

def load_rules(w):
    R = {}
    for s in range(NSLICE):
        rs = []
        for e in range(NENTRY):
            cam, c = assemble(w, CAM_BASE, s, e)
            if not c: continue
            ram, _ = assemble(w, RAM_BASE, s, e)
            k = (cam >> 64) & ((1 << 64) - 1)
            rs.append((e, k, k ^ (cam & ((1 << 64) - 1)), ram))
        R[s] = rs
    return R

def walk(w, rules, port, pkt, verbose=True):
    a = INIT_STATE + port * 2
    if a not in w:
        print("no PARSER_INIT_STATE for port %d -- pass a file that contains it "
              "(the ucode file writes zeros; the replay programs it)" % port)
        return {}
    init = w[a]
    st = [init & 0xff, (init >> 8) & 0xff, (init >> 16) & 0xff, (init >> 24) & 0xff]
    if verbose:
        print("port %d PARSER_INIT_STATE = %08x -> State0=%02x State1=%02x "
              "State2=%02x State3=%02x\n" % (port, init, *st))
        print("%-6s %-5s %-5s %s" % ("slice", "off", "rule", "captures"))
    off, got = 0, {}
    for s in range(NSLICE):
        p = [pkt[off + i] if off + i < len(pkt) else 0 for i in range(4)]
        kv = 0
        for i, b in enumerate([st[3], st[2], st[1], st[0]]): kv |= b << (8 * (7 - i))
        for i, b in enumerate(p): kv |= b << (8 * (3 - i))
        m = [r for r in rules[s] if (kv & r[2]) == (r[1] & r[2])]
        if not m:
            if verbose: print("  %-6d %-5d NO MATCH -- parse cannot continue" % (s, off))
            break
        e, _k, _c, ram = m[-1]
        cap = []
        for hw in (0, 1):
            d = fld(ram, "Halfword%dDest" % hw)
            if d:
                o = off + 2 * hw
                got.setdefault(d, o)
                cap.append("r%-3d <- [%d] %s" % (d, o, IPV4_NAMES.get(o, "?")))
        for i in range(4):
            if fld(ram, "StateOp%d" % i) == 1: st[i] = fld(ram, "StateValue%d" % i)
        if verbose: print("  %-6d %-5d %-5d %s" % (s, off, e, "  ".join(cap) or "-"))
        if fld(ram, "Terminate"):
            if verbose: print("  TERMINATE")
            break
        off += 4
    return got

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+",
                    help="ADDR VALUE files; pass BOTH the ucode file (parser program) "
                         "and the replay (per-port PARSER_INIT_STATE)")
    ap.add_argument("--port", type=int, default=20, help="ingress port (default 20 = et2)")
    a = ap.parse_args()
    w = {}
    for f in a.files: w.update(read(f))
    got = walk(w, load_rules(w), a.port, IPV4_TCP)
    if not got: return
    print("\nextracted-field registers resolved on the untagged IPv4/TCP path:")
    for d in sorted(got):
        print("   r%-4d offset %-4d %s" % (d, got[d], IPV4_NAMES.get(got[d], "?")))

if __name__ == "__main__":
    main()
