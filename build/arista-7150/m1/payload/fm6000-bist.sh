#!/bin/sh
# fm6000-bist.sh - FM6000 cold-BIST memory-init (clean-room replay).
#
# Marches every table RAM to a parity-valid state so subsequent CPU writes to the
# forwarding tables (MCAST/MOD/L2F) don't fault the block. Sequence recovered from
# libFocalpointSDK.so:fm6000BistMemoryInit (elf32-i386 @0x34bb94), every value
# cross-checked bit-for-bit against the running-EOS capture
# (arista reference/live-captures/7150-fm6000/eos-bist-2026-07-26). Run AFTER the
# chip is accessible (pcie-init) and BEFORE the boot-controller/MSB/microcode.
# Run with the SCD watchdog armed. Word-addressed CSRs via fm6000reg.
# SPDX-License-Identifier: GPL-2.0-or-later
B="${1:-0000:02:00.0}"
W(){ fm6000reg "$B" "$1" "$2" >/dev/null 2>&1; }
R(){ fm6000reg "$B" "$1" 2>/dev/null | grep -o '[0-9a-f]*$'; }

echo "[bist] 1. scan/PLL setup (0x1C03A) + delays"
W 0x1C03A 0x00000063; usleep 640 2>/dev/null || sleep 1
R 0x1C03C >/dev/null                          # dummy read (SDK does this)
W 0x1C03A 0x80000063; usleep 640 2>/dev/null || sleep 1
W 0x1C03A 0x88D55555; usleep 640 2>/dev/null || sleep 1
W 0x1C03A 0x88009555; usleep 1640 2>/dev/null || sleep 1

echo "[bist] 2. gate: BM_ENGINE_STATUS(0x1D08E) must be idle -> 0x$(R 0x1D08E)"

echo "[bist] 3. BM march-sequence table (0x1D080-83, mirror 0x1D708-0B)"
for a in 0x1D080 0x1D708; do
  W $(printf 0x%x $(($a+0))) 0x6529EDA9
  W $(printf 0x%x $(($a+1))) 0x9B8ED9B1
  W $(printf 0x%x $(($a+2))) 0xEFCA952B
  W $(printf 0x%x $(($a+3))) 0x000FCA99
done

echo "[bist] 4. per-block enables (bit21=0x200000) + 0xB4"
for w in 0 1 2 3; do W $(printf 0x%x $((0x1D210+w*0x80))) 0x00200000; done
for a in 0x1D400 0x1D480 0x1D500 0x1D580 0x1D600; do W $a 0x00200000; done
for w in 0 1 2 3; do W $(printf 0x%x $((0x1D218+w*0x80))) 0x000000B4; done

echo "[bist] 5. fusebox/repair enables (=4)"
for a in 0x1D241 0x1D2C1 0x1D261 0x1D281 0x1D2A1; do W $a 0x4; done

echo "[bist] 6. march per-memory config"
for w in 0 1 2 3; do W $(printf 0x%x $((0x1D404+w*0x80))) 0x0000000C; done
W 0x1D604 0x00000004
for a in 0x1D440 0x1D4C0 0x1D4E0 0x1D540 0x1D5C0 0x1D5E0 0x1D640 0x1D660; do W $a 0x1; done
W 0x1D409 0x00000FFF; W 0x1D489 0x00007FFF; W 0x1D509 0x00003FFF; W 0x1D589 0x00000FFF; W 0x1D609 0x000003FF
W 0x1D441 0x4; W 0x1D4C1 0x4; W 0x1D4E1 0x4; W 0x1D541 0x4; W 0x1D5C1 0x6; W 0x1D5E1 0x6; W 0x1D641 0xA; W 0x1D661 0xA
for w in 0 1 2 3; do W $(printf 0x%x $((0x1D220+w*0x80))) 0x3; done
W 0x1D40B 0x0; W 0x1D48B 0x2; W 0x1D50B 0x2; W 0x1D58B 0x2; W 0x1D60B 0x0

echo "[bist] 7. poll BM_ENGINE_STATUS(0x1D08E) until idle (==0), cap 30s"
i=0; st=$(R 0x1D08E)
while [ "$st" != "0" ] && [ "$st" != "00000000" ] && [ $i -lt 30 ]; do
  sleep 1; st=$(R 0x1D08E); i=$((i+1))
  [ "$st" = "ffffffff" ] && { echo "[bist] *** WEDGE during BIST (0x1D08E=ffffffff) ***"; exit 2; }
done
echo "[bist]   BM idle after ${i}s (status=0x$st)"

echo "[bist] 8. result: BM_result(0x1D70E)=0x$(R 0x1D70E) BM_ctrl(0x1D08C)=0x$(R 0x1D08C)"
echo "[bist] DONE"
