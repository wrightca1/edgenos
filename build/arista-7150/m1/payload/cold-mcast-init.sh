#!/bin/sh
# cold-mcast-init.sh — clean-room COLD init of the FM6000 MCAST banked-ECC memory.
#
# Runs on M1 AFTER a cold boot has done repair(cmd=2)+BIST march+enum (fm6000_i2c_bringup)
# but has NOT yet touched MCAST. Establishes valid ECC across all 4096 MCAST_DEST entries
# via the CRM Memory-Set HARDWARE ENGINE (blind full-width 128-bit fill, value 0) — the
# exact mechanism EOS uses (RE: libFocalpointSDK fm6000CrmSetMemoryExt @0x35fc78, MCAST
# call site 0x3c37ab: base=0x240000 val=0 regSize=3(128b) count=0x1000). NO cpu read or
# partial write of MCAST happens before the fill — that is what wedges the chip.
#
# SAFETY: run only with the SCD watchdog ARMED (cold-init-test init does this) so a wedge
# recovers to EOS. Order matters: fill FIRST, read/verify SECOND.
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
B=0000:02:00.0
R(){ fm6000reg $B "$1" 2>/dev/null | grep -o '[0-9a-f]*$'; }

echo "== pre-flight: chip must be enumerated + repair/BIST done, MCAST NOT yet touched =="
[ -e /sys/bus/pci/devices/$B/vendor ] || { echo "  FM6000 $B absent — abort"; exit 1; }
# ensure BAR0 MMIO on
CMD=$(pcicfg $B 0x04 2>/dev/null | grep -o '0x[0-9a-f]*$'); pcicfg $B 0x04 $(printf '0x%x' $(( ${CMD:-0} | 0x6 ))) >/dev/null 2>&1
echo "  BM_ENGINE_STATUS(0x1D08E)=0x$(R 0x1D08E)  (want 0 = BIST idle/done)"
echo "  BOOT_CTRL(0x1C022)=0x$(R 0x1C022)         (want bit4=1 = repair CommandDone)"
echo "  BIST pass/fail 0x1D407=0x$(R 0x1D407) 0x1D587=0x$(R 0x1D587) 0x1D70E=0x$(R 0x1D70E) (want 0)"

echo "== CRM Memory-Set BLIND FILL of the banked memories (establishes ECC; NO read first) =="
# MCAST_MID: 4096 x 128-bit, value 0
echo "-- MCAST_MID 0x240000 (4096 x 128b) --"
fm6000_crm 0x240000 4096 0 3 2>&1 | sed 's/^/   /'
mcrc=$?
[ $mcrc -ne 0 ] && { echo "   *** MCAST_MID CRM fill FAILED (rc=$mcrc) — chip may be wedged; WD will recover to EOS ***"; exit 2; }
# MCAST_POST 0x260000 and STATS_BANK 0x200000 (other repairable banks; regSize per EOS)
echo "-- MCAST_POST 0x260000 (32768 x 32b) --"; fm6000_crm 0x260000 32768 0 0 2>&1 | sed 's/^/   /'
echo "-- STATS_BANK 0x200000 (16384 x 32b) --"; fm6000_crm 0x200000 16384 0 0 2>&1 | sed 's/^/   /'

echo "== post-fill verify: MCAST array now readable (valid ECC), not 0xffffffff =="
for a in 0x240000 0x240004 0x244040 0x24FFFC; do echo "  $a = 0x$(R $a)"; done

echo "== program the byte-mover target: MCAST_DEST[1] = {bit0 = CPU port 0} (128-bit write) =="
# entry 1 = word 0x240004..0x240007; catch-all GLORT DMaskBaseIdx=1 points here.
fm6000reg $B 0x240007 0x0 >/dev/null 2>&1
fm6000reg $B 0x240006 0x0 >/dev/null 2>&1
fm6000reg $B 0x240005 0x0 >/dev/null 2>&1
fm6000reg $B 0x240004 0x1 >/dev/null 2>&1   # commit word LAST -> 128b entry {DestMask=0x1}
echo "  MCAST_DEST[1] = 0x$(R 0x240004) (want 00000001)"
echo "== COLD MCAST INIT COMPLETE. Next: insmod fm6000dma + fpdma_probe tx 0xff00 0x0028 =="
