#!/usr/bin/env python3
# Read-only diagnostic: dump current DMA + packet I/O register state.
# Uses BDE_IOC_REG_READ on /dev/linux-kernel-bde, same path as leddance.py.

import ctypes, fcntl, os, struct, sys

BDE_DEV = "/dev/linux-kernel-bde"
BDE_IOC_TYPE = ord('b')
BDE_IOC_REG_READ = (6 << 29) | (12 << 16) | (BDE_IOC_TYPE << 8) | 1

class BDE:
    def __init__(self):
        self.fd = os.open(BDE_DEV, os.O_RDWR)
    def r(self, addr):
        buf = struct.pack("III", 0, addr, 0)
        ret = fcntl.ioctl(self.fd, BDE_IOC_READ if False else BDE_IOC_REG_READ, buf, True)
        return struct.unpack("III", ret)[2]
    def close(self): os.close(self.fd)

bde = BDE()

def show(label, addr, decode=None):
    v = bde.r(addr)
    extra = f"  ({decode(v)})" if decode else ""
    print(f"  {label:40s} [0x{addr:05x}] = 0x{v:08x}{extra}")

print("=== PAXB sub-window IMAP table (BAR0 + 0x2C00) ===")
for i in range(8):
    show(f"IMAP0_{i}",        0x2C00 + i*4)

print()
print("=== Legacy CMIC DMA (CMIC at 0x100) — what XGS DMA path uses ===")
def dma_ctrl(v): return f"EN={v&1} DIR={(v>>4)&1} ABORT={(v>>2)&1}"
def dma_stat(v):
    bits = []
    if v & 0x1: bits.append("ACTIVE")
    if v & 0x2: bits.append("DONE")
    if v & 0x4: bits.append("DESCRD_DONE")
    if v & 0x80000: bits.append("CHAIN_DONE")
    return " ".join(bits) if bits else "idle"
for chan in range(4):
    base = 0x100 + chan*0x40
    print(f" -- chan {chan} --")
    show(f"CMIC_DMA_CTRL[{chan}]",   base + 0x00, dma_ctrl)
    show(f"CMIC_DMA_STAT[{chan}]",   base + 0x04, dma_stat)
    show(f"CMIC_DMA_DESC[{chan}]",   base + 0x10)
    show(f"CMIC_DMA_HALT_ADDR[{chan}]", base + 0x14)

print()
print("=== CMICm DMA (CMC0 at 0x31100) — what XGSD DMA path uses ===")
# Per chip header: CTRL stride is 4 bytes per channel, base 0x31140
# DESC at 0x31158, STAT at 0x31150
for chan in range(4):
    show(f"CMIC_CMC_DMA_CTRL[{chan}]", 0x31140 + chan*4, dma_ctrl)
    show(f"CMIC_CMC_DMA_STAT[{chan}]", 0x31150 + chan*4, dma_stat)
    show(f"CMIC_CMC_DMA_DESC[{chan}]", 0x31158 + chan*4)

print()
print("=== CMIC packet control / IRQ ===")
show("CMIC_PKT_CTRL",            0x714)
show("CMIC_LED0_CTRL",           0x1000, lambda v: f"EN={v&1}")
show("CMIC_LED1_CTRL",           0x2000, lambda v: f"EN={v&1}")

print()
print("=== Sample LEDUP0 program (was passthrough.hex if leddance ran) ===")
prog = bytes(bde.r(0x1800 + i*4) & 0xFF for i in range(16))
print(f"  program[0..15] = {prog.hex(' ')}")

bde.close()
