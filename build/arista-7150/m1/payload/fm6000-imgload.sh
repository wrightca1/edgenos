#!/bin/sh
# fm6000-pacedload.sh - load full vendor microcode, but the SIDE-EFFECTING danger zone
# (lines 30322+, the 0x1a10xx-data / 0x14085-commit indexed table) PACED line-by-line via
# fm6000reg (one process per write ~= natural ms spacing), to test whether the DZ5 wedge is
# HW-table-commit backpressure (fixed by pacing) vs a specific bad entry (not fixed).
B=0000:02:00.0
PATH=/tmp:$PATH; export PATH
c(){ echo "[PL] $*" > /dev/kmsg 2>/dev/null; echo "[PL] $*" > /dev/console 2>/dev/null; echo "[PL] $*"; }
R(){ fm6000reg $B $1 2>/dev/null | grep -o '[0-9a-f]*$'; }
G(){ grep -o '0x[0-9a-f]*$'; }
WD(){ scdreg 0x0120 0xC0000BB8 >/dev/null 2>&1; }

c "=== PACED vendor microcode load ==="
V=$(pcicfg $B 0x04 | G); pcicfg $B 0x04 $(printf '0x%x' $(( V | 0x6 ))) >/dev/null 2>&1; WD
c "MSE=$(pcicfg $B 0x04|G) wd=$(scdreg 0x0120|G) SOFT_RESET=0x$(R 0x00009) PIN_STRAP=0x$(R 0x1C021)"
# Boot-controller commands in datasheet Table 4-1 order: 1=FFU slices, 2=bank
# repairs, 3=freelists. Poll BOOT_STATUS:CommandDone (0x1C022 bit4) after each —
# NOT a blind sleep (the command holds the next step until done).
for cmd in 1 2 3; do
  fm6000reg $B 0x1C022 $cmd >/dev/null 2>&1
  n=0; while [ $n -lt 50 ] && [ $(( 0x$(R 0x1C022) & 0x10 )) -eq 0 ]; do sleep 0.1 2>/dev/null || sleep 1; n=$((n+1)); done
  WD
done
c "BOOT_CTRL=0x$(R 0x1C022)"
fm6000reg $B 0x00009 0x0 >/dev/null 2>&1; sleep 1; WD
c "MSB released SOFT_RESET=0x$(R 0x00009) PLL_STAT=0x$(R 0x1C046)"

c "LOAD L2 pipeline 1-30321 fast (proven clean)"
fm6000load $B /tmp/ucode_l2.raw >/dev/null 2>&1; WD
c "  L2 done PIN_STRAP=0x$(R 0x1C021)"

c "DANGER ZONE 30322-39461 PACED line-by-line (this is the test)"
n=0
while read a v; do
  n=$((n+1))
  fm6000reg $B 0x$a 0x$v >/dev/null 2>&1
  if [ $((n % 200)) -eq 0 ]; then WD; c "  paced $n/9140 lastaddr=0x$a PIN_STRAP=0x$(R 0x1C021)"; fi
done < /tmp/ucode_tail.raw
WD
c "=== PACED FULL LOAD COMPLETE (NO WEDGE) wrote $n DZ lines PIN_STRAP=0x$(R 0x1C021) ==="
scdreg 0x0120 0x0 >/dev/null 2>&1
c "watchdog disabled=$(scdreg 0x0120|G) DONE"
