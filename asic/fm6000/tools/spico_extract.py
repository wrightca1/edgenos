#!/usr/bin/env python3
"""spico_extract.py - recover the FM6000 SerDes SPICO firmware image from a
register trace, and analyse it.

The SPICO IMEM upload is not opaque: it is a well-defined SBus transaction
sequence, so the firmware image can be reconstructed from any capture of a
working bring-up. This is our own tool; it bundles no firmware.

Upload protocol (decoded from EOS's own trace, 2026-08-06)
----------------------------------------------------------
Every SBus transaction in a trace appears as three register writes:

    0000f002 <data>     SBUS_REQUEST  - data for the transaction
    0000f001 00000000   SBUS_COMMAND  - clear
    0000f001 <cmd>      SBUS_COMMAND  - Register[7:0] | Receiver[15:8] | Op[23:16] | Exec[24]

The SPICO sits at receiver 0xFD. The IMEM write sequence per word is:

    reg 0x04 <- addr[15:8]
    reg 0x05 <- addr[7:0]
    reg 0x07 <- data[7:0]
    reg 0x06 <- data[9:8] | 0xC     (bit3 = IMEM write enable, bit2 = strobe)
    reg 0x06 <- data[9:8] | 0x8     (strobe released)

*** The IMEM word is 10 BITS WIDE, not 16. ***
Register 0x06 carries only data[9:8] in bits [1:0]; bits 3 and 2 are control.
Confirmed empirically: every value written to reg 0x06 is in
{0x0,0x8,0x9,0xa,0xb,0xc,0xd,0xe,0xf}, i.e. the data field never exceeds 0x3.
The stock image is 6000 words at addresses 0x0000-0x176f, contiguous, and every
word is <= 0x3ff.

Usage:
    spico_extract.py <trace.txt> [-o out.bin]   reconstruct the image
    spico_extract.py --analyse <image.bin>      structural analysis
"""
import argparse
import collections
import math
import struct
import sys

SBUS_COMMAND = 0xF001
SBUS_REQUEST = 0xF002
SPICO_RECEIVER = 0xFD


def sbus_transactions(path):
    """Yield (receiver, register, op, data) for each executed SBus transaction."""
    pending = None
    with open(path) as fh:
        for line in fh:
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                addr = int(parts[0], 16)
                val = int(parts[1], 16)
            except ValueError:
                continue
            if addr == SBUS_REQUEST:
                pending = val
            elif addr == SBUS_COMMAND and (val >> 24) & 1:
                yield (val >> 8) & 0xFF, val & 0xFF, (val >> 16) & 0xFF, pending


def extract(path):
    """Replay the IMEM upload state machine and return the image as a word list."""
    addr_hi = addr_lo = data_lo = 0
    image = {}
    for receiver, reg, _op, data in sbus_transactions(path):
        if receiver != SPICO_RECEIVER:
            continue
        if reg == 0x04:
            addr_hi = data
        elif reg == 0x05:
            addr_lo = data
        elif reg == 0x07:
            data_lo = data
        elif reg == 0x06 and (data & 0x4):          # strobe -> commit
            image[(addr_hi << 8) | addr_lo] = ((data & 0x3) << 8) | data_lo

    if not image:
        sys.exit("no SPICO IMEM writes found in the trace")
    top = max(image)
    missing = [i for i in range(top + 1) if i not in image]
    if missing:
        print(f"warning: {len(missing)} gaps in the image (first at 0x{missing[0]:04x})",
              file=sys.stderr)
    return [image.get(i, 0) for i in range(top + 1)]


def entropy(values):
    counts = collections.Counter(values)
    n = len(values)
    return -sum((c / n) * math.log2(c / n) for c in counts.values()) if n else 0.0


def analyse(words):
    print(f"words          : {len(words)}")
    print(f"value range    : 0x{min(words):03x}-0x{max(words):03x}"
          f"  ({'10-bit OK' if max(words) <= 0x3FF else 'NOT 10-bit!'})")
    print(f"distinct values: {len(set(words))} of 1024 possible")
    print(f"entropy        : {entropy(words):.2f} bits/word\n")

    print("most common words:")
    for val, n in collections.Counter(words).most_common(12):
        print(f"  0x{val:03x}  {n:5d}  {100 * n / len(words):5.2f}%")

    print("\nper-position entropy (a low-entropy slot implies fixed-width instructions):")
    for k in range(1, 6):
        slots = "  ".join(f"s{i}={entropy(words[i::k]):.2f}" for i in range(k))
        print(f"  stride {k}: {slots}")

    print("\nsuccessor entropy for frequent words")
    print("(low H = fixed idiom/pair; high H = operand or immediate follows):")
    succ = collections.defaultdict(list)
    for a, b in zip(words, words[1:]):
        succ[a].append(b)
    for val, n in collections.Counter(words).most_common(12):
        top = collections.Counter(succ[val]).most_common(3)
        pretty = " ".join(f"{a:03x}x{c}" for a, c in top)
        print(f"  0x{val:03x} n={n:<5d} H(next)={entropy(succ[val]):5.2f}  {pretty}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="register trace, or image with --analyse")
    ap.add_argument("-o", "--out", help="write the reconstructed image here")
    ap.add_argument("--analyse", action="store_true", help="analyse an existing image")
    args = ap.parse_args()

    if args.analyse:
        blob = open(args.input, "rb").read()
        words = list(struct.unpack("<%dH" % (len(blob) // 2), blob))
    else:
        words = extract(args.input)
        print(f"recovered {len(words)} words "
              f"(0x0000-0x{len(words) - 1:04x}) from {args.input}", file=sys.stderr)
        if args.out:
            with open(args.out, "wb") as fh:
                fh.write(struct.pack("<%dH" % len(words), *words))
            print(f"wrote {args.out}", file=sys.stderr)

    analyse(words)


if __name__ == "__main__":
    main()
