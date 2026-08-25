#!/usr/bin/env python3
"""strip-spico.py - remove the SPICO SerDes firmware upload from a replay file.

    strip-spico.py <fwd4.txt> <out.txt>

The single largest item in the replay is Intel's SerDes microcode, pushed over the
SBus as 30,002 transactions = 90,006 MMIO writes, 41% of everything still replayed
after the generators have run. It is a firmware image, not a table: decoding it
does not let us author it.

⚠ It is load-bearing for COPPER. Stripped, Et2 (10GBASE-CR) linked 0 of 7 boots
against 5 of 10 with it (Fisher p = 0.041). Fibre is unaffected -- Et1 came up on
all 17 stripped boots. So this produces a FIBRE-ONLY image. See docs/EDGENOS-7150.md (was SPICO-RE)
and docs/EDGENOS-7150.md (was BLOB-REMOVAL-PLAN).

SBus transaction shape, from our own fm6000_initsbus.c (not from EOS):

    wr(0xF002, data)          REQUEST = data
    wr(0xF001, 0)             clear stale Execute
    wr(0xF001, cmd)           trigger; cmd = reg | dev<<8 | op<<16 | 1<<24

The firmware upload is device 0xFD (the SPICO broadcast) registers 0x04-0x07.
All three writes of a matching transaction are dropped together -- dropping only
the trigger would leave a stranded REQUEST that the next transaction inherits as
its data, which is silent corruption rather than a smaller file.
"""
import sys

REQ, CMD = 0x0F002, 0x0F001
SPICO_DEV = 0xFD
SPICO_REGS = {0x04, 0x05, 0x06, 0x07}


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]

    out, pend = [], []          # pend = lines of the transaction being assembled
    kept = dropped = txns = 0

    def flush():
        out.extend(pend)
        pend.clear()

    for line in open(src, errors="replace"):
        f = line.split()
        if len(f) != 2:
            flush(); out.append(line); continue
        try:
            addr, val = int(f[0], 16), int(f[1], 16)
        except ValueError:
            flush(); out.append(line); continue

        if addr == REQ:
            flush(); pend.append(line)
        elif addr == CMD and val == 0 and pend:
            pend.append(line)
        elif addr == CMD and (val & (1 << 24)):
            pend.append(line)
            dev, reg = (val >> 8) & 0xFF, val & 0xFF
            txns += 1
            if dev == SPICO_DEV and reg in SPICO_REGS:
                dropped += len(pend); pend.clear()
            else:
                kept += len(pend); flush()
        else:
            flush(); out.append(line)
    flush()

    with open(dst, "w") as fh:
        fh.writelines(out)

    src_n = sum(1 for _ in open(src, errors="replace"))
    print(f"in  : {src_n} lines")
    print(f"out : {len(out)} lines")
    print(f"SBus transactions seen : {txns}")
    print(f"SPICO writes dropped   : {dropped}   ({dropped // 3} transactions)")
    if dropped != 90006:
        print(f"⚠ expected 90,006 dropped writes (30,002 transactions), got {dropped}")


if __name__ == "__main__":
    main()
