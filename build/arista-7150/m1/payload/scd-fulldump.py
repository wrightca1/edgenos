#!/usr/bin/env python
# Full 256KB SCD BAR0 dump - NON-DESTRUCTIVE (reads only).
# Runs on EOS (python2.7) and on M1 (python2 or 3). mmaps sysfs resource0.
#
# Purpose: find the switch-side (interruptBlock-3) accelerator DOMAIN-ENABLE
# register by diffing EOS-alive vs M1-dead over the WHOLE bar. Prior dumps only
# covered 0x0-0x9010; the enable is almost certainly at an unsampled offset
# (0x2000 gap, or 0xa000-0x40000, all never read).
#
# Usage:
#   python scd-fulldump.py            # auto-find 3475:0001, dump 0..0x40000
#   python scd-fulldump.py <resource0-path>
# Output: lines "OFFSET=VALUE" (hex, 4-byte). Redirect to a file per boot:
#   EOS:  python scd-fulldump.py > /mnt/flash/scd-eos-full.txt
#   M1:   python scd-fulldump.py > /mnt/flash/scd-m1-full.txt
# then:   diff (compare with the awk one-liner at the bottom of this file)
#
# CAVEAT: a blind full read touches FIFO-pop / clear-on-read regs. In each
# 0x80 smbus-accel block, +0x30 (response FIFO) is pop-on-read and +0x00/+0x10
# are the request FIFO. The interrupt status regs (0x3020/0x3050/0x3080/0x30b0)
# may be clear-on-read. This is harmless for a one-shot diff, but do NOT loop it
# while the smbus/interrupt agents are live, and treat those specific offsets as
# noise in the diff. Everything else is plain registers.

import os, sys, mmap, struct, glob

BAR_LEN = 0x40000  # 256KB

def find_resource0():
    for d in glob.glob('/sys/bus/pci/devices/*'):
        try:
            vid = open(os.path.join(d, 'vendor')).read().strip()
            did = open(os.path.join(d, 'device')).read().strip()
        except IOError:
            continue
        if vid == '0x3475' and did == '0x0001':
            return os.path.join(d, 'resource0')
    return None

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else find_resource0()
    if not path or not os.path.exists(path):
        sys.stderr.write('SCD resource0 not found (looked for 3475:0001)\n')
        return 1
    size = os.path.getsize(path)
    length = min(BAR_LEN, size)
    fd = os.open(path, os.O_RDWR)  # RDWR needed for PROT map even to read on some kernels; falls back below
    try:
        m = mmap.mmap(fd, length, mmap.MAP_SHARED, mmap.PROT_READ)
    except Exception:
        os.close(fd)
        fd = os.open(path, os.O_RDONLY)
        m = mmap.mmap(fd, length, mmap.MAP_SHARED, mmap.PROT_READ)
    out = sys.stdout
    for off in range(0, length, 4):
        val = struct.unpack('<I', m[off:off+4])[0]
        out.write('%05x=%08x\n' % (off, val))
    m.close(); os.close(fd)
    sys.stderr.write('dumped 0x0..0x%x (%d regs) from %s\n' % (length, length//4, path))
    return 0

if __name__ == '__main__':
    sys.exit(main())

# ---- diff step (run on either box after copying both files together) ----
# awk -F= 'NR==FNR{e[$1]=$2;next} $1 in e && e[$1]!=$2 {printf "0x%s  M1=0x%s  EOS=0x%s\n",$1,$2,e[$1]}' \
#     scd-eos-full.txt scd-m1-full.txt
# The block-3 enable = a reg that is set/nonzero on EOS and 0 (or different) on
# M1, OUTSIDE the accel data regs. Prime suspects to eyeball first:
#   0x02000-0x02fff (unused gap, never sampled), 0x0a000-0x3ffff (never sampled),
#   0x00160 (known to differ: M1=0 EOS=0x2a0000), and any *set/clear reset* pair.
