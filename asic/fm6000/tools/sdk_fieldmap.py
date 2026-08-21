#!/usr/bin/env python3
"""sdk_fieldmap.py - recover NAMED, bit-exact field layouts for the FM6000 CSRs.

sdk_regmap.py recovers each register's address, width and geometry. This recovers
what the bits inside it MEAN: 709 registers mapped to their field lists, names and
all, straight out of libFocalpointSDK.so.

★ HOW, because the obvious routes all fail.

The SDK holds two tables. The register descriptor table (56-byte stride) is easy.
The field table is 12-byte entries `{name_ptr, bit_offset, width}` in
NUL-terminated groups, and NOTHING in the data connects a group to a register:

  - the register descriptor has no field pointer (all 14 words accounted for);
  - group starts have no absolute references anywhere in the file;
  - groups are packed contiguously but NOT one per register, so walking them in
    register order desynchronises after ~44 entries.

The association lives in CODE. This is 32-bit PIC, so a data address is formed as
`lea eax,[ebx+disp]` with ebx = GOT, i.e. `8d 83 <disp32>` and disp = target-GOT.
Two mistakes cost time here and are worth recording: the displacement is
**GOT-relative, not absolute**, and instruction operands are **not 4-byte
aligned**, so an aligned scan finds nothing.

The emitting function repeats one shape per register:

    8d 83 <disp>   lea eax,[ebx+disp]   ; the register NAME string
    ... mov/mov/call ...
    8d 83 <disp>   lea eax,[ebx+disp]   ; that register's FIELD GROUP

so pairing each name-lea with the next group-lea recovers the whole mapping.

★ VALIDATION, against layouts established independently and painfully:

    L3AR_RAM4  -> SetHashProfile, POL3_IDX_PROFILE, ...   (hand-transcribed match)
    L3AR_RAM5  -> SetDestMaskCmdProfile, CSGLORT_PROFILE  (hand-transcribed match)
    L3AR_CAM   -> Key, KeyInvert

That last one is the check that matters. L3AR_CAM's Key/KeyInvert split was
originally recovered by byte-comparing our output against EOS's own rules, and
the word ORDER was wrong at first (docs/L3AR-STRUCTURE.md). The table says it
outright.

★ THE SBUS/SERDES SPACE IS COVERED TOO, by a different shape (--sbus).
`fm6000DbgGetEthWriteRegFields` is one long if-chain, and each arm names the
register by NUMBER rather than by string:

    83 7d 08 <imm8>   cmp DWORD PTR [ebp+8], <SBus register number>
    75 0b             jne next
    8d 81 <disp32>    lea eax,[ecx+disp]      ; that register's field group

so the register number falls out of the compare. 54 SBus registers recovered.

⚠ These are the registers reached by `fm6000_sbus read/write <dev> <reg>`, and
what they reveal corrects a long-standing misreading -- see docs/SPICO-RE.md and
the header of ../fm6000_serdes_enable.c.

PROVENANCE: reads the SDK at runtime and embeds nothing. Facts about the silicon,
recorded in our own words (docs/PROVENANCE.md).

usage:
    sdk_fieldmap.py --so <libFocalpointSDK.so> [--reg NAME] [--json out.json] [--check]
SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import json
import re
import struct
import sys

NAME_RE = re.compile(r'^[A-Z][A-Z0-9_]{2,}$')
LEA_EBX = b"\x8d\x83"            # lea eax,[ebx+disp32]

# Layouts established WITHOUT this table; if it disagrees, it is not the table
# we think it is.
KNOWN = {
    "L3AR_CAM":  {"Key", "KeyInvert"},
    "L3AR_RAM4": {"ALU46_OP_PROFILE", "QOS_PROFILE", "HASH_PROFILE"},
    "L3AR_RAM5": {"CSGLORT_PROFILE", "DGLORT_PROFILE", "SGLORT_PROFILE"},
}


def load(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1:
        sys.exit("not a 32-bit ELF")
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    shes = struct.unpack_from("<H", d, 0x2E)[0]
    shn = struct.unpack_from("<H", d, 0x30)[0]
    shstr = struct.unpack_from("<H", d, 0x32)[0]
    sh = [struct.unpack_from("<10I", d, shoff + i * shes) for i in range(shn)]
    stro = sh[shstr][4]

    def snm(x):
        return d[stro + x:d.find(b"\0", stro + x)].decode()

    sec = {snm(s[0]): s for s in sh}
    got = (sec.get(".got.plt") or sec[".got"])[3]
    phoff = struct.unpack_from("<I", d, 0x1C)[0]
    phes = struct.unpack_from("<H", d, 0x2A)[0]
    phn = struct.unpack_from("<H", d, 0x2C)[0]
    segs = []
    for i in range(phn):
        t, o, va, _, fs = struct.unpack_from("<IIIII", d, phoff + i * phes)
        if t == 1:
            segs.append((va, o, fs))
    return d, sec, got, segs


def scan(path):
    d, sec, got, segs = load(path)
    lo = min(s[0] for s in segs)
    hi = max(s[0] + s[2] for s in segs)

    def v2o(v):
        for va, fo, sz in segs:
            if va <= v < va + sz:
                return fo + (v - va)
        return None

    def rds(v, mx=72):
        o = v2o(v)
        if o is None:
            return None
        e = d.find(b"\0", o)
        if e < 0 or e == o or e - o > mx:
            return None
        s = d[o:e]
        return s.decode("ascii") if all(32 <= c < 127 for c in s) else None

    def group(v):
        """Read a NUL-terminated run of {name_ptr, bit, width}."""
        o = v2o(v)
        if o is None:
            return None
        out = []
        while o + 12 <= len(d) and len(out) < 64:
            a, b, c = struct.unpack_from("<3I", d, o)
            if a == 0:
                break
            if not (lo <= a < hi):
                return None
            s = rds(a, 64)
            if not s or b >= 512 or not (0 < c <= 64):
                return None
            out.append((s, b, c))
            o += 12
        return out or None

    text = sec[".text"]
    tlo, thi = text[4], text[4] + text[5]
    seq, i = [], tlo
    while i < thi - 6:
        if d[i:i + 2] == LEA_EBX:
            v = (got + struct.unpack_from("<i", d, i + 2)[0]) & 0xFFFFFFFF
            s = rds(v)
            if s and NAME_RE.match(s):
                seq.append(("name", s))
            else:
                g = group(v)
                if g:
                    seq.append(("group", g))
            i += 6
            continue
        i += 1

    m = {}
    for k, (t, val) in enumerate(seq):
        if t != "name":
            continue
        # the register's own group is the next group emitted in the block
        for t2, val2 in seq[k + 1:k + 4]:
            if t2 == "group":
                m.setdefault(val, val2)
                break
    return m


def scan_sbus(path):
    """SBus registers, keyed by NUMBER, from the if-chain in
    fm6000DbgGetEthWriteRegFields (see the module docstring)."""
    d, sec, got, segs = load(path)
    lo = min(x[0] for x in segs)
    hi = max(x[0] + x[2] for x in segs)

    def v2o(v):
        for va, fo, sz in segs:
            if va <= v < va + sz:
                return fo + (v - va)
        return None

    def rds(v, mx=64):
        o = v2o(v)
        if o is None:
            return None
        e = d.find(b"\0", o)
        if e < 0 or e == o or e - o > mx:
            return None
        t = d[o:e]
        return t.decode("ascii") if all(32 <= c < 127 for c in t) else None

    def group(v):
        o = v2o(v)
        if o is None:
            return None
        out = []
        while o + 12 <= len(d) and len(out) < 64:
            a, b, c = struct.unpack_from("<3I", d, o)
            if a == 0:
                break
            if not (lo <= a < hi):
                return None
            t = rds(a)
            if not t or b >= 512 or not (0 < c <= 64):
                return None
            out.append((t, b, c))
            o += 12
        return out or None

    text = sec[".text"]
    tlo, thi = text[4], text[4] + text[5]
    m, i = {}, tlo
    while i < thi - 16:
        if (d[i:i + 3] == b"\x83\x7d\x08" and d[i + 4:i + 6] == b"\x75\x0b"
                and d[i + 6:i + 8] == b"\x8d\x81"):
            g = group((got + struct.unpack_from("<i", d, i + 8)[0]) & 0xFFFFFFFF)
            if g:
                m.setdefault(d[i + 3], g)
            i += 12
            continue
        i += 1
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--so", required=True)
    ap.add_argument("--reg")
    ap.add_argument("--json")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--sbus", action="store_true",
                    help="SBus/SerDes registers, keyed by register NUMBER")
    a = ap.parse_args()
    m = scan_sbus(a.so) if a.sbus else scan(a.so)
    print(f"{len(m)} registers mapped to field groups, "
          f"{sum(len(v) for v in m.values())} fields", file=sys.stderr)

    if a.check:
        bad = 0
        for reg, want in KNOWN.items():
            got = {f[0] for f in m.get(reg, [])}
            ok = want <= got
            bad += not ok
            print(f"  {reg:12s} {'ok' if ok else 'MISMATCH'}  "
                  f"({len(got)} fields)")
            if not ok:
                print(f"      expected to contain {sorted(want)}")
        print("CHECK PASS" if not bad else f"CHECK FAIL ({bad})")
        return 1 if bad else 0

    if a.json:
        json.dump({k: [list(f) for f in v] for k, v in m.items()},
                  open(a.json, "w"), indent=1)
        return 0

    if a.reg:
        key = int(a.reg, 0) if a.sbus else a.reg
        g = m.get(key)
        if not g:
            print(f"{a.reg}: no field group found", file=sys.stderr)
            return 1
        for n, b, w in sorted(g, key=lambda x: x[1]):
            print(f"  bit {b:<3d} w {w:<2d}  {n}")
        return 0

    for reg in sorted(m):
        if a.sbus:
            names = ", ".join(f[0] for f in sorted(m[reg], key=lambda x: x[1]))
            print(f"  0x{reg:02x}  {names}")
        else:
            print(f"{reg:40s} {len(m[reg])} fields")
    return 0


if __name__ == "__main__":
    sys.exit(main())
