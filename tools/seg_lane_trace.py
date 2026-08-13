#!/usr/bin/env python3
"""Segment a live FM6000 register trace to one SerDes lane, as a fm6000_lanelink op table.

    seg_lane_trace.py <trace> [sbus_dev] [epl_base] > table.inc

A capture taken while one port is brought up (or tuned) also contains every other
port's traffic in the same window. An op belongs to the lane under test if:

  - it is an SBus op whose device is the lane's own SerDes (EPL14 lane 1 = 0x4a), or
  - it is a SPICO broadcast op (dev 0xfd) and the most recent `dev=0xfd reg=0x03`
    write named that device in its DATA -- the broadcast device never changes, its
    reg-0x03 payload does, or
  - it is an MMIO write inside the lane's own EPL window, base .. base+0x7f.

Everything else belongs to another port. Emitted offsets are relative to the EPL
base, so fm6000_lanelink can retarget the table to any lane.

VALIDATION. Run over the Ethernet3 no-shut capture with the defaults, this
reproduces the 168-op SEQ[] in asic/fm6000/fm6000_lanelink.c byte-for-byte. Do
that before trusting it on any new trace -- a segmentation rule that is subtly
wrong still produces a plausible-looking table.

Two input formats are accepted, detected per line:

    Write: 0x0000f002 <- 0x0000000a      the tracer's format
    0000f002 0000000a                    the replay's format (fwd4.txt)

The second matters because the replay is where a lane's *initial* provisioning
lives. A no-shut capture is only the delta applied to an already-provisioned
lane; segmenting the replay gives the provisioning itself.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import collections
import re
import sys

SB_CMD, SB_REQ = 0x0F001, 0x0F002
SPICO_BC = 0xFD

LINE = re.compile(r'Write:\s*0x([0-9a-fA-F]+)\s*<-\s*0x([0-9a-fA-F]+)')
PLAIN = re.compile(r'^\s*([0-9a-fA-F]{4,8})\s+([0-9a-fA-F]{1,8})\s*$')


def parse(path):
    """Fold the (data, 0, cmd) triplets back into SBus ops; keep MMIO in order."""
    raw = []
    with open(path) as fh:
        for ln in fh:
            m = LINE.search(ln) or PLAIN.match(ln)
            if m:
                raw.append((int(m.group(1), 16), int(m.group(2), 16)))

    ops, data = [], 0
    for addr, val in raw:
        if addr == SB_REQ:
            data = val
        elif addr == SB_CMD:
            if val == 0:                       # the clear preceding every command
                continue
            ops.append(('SBUS', (val >> 16) & 0xFF, data, val & 0xFF, (val >> 8) & 0xFF))
        else:
            ops.append(('MMIO', addr, val, 0, 0))
    return len(raw), ops


def segment(ops, dev, base):
    kept, target, stats = [], None, collections.Counter()
    for kind, a, val, reg, d in ops:
        if kind == 'MMIO':
            if base <= a <= base + 0x7F:
                kept.append(('MMIO', a - base, val, 0, 0))
                stats['mmio lane'] += 1
            else:
                stats['mmio other'] += 1
        elif d == SPICO_BC:
            if reg == 0x03:
                target = val                   # names the device the block targets
            if target == dev:
                kept.append(('SBUS', a, val, reg, d))
                stats['spico ours'] += 1
            else:
                stats['spico other (0x%02x)' % (0xFF if target is None else target)] += 1
        elif d == dev:
            kept.append(('SBUS', a, val, reg, d))
            stats['sbus lane'] += 1
        else:
            stats['sbus other (0x%02x)' % d] += 1
    return kept, stats


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    dev = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x4A
    base = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0xE3880

    nraw, ops = parse(path)
    kept, stats = segment(ops, dev, base)

    sys.stderr.write('%s: %d writes -> %d ops; kept %d for dev 0x%02x base 0x%05x\n'
                     % (path, nraw, len(ops), len(kept), dev, base))
    for k, v in sorted(stats.items()):
        sys.stderr.write('  %-24s %d\n' % (k, v))
    sys.stderr.write('⚠ a capture disarmed mid-block leaves a partial SPICO interrupt at the\n'
                     '  end (reg 0x01/0x02 with no reg-0x03 target). Check the tail by hand.\n')

    for kind, a, val, reg, d in kept:
        if kind == 'MMIO':
            print('\t{ OP_MMIO, 0x%02x, 0x%08xu, 0x00, 0x00 },' % (a, val))
        else:
            print('\t{ OP_SBUS, 0x%02x, 0x%08xu, 0x%02x, 0x%02x },' % (a, val, reg, d))


if __name__ == '__main__':
    main()
