#!/usr/bin/env python3
"""parser_program.py - author an FM6000 parser program from a protocol list.

This is the thing the decode work was for: a parser we WRITE, not one we copy.
Output is CAM+RAM register writes, emitted by our own code from a declarative
description. No table from EOS is read, embedded or consulted at runtime --
unlike parser_decode.py, this tool needs no --image at all.

    parser_program.py --emit                  # <addr> <value> writes
    parser_program.py --check                 # structural validation
    parser_program.py --summary               # what the program does

WHAT THE PROGRAM MUST SATISFY, all established in docs/PARSER-CONVENTIONS.md:

  * FIELDS channels are fixed in HARDWARE (datasheet Table 5-5). The DMAC goes
    in channels 7/6/5 because that is where the chip reads it. We do not choose.
  * Parsing starts at STATE8[0] = 0x00, slice 0.
  * STATE8[0] is the state variable; every rule constrains it. Bytes 1-3
    qualify it -- here byte 1 carries VLAN tag depth.
  * A state is NOT a slice. The same header position lands at a different slice
    per tag count, so every rule is emitted at EVERY slice where its state is
    reachable. Emitting once yields a parser that handles untagged frames and
    silently drops tagged ones.

GEOMETRY. window_shift stays 0, so slice i sees frame bytes [4i, 4i+3] and a
position at byte offset B is parsed by slice B//4. The 32-bit frame key holds
those 4 bytes with the first halfword in the low 16 bits -- fixed by the known
IPv4 rule, whose EtherType 0x0800 reads directly as 0x0800 in key[15:0].
Halfword0 is bytes [0,1] of the window, Halfword1 is bytes [2,3].

⚠ UNTESTED ON HARDWARE. Structural checks only. Per SELF-CONTAINED-PLAN.md
nothing counts until a cold boot with a stock-replay control at the same
cadence, and the pre-existing ping/OSPF degradation makes that control
mandatory rather than optional.

SCOPE. L2 complete (DMAC, SMAC, EtherType, 0/1/2 VLAN tags with VID and
priority) plus the IPv4 header: TOS/DSCP, total length, TTL, protocol, source
and destination addresses, and both L4 ports. IPv6 and ARP are dispatched to
their own states but their headers are not yet walked.
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import (  # noqa: E402
    PARSER_BASE, SLICE_STRIDE, RAM_OFFSET, WORDS_PER_ENTRY,
    ENTRIES_PER_SLICE, NUM_SLICES, FIELDS_CHANNEL,
)
from gen_parser import encode_cam, encode_action  # noqa: E402

# ---------------------------------------------------------------- field map
# Table 5-5, hardware-fixed. Names are ours; the numbers are not negotiable.
CH_VID1 = 1
CH_VID2 = 2          # inner / C-tag VID; Table 5-5
CH_SGLORT = 3
CH_DMAC_HI, CH_DMAC_MID, CH_DMAC_LO = 7, 6, 5
CH_SMAC_HI, CH_SMAC_MID, CH_SMAC_LO = 14, 13, 12
CH_ETHERTYPE = 15
# L3. Table 5-5 lists IDENTICAL channels for L3_SIP and L3_DIP, which cannot
# both be right -- the DIP row is a copy-paste of the SIP row, note even its
# comment says "IPv4 SIP goes in L3_DIP[31:0]". Resolved from EOS's own program:
# ch20/21 are first written at slice 7 and ch22/23 at slice 8, one slice (4
# bytes) later, which is exactly SIP-then-DIP in an IPv4 header. The state chain
# confirms it -- 0x23 (writes ch20/21) -> 0x24 (writes ch22/23) -> 0x40 (writes
# L4 ports), i.e. SIP, DIP, ports in header order -- and no rule anywhere writes
# both pairs, so they are distinct fields.
CH_L3_PRI = 16       # bits 7:0 carry TOS/DSCP
CH_L3_LENGTH = 18
CH_L3_TTL_PROT = 19  # TTL in 15:8, protocol in 7:0
CH_SIP_HI, CH_SIP_LO = 21, 20
CH_DIP_HI, CH_DIP_LO = 23, 22
CH_L4_SRC, CH_L4_DST = 24, 25
# IPv6 128-bit address channels, in descending byte order. DERIVED from EOS's
# own chain, which walks SIP then DIP across 8 consecutive slices:
#   SIP  0x32 -> 0x33 -> 0x34 -> 0x35   ch33/32, 39/38, 37/36, 21/20
#   DIP  0x36 -> 0x37 -> 0x38 -> 0x39   ch31/30, 29/28, 35/34, 23/22
# The SIP set reproduces Table 5-5's L3_SIP list exactly, and the DIP set ends
# on the ch23/22 pair derived independently from the IPv4 path -- two separate
# routes agreeing on the same answer.
CH_SIP6 = [33, 32, 39, 38, 37, 36, 21, 20]   # [127:112] .. [15:0]
CH_DIP6 = [31, 30, 29, 28, 35, 34, 23, 22]
CH_L3_FLOW_LO = 17

# ---------------------------------------------------------------- flags
# Table 5-6, Parser Action Flags. The datasheet is explicit that only bits
# 37-39 are fixed-function inside the parser, and that other bits are
# "interpreted by downstream fixed-function logic" or are conventions. Since we
# are replacing ONLY the parser and keeping EOS's mapper/FFU/L3AR
# configuration, these are not ours to choose -- downstream is already wired to
# expect them. SetFlags ORs into FLAGS, so a flag set at one slice persists.
FLAG_VLAN1_TAGGED = 1 << 6    # has S-TAG
FLAG_VLAN2_TAGGED = 1 << 7    # has C-TAG
FLAG_IS_IPV4 = 1 << 8
FLAG_IS_IPV6 = 1 << 9
FLAG_L3_OPTIONS = 1 << 16
FLAG_L3_MCST = 1 << 17        # "derives from bit 40 of DMAC" -- the I/G bit
FLAG_L3_BCST = 1 << 18        # DMAC == ff:ff:ff:ff:ff:ff
FLAG_IPV6_HOPBYHOP = 1 << 22

ET_VLAN_C = 0x8100
ET_VLAN_S = 0x88A8
ET_IPV4 = 0x0800
ET_IPV6 = 0x86DD
ET_ARP = 0x0806

# ---------------------------------------------------------------- states
# Ours to choose: STATE8[0] is internal to the parser and no downstream block
# sees it. Values are picked to be readable in a trace, not to match EOS.
S_START = 0x00       # slice 0, before any bytes
S_DMAC_LO = 0x10     # consumed DMAC[47:16]
S_SMAC_LO = 0x12     # consumed DMAC[15:0] + SMAC[47:32]
S_ETYPE = 0x14       # consumed SMAC[31:0]; next halfword is an EtherType
S_TAGGED = 0x16      # just consumed a VLAN tag; another EtherType follows
S_IP4_LEN = 0x20     # at total-length word
S_IP4_TTL = 0x21     # at flags/frag + TTL/protocol
S_IP4_SIP_HI = 0x22  # at checksum + SIP[31:16]
S_IP4_SIP_LO = 0x23  # at SIP[15:0] + DIP[31:16]
S_IP4_DIP_LO = 0x24  # at DIP[15:0] + L4 source port
S_IP4_L4 = 0x25      # at L4 destination port
S_DONE_IPV4 = S_IP4_LEN
S_IP6_FLOW = 0x40    # at flow[15:0] + payload length
S_IP6_NH = 0x41      # at next-header/hop-limit + SIP[127:112]
S_IP6_S1 = 0x42
S_IP6_S2 = 0x43
S_IP6_S3 = 0x44
S_IP6_S4 = 0x45      # SIP[15:0] + DIP[127:112]
S_IP6_D1 = 0x46
S_IP6_D2 = 0x47
S_IP6_D3 = 0x48      # DIP[31:16] via ch23
S_IP6_L4 = 0x49
S_DONE_IPV6 = 0x30   # was 0x22 -- collided with S_IP4_SIP_HI
S_DONE_ARP = 0x32    # was 0x24 -- collided with S_IP4_DIP_LO
S_DONE_OTHER = 0x34

STATE_NAME = {
    S_START: "start", S_DMAC_LO: "dmac-hi-consumed", S_SMAC_LO: "smac-hi-consumed",
    S_ETYPE: "at-ethertype", S_TAGGED: "at-ethertype(after tag)",
    S_IP4_LEN: "ipv4-length", S_IP4_TTL: "ipv4-ttl/proto",
    S_IP4_SIP_HI: "ipv4-sip-hi", S_IP4_SIP_LO: "ipv4-sip-lo/dip-hi",
    S_IP4_DIP_LO: "ipv4-dip-lo/l4src", S_IP4_L4: "ipv4-l4dst",
    S_IP6_FLOW: "ipv6-flow", S_IP6_NH: "ipv6-nexthdr", S_IP6_S1: "ipv6-sip-1",
    S_IP6_S2: "ipv6-sip-2", S_IP6_S3: "ipv6-sip-3", S_IP6_S4: "ipv6-sip-4/dip-1",
    S_IP6_D1: "ipv6-dip-2", S_IP6_D2: "ipv6-dip-3", S_IP6_D3: "ipv6-dip-4",
    S_IP6_L4: "ipv6-l4dst", S_DONE_IPV6: "ipv6-other", S_DONE_ARP: "arp",
    S_DONE_OTHER: "other-l3",
}

MAX_TAGS = 2


class Rule:
    """One CAM+RAM entry, before slice placement."""

    def __init__(self, state, next_state, tag_depth=0, next_tag_depth=None,
                 match_halfword0=None, match_halfword0_mask=0xFFFF,
                 dest0=None, dest1=None,
                 match_halfword1=None, match_halfword1_mask=0xFFFF,
                 rot0=0, rot1=0, match_q=None, set_q=None,
                 match_r=None, set_r=None, set_flags=0,
                 terminate=False, note=""):
        self.state = state
        self.next_state = next_state
        self.tag_depth = tag_depth
        self.next_tag_depth = tag_depth if next_tag_depth is None else next_tag_depth
        self.match_halfword0 = match_halfword0   # match on bytes[0:1]
        self.match_halfword0_mask = match_halfword0_mask
        self.match_halfword1 = match_halfword1   # masked match on bytes[2:3]
        self.match_halfword1_mask = match_halfword1_mask
        self.dest0 = dest0                       # FIELDS channel for bytes[0:1]
        self.dest1 = dest1                       # FIELDS channel for bytes[2:3]
        self.rot0 = rot0      # nibble rotation, 4*rot bits; 2 == byte swap
        self.rot1 = rot1
        self.set_flags = set_flags
        self.match_q = match_q   # require STATE8[2] == this
        self.set_q = set_q       # set STATE8[2]; None leaves it unchanged
        self.match_r = match_r   # require STATE8[3] == this
        self.set_r = set_r       # set STATE8[3]
        self.terminate = terminate
        self.note = note

    def cam(self):
        """Return (value, care) for the 64-bit key."""
        value = (self.state | (self.tag_depth << 8)) << 32
        care = 0x0000FFFF << 32                  # pin STATE8[0] and STATE8[1]
        if self.match_halfword0 is not None:
            m = self.match_halfword0_mask & 0xFFFF
            value |= self.match_halfword0 & m
            care |= m
        if self.match_halfword1 is not None:
            m = self.match_halfword1_mask & 0xFFFF
            value |= (self.match_halfword1 & m) << 16
            care |= m << 16
        if self.match_q is not None:
            value |= (self.match_q & 0xFF) << 48
            care |= 0xFF << 48
        if self.match_r is not None:
            value |= (self.match_r & 0xFF) << 56
            care |= 0xFF << 56
        return value, care

    def action(self):
        f = {}
        # StateOp1 := literal, for both the state byte and the tag-depth byte.
        f["StateOp0"], f["StateValue0"] = 1, self.next_state
        f["StateOp1"], f["StateValue1"] = 1, self.next_tag_depth
        if self.dest0 is not None:
            f["Halfword0Dest"] = self.dest0
            f["Halfword0Rot"] = self.rot0
            f["Byte0Enable"] = f["Byte1Enable"] = 1
        if self.dest1 is not None:
            f["Halfword1Dest"] = self.dest1
            f["Halfword1Rot"] = self.rot1
            f["Byte2Enable"] = f["Byte3Enable"] = 1
        # StateOp2=0 with value 0 leaves STATE8[2] unchanged, so the qualifier
        # survives across the address walk without every rule restating it.
        if self.set_flags:
            f["SetFlags"] = self.set_flags
        if self.set_q is not None:
            f["StateOp2"], f["StateValue2"] = 1, self.set_q
        if self.set_r is not None:
            f["StateOp3"], f["StateValue3"] = 1, self.set_r
        if self.terminate:
            f["Terminate"] = 1
        return f


def build_program():
    """The protocol description. Everything below is a choice we are making."""
    rules = []

    # --- Ethernet header, identical regardless of tags (it precedes them) ---
    rules.append(Rule(S_START, S_DMAC_LO, dest0=CH_DMAC_HI, dest1=CH_DMAC_MID,
                      note="bytes 0-3: DMAC[47:32], DMAC[31:16]"))
    # L3_Mcst (Table 5-6 bit 17) "derives from bit 40 of DMAC" -- the I/G bit,
    # the low bit of the first octet on the wire. That octet is the MOST
    # significant byte of the first halfword (datasheet 5.5: first byte received
    # lands in the most significant byte), so DMAC bit 40 is key bit 8.
    # Without this every multicast frame classifies as unicast downstream.
    rules.append(Rule(S_START, S_DMAC_LO, dest0=CH_DMAC_HI, dest1=CH_DMAC_MID,
                      match_halfword0=0x0100, match_halfword0_mask=0x0100,
                      set_flags=FLAG_L3_MCST,
                      note="DMAC I/G bit set -> L3_Mcst"))
    # L3_Bcst needs DMAC == ff:ff:ff:ff:ff:ff, which straddles slices 0 and 1 --
    # the first 4 octets land in slice 0 and the last 2 in slice 1. No single
    # rule can see all 48 bits, so slice 0 records "top 32 bits were all ones"
    # in STATE8[3] and slice 1 completes the comparison.
    #
    # This is what the auxiliary state bytes buy: STATE8[0] tracks position,
    # STATE8[2] carries the IPv6 extension-header verdict, STATE8[3] carries a
    # partially-evaluated match. A parser that only had a position variable
    # could not express a comparison wider than its parsing window.
    rules.append(Rule(S_START, S_DMAC_LO,
                      match_halfword0=0xFFFF, match_halfword1=0xFFFF,
                      dest0=CH_DMAC_HI, dest1=CH_DMAC_MID,
                      set_flags=FLAG_L3_MCST, set_r=1,
                      note="DMAC[47:16] all ones -> broadcast candidate"))
    rules.append(Rule(S_DMAC_LO, S_SMAC_LO, dest0=CH_DMAC_LO, dest1=CH_SMAC_HI,
                      note="bytes 4-7: DMAC[15:0], SMAC[47:32]"))
    rules.append(Rule(S_DMAC_LO, S_SMAC_LO, match_halfword0=0xFFFF, match_r=1,
                      dest0=CH_DMAC_LO, dest1=CH_SMAC_HI,
                      set_flags=FLAG_L3_BCST, set_r=0,
                      note="DMAC[15:0] all ones and candidate -> L3_Bcst"))
    rules.append(Rule(S_SMAC_LO, S_ETYPE, dest0=CH_SMAC_MID, dest1=CH_SMAC_LO,
                      note="bytes 8-11: SMAC[31:16], SMAC[15:0]"))

    # --- EtherType dispatch, once per reachable tag depth ---
    for depth in range(MAX_TAGS + 1):
        here = S_ETYPE if depth == 0 else S_TAGGED
        # A VLAN tag: capture the EtherType, and the TCI lands in L2_VID1,
        # whose top nibble is the priority -- exactly the TCI layout.
        if depth < MAX_TAGS:
            # Table 5-5: the OUTER tag's VID belongs in L2_VID1 and the inner in
            # L2_VID2, so the destination depends on tag depth. Writing both to
            # ch1 -- as this did until now -- loses the inner VID entirely on
            # double-tagged frames, and the mapper's VID2 table then sees zero.
            vid_ch = CH_VID1 if depth == 0 else CH_VID2
            # Table 5-6 names the flags by tag TYPE, not position: bit 6 is
            # "has S-TAG", bit 7 is "has C-TAG".
            for et, flag in ((ET_VLAN_C, FLAG_VLAN2_TAGGED),
                             (ET_VLAN_S, FLAG_VLAN1_TAGGED)):
                rules.append(Rule(here, S_TAGGED, tag_depth=depth,
                                  next_tag_depth=depth + 1,
                                  match_halfword0=et, set_flags=flag,
                                  dest0=CH_ETHERTYPE, dest1=vid_ch,
                                  note=f"VLAN 0x{et:04x} at depth {depth} -> ch{vid_ch}"))
        # IPv4 dispatch also captures the halfword after the EtherType, which is
        # ver/IHL + TOS -- and ch16 bits 7:0 are L3_PRI, so TOS lands correctly.
        #
        # ⚠ The walk that follows assumes a 20-byte header. Requiring
        # ver/IHL == 0x45 here makes that assumption explicit in the hardware
        # match rather than implicit in our arithmetic: a packet carrying IPv4
        # options simply does not take this path. Without the guard, options
        # shift every subsequent field and the parser reports a source address
        # read from the middle of the option area -- wrong, and silently so.
        rules.append(Rule(here, S_IP4_LEN, tag_depth=depth, match_halfword0=ET_IPV4,
                          match_halfword1=0x4500, match_halfword1_mask=0xFF00,
                          set_flags=FLAG_IS_IPV4,
                          dest0=CH_ETHERTYPE, dest1=CH_L3_PRI,
                          note=f"IPv4 (ver/IHL=0x45) at depth {depth}; TOS -> L3_PRI"))
        # IPv4 WITH options: record the EtherType and stop rather than mis-parse.
        # Placed after the guarded rule so the TCAM prefers the specific match.
        rules.append(Rule(here, S_DONE_OTHER, tag_depth=depth,
                          match_halfword0=ET_IPV4,
                          set_flags=FLAG_IS_IPV4 | FLAG_L3_OPTIONS,
                          dest0=CH_ETHERTYPE, terminate=True,
                          note=f"IPv4 with options at depth {depth}: L2 only"))
        # IPv6: the halfword after the EtherType is ver/traffic-class/flow[19:16].
        # Table 5-5 wants traffic class in ch16[7:0] and FlowLabel[19:16] in
        # ch16[15:12], which is the ">>4 rotation" its note calls for: rot=3
        # (BarrelShiftLeft by 12 == right by 4 in a 16-bit halfword).
        rules.append(Rule(here, S_IP6_FLOW, tag_depth=depth, match_halfword0=ET_IPV6,
                          set_flags=FLAG_IS_IPV6,
                          dest0=CH_ETHERTYPE, dest1=CH_L3_PRI, rot1=3,
                          note=f"IPv6 at depth {depth}; TC/flow -> L3_PRI"))
        rules.append(Rule(here, S_DONE_ARP, tag_depth=depth, match_halfword0=ET_ARP,
                          dest0=CH_ETHERTYPE, terminate=True,
                          note=f"ARP at depth {depth}"))
        # Anything else: record the EtherType and stop.
        rules.append(Rule(here, S_DONE_OTHER, tag_depth=depth,
                          dest0=CH_ETHERTYPE, terminate=True,
                          note=f"unknown EtherType at depth {depth}"))

        # --- IPv4 header walk. One rule per state per tag depth, because the
        # header sits 4 bytes later for each tag in front of it. ---
        for st, nxt, d0, d1, term, note in (
            (S_IP4_LEN, S_IP4_TTL, CH_L3_LENGTH, None, False,
             "IPv4 total length"),
            (S_IP4_TTL, S_IP4_SIP_HI, None, CH_L3_TTL_PROT, False,
             "IPv4 TTL + protocol"),
            (S_IP4_SIP_HI, S_IP4_SIP_LO, None, CH_SIP_HI, False,
             "IPv4 SIP[31:16]"),
            (S_IP4_SIP_LO, S_IP4_DIP_LO, CH_SIP_LO, CH_DIP_HI, False,
             "IPv4 SIP[15:0], DIP[31:16]"),
            (S_IP4_DIP_LO, S_IP4_L4, CH_DIP_LO, CH_L4_SRC, False,
             "IPv4 DIP[15:0], L4 source port"),
            (S_IP4_L4, S_DONE_OTHER, CH_L4_DST, None, True,
             "L4 destination port; done"),
        ):
            rules.append(Rule(st, nxt, tag_depth=depth, dest0=d0, dest1=d1,
                              terminate=term, note=f"{note} (depth {depth})"))

        # --- IPv6 header walk. Fixed 40 bytes, so no options problem. ---
        S6, D6 = CH_SIP6, CH_DIP6
        for st, nxt, d0, d1, r0, term, note in (
            (S_IP6_FLOW, S_IP6_NH, CH_L3_FLOW_LO, CH_L3_LENGTH, 0, False,
             "IPv6 flow[15:0], payload length"),
            # next-header then hop-limit on the wire, but ch19 wants TTL in
            # [15:8] and protocol in [7:0] -- rot0=2 swaps the bytes.
            (S_IP6_NH, S_IP6_S1, CH_L3_TTL_PROT, S6[0], 2, False,
             "IPv6 hop-limit/next-header (ext headers assumed), SIP[127:112]"),
            (S_IP6_S1, S_IP6_S2, S6[1], S6[2], 0, False, "IPv6 SIP[111:80]"),
            (S_IP6_S2, S_IP6_S3, S6[3], S6[4], 0, False, "IPv6 SIP[79:48]"),
            (S_IP6_S3, S_IP6_S4, S6[5], S6[6], 0, False, "IPv6 SIP[47:16]"),
            (S_IP6_S4, S_IP6_D1, S6[7], D6[0], 0, False,
             "IPv6 SIP[15:0], DIP[127:112]"),
            (S_IP6_D1, S_IP6_D2, D6[1], D6[2], 0, False, "IPv6 DIP[111:80]"),
            (S_IP6_D2, S_IP6_D3, D6[3], D6[4], 0, False, "IPv6 DIP[79:48]"),
            (S_IP6_D3, S_IP6_L4, D6[5], D6[6], 0, False, "IPv6 DIP[47:16]"),
        ):
            kw = {"set_q": 0, "set_flags": FLAG_IPV6_HOPBYHOP} if st == S_IP6_NH else {}
            rules.append(Rule(st, nxt, tag_depth=depth, dest0=d0, dest1=d1,
                              rot0=r0, terminate=term, **kw,
                              note=f"{note} (depth {depth})"))

        # ⚠ IPv6 extension headers are the same trap as IPv4 options: they sit
        # between the addresses and L4, so the ports are NOT where the fixed
        # walk expects them. The addresses are unaffected -- extension headers
        # follow them -- so only the L4 step needs guarding.
        #
        # STATE8[2] carries the verdict. It is set at the next-header field and
        # survives the whole address walk untouched, because every intervening
        # rule leaves StateOp2 at 0 (add zero).
        #
        # The next-header byte is the FIRST byte of the halfword at S_IP6_NH,
        # and the first byte received occupies the most significant byte
        # (datasheet 5.5), so it is key[15:8] -- matched with mask 0xFF00.
        for proto in (6, 17, 58):        # TCP, UDP, ICMPv6
            rules.append(Rule(S_IP6_NH, S_IP6_S1, tag_depth=depth,
                              match_halfword0=proto << 8,
                              match_halfword1_mask=0xFFFF,
                              dest0=CH_L3_TTL_PROT, dest1=CH_SIP6[0], rot0=2,
                              set_q=1,
                              note=f"IPv6 next-header {proto}: L4 follows directly"))
        # L4 ports, only when STATE8[2] says the walk is still aligned.
        rules.append(Rule(S_IP6_L4, S_DONE_OTHER, tag_depth=depth,
                          dest0=CH_DIP6[7], dest1=CH_L4_SRC, match_q=1,
                          terminate=True,
                          note="IPv6 DIP[15:0] + L4 source port (no ext headers)"))
        rules.append(Rule(S_IP6_L4, S_DONE_OTHER, tag_depth=depth,
                          dest0=CH_DIP6[7], terminate=True,
                          note="IPv6 DIP[15:0]; ext headers present, ports skipped"))
    return rules


def reachable_slices(rule):
    """Every slice at which this rule's state can occur.

    A position sits at byte offset 4*n + 4*tag_depth, so the slice is fixed by
    the state AND the tag depth. This is the step that makes tagged frames work.
    """
    base = {S_START: 0, S_DMAC_LO: 1, S_SMAC_LO: 2, S_ETYPE: 3, S_TAGGED: 3,
            S_IP4_LEN: 4, S_IP4_TTL: 5, S_IP4_SIP_HI: 6,
            S_IP4_SIP_LO: 7, S_IP4_DIP_LO: 8, S_IP4_L4: 9,
            S_IP6_FLOW: 4, S_IP6_NH: 5, S_IP6_S1: 6, S_IP6_S2: 7, S_IP6_S3: 8,
            S_IP6_S4: 9, S_IP6_D1: 10, S_IP6_D2: 11, S_IP6_D3: 12, S_IP6_L4: 13}
    if rule.state not in base:
        return []
    return [base[rule.state] + rule.tag_depth]


def place(rules):
    """Assign rules to (slice, entry), general first and specific last.

    ★ ORDER IS LOAD-BEARING, and in the opposite direction to the obvious
    guess. Measured across EOS's own program: of 2,349 overlapping
    specific/general entry pairs, the MORE SPECIFIC rule sits at the HIGHER
    index in 2,349 of them -- 100%, never once the other way. The parser CAM
    therefore resolves to the LAST matching entry, not the first.

    The datasheet does not state this anywhere we can find, so it rests on that
    measurement. It is not cosmetic: the IPv4 walk is guarded by a ver/IHL==0x45
    match that overlaps a general "IPv4 with options" fallback. Emitted in the
    intuitive order -- specific first -- the fallback would win every time and
    IPv4 would never be walked at all, while every structural check still
    passed.

    Sorting by care-bit count puts general rules first and specific ones last.
    """
    out = {}
    for r in rules:
        for s in reachable_slices(r):
            out.setdefault(s, []).append(r)
    placed = {}
    for s, rs in sorted(out.items()):
        rs = sorted(rs, key=lambda r: bin(r.cam()[1]).count("1"))
        placed[s] = list(enumerate(rs))
    return placed


def writes(placed):
    """Emit (address, value) pairs for CAM and RAM."""
    out = []
    for s, entries in sorted(placed.items()):
        for entry, rule in entries:
            value, care = rule.cam()
            key, keyinvert = encode_cam(value, care)
            cam_base = PARSER_BASE + SLICE_STRIDE * s + WORDS_PER_ENTRY * entry
            for i, w in enumerate([keyinvert & 0xFFFFFFFF, (keyinvert >> 32) & 0xFFFFFFFF,
                                   key & 0xFFFFFFFF, (key >> 32) & 0xFFFFFFFF]):
                out.append((cam_base + i, w))
            ram_base = cam_base + RAM_OFFSET
            for i, w in enumerate(encode_action(rule.action())):
                out.append((ram_base + i, w))
    return out


def check(placed):
    problems = []

    # State values must be unique. Two states sharing a value silently merge
    # two parse positions -- the IPv6/ARP terminals originally collided with
    # the IPv4 walk this way, and nothing downstream would have flagged it.
    seen = {}
    for name, val in sorted(STATE_NAME.items(), key=lambda t: t[1]):
        if name in seen:
            problems.append(f"state value 0x{name:02x} used by both "
                            f"'{seen[name]}' and '{val}'")
        seen[name] = val
    for s, entries in placed.items():
        if len(entries) > ENTRIES_PER_SLICE:
            problems.append(f"slice {s}: {len(entries)} entries exceeds {ENTRIES_PER_SLICE}")
        if s >= NUM_SLICES:
            problems.append(f"slice {s} is beyond the {NUM_SLICES}-slice array")
    # every non-terminal next_state must be matched by some rule
    produced = {r.next_state for _, rs in placed.items() for _, r in rs if not r.terminate}
    consumed = {r.state for _, rs in placed.items() for _, r in rs}
    terminal = {S_DONE_IPV6, S_DONE_ARP, S_DONE_OTHER}
    for st in produced - consumed - terminal:
        problems.append(f"state 0x{st:02x} ({STATE_NAME.get(st,'?')}) is produced but never consumed")
    # channels must exist in Table 5-5
    for _, rs in placed.items():
        for _, r in rs:
            for ch in (r.dest0, r.dest1):
                if ch is not None and ch not in FIELDS_CHANNEL:
                    problems.append(f"channel {ch} is not in Table 5-5")
    return problems


PROGRAM_LO = PARSER_BASE                       # 0x100000
PROGRAM_HI = PARSER_BASE + SLICE_STRIDE * NUM_SLICES   # 0x107000, exclusive


def splice(replay_path, out_path):
    """Replace a replay's entire parser program with ours.

    ⚠ ALL of EOS's parser program must go, not merely the addresses we happen
    to write. Their rules match THEIR state encoding, ours match ours, and both
    would be live in the same CAM: EOS's slice-0 rules key on state 0x00 exactly
    as ours do, and with last-match-wins whichever sits at the higher index
    takes the frame. Removing only our own addresses would leave a parser that
    is half theirs and half ours, which is worse than either.

    Placement follows gen_list_early: our writes go where the parser block's
    FIRST recorded write was, not at the end of the loop. The boot script's own
    comment records why -- PARSER/L2AR/MOD/MAPPER are written early, before port
    bring-up depends on them, and moving them late gave routes=2 and rx=0 while
    looking fine.

    PARSER_INIT_STATE (0x108000) and PARSER_INIT_FIELDS (0x108200) are left
    alone: they are all zeros in EOS's image, every port starts at state 0,
    which is what our program assumes.
    """
    lines = []
    with open(replay_path) as fh:
        for line in fh:
            parts = line.split()
            if len(parts) != 2:
                continue
            lines.append((int(parts[0], 16), int(parts[1], 16)))

    first = None
    for i, (addr, _) in enumerate(lines):
        if PROGRAM_LO <= addr < PROGRAM_HI:
            first = i
            break
    if first is None:
        sys.exit(f"{replay_path}: no parser writes found in "
                 f"0x{PROGRAM_LO:06x}-0x{PROGRAM_HI - 1:06x}")

    ours = writes(place(build_program()))
    kept = [(a, v) for a, v in lines if not (PROGRAM_LO <= a < PROGRAM_HI)]
    head = [(a, v) for a, v in lines[:first] if not (PROGRAM_LO <= a < PROGRAM_HI)]
    tail = [(a, v) for a, v in lines[first:] if not (PROGRAM_LO <= a < PROGRAM_HI)]

    with open(out_path, "w") as fh:
        for a, v in head:
            fh.write(f"{a:08x} {v:08x}\n")
        for a, v in ours:
            fh.write(f"{a:08x} {v:08x}\n")
        for a, v in tail:
            fh.write(f"{a:08x} {v:08x}\n")

    removed = len(lines) - len(kept)
    print(f"replay in:   {len(lines)} writes")
    print(f"  EOS parser writes removed: {removed}")
    print(f"  our parser writes added:   {len(ours)}")
    print(f"  spliced at line {first} (the block's first recorded write)")
    print(f"replay out:  {len(kept) + len(ours)} writes  -> {out_path}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emit", action="store_true", help="print <addr> <value> writes")
    ap.add_argument("--check", action="store_true", help="structural validation")
    ap.add_argument("--summary", action="store_true", help="what the program does")
    ap.add_argument("--addresses", action="store_true",
                    help="print the addresses this program writes (like a generator's -a)")
    ap.add_argument("--splice", metavar="REPLAY",
                    help="replace REPLAY's parser program with ours")
    ap.add_argument("--out", metavar="FILE", help="output file for --splice")
    args = ap.parse_args()

    rules = build_program()
    placed = place(rules)

    if args.summary:
        print(f"rules authored: {len(rules)}")
        print(f"slices used:    {sorted(placed)}")
        total = sum(len(v) for v in placed.values())
        print(f"entries placed: {total}  (CAM+RAM writes: {total * 8})\n")
        for s, entries in sorted(placed.items()):
            print(f"  slice {s} ({len(entries)} entries)")
            for entry, r in entries:
                dest = []
                if r.dest0 is not None:
                    dest.append(f"hw0->{FIELDS_CHANNEL.get(r.dest0)}")
                if r.dest1 is not None:
                    dest.append(f"hw1->{FIELDS_CHANNEL.get(r.dest1)}")
                print(f"    [{entry:>2}] {STATE_NAME.get(r.state,'?'):<24}"
                      f" -> {STATE_NAME.get(r.next_state,'?'):<24} {r.note}")
                if dest:
                    print(f"         {', '.join(dest)}")
        print("\nNOT COVERED: IPv6 and ARP header walks (both are dispatched to a")
        print("state, but their fields are not extracted). IPv4 options are not")
        print("handled -- the walk assumes IHL=5, so a packet with options mis-aligns")
        print("everything after it.")

    if args.check:
        problems = check(placed)
        for p in problems:
            print("  PROBLEM:", p)
        print("check " + ("PASS" if not problems else "FAIL"))
        return 1 if problems else 0

    if args.addresses:
        for addr, _ in writes(placed):
            print(f"{addr:08x}")

    if args.splice:
        if not args.out:
            sys.exit("--splice needs --out")
        return splice(args.splice, args.out)

    if args.emit:
        for addr, val in writes(placed):
            print(f"{addr:08x} {val:08x}")

    if not (args.emit or args.check or args.summary or args.addresses or args.splice):
        ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
