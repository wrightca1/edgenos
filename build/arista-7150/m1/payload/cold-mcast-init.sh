#!/bin/sh
# cold-mcast-init.sh — clean-room COLD init of the FM6000 MCAST banked-ECC memory.
#
# Runs on M1 over PCIe/BAR0 AFTER a cold boot has done cmd=2 repair + BIST march + enum
# (fm6000_i2c_bringup) but has NOT touched any bank. Completes the boot-controller
# sequence EOS runs, then blind-fills the banked memories via the CRM Memory-Set engine.
#
# RE (libFocalpointSDK 4.16.7M): EOS issues ExecuteBootCommand cmd=2 (Repair, PrebootSwitch
# 0x3c7d0a) -> BIST march -> cmd=1 (FFU init, BootSwitch 0x3c9605) -> cmd=3 (free-list/
# segment init, 0x3c97d8), and ONLY THEN table-init (0x3be713) calls fm6000CrmSetMemoryExt
# (0x3c37ab) to fill MCAST_MID 0x240000 (val 0, 128-bit, 4096). The banked memories share a
# sequencer/segment subsystem that is not driven online until cmd=1 AND cmd=3 run — cmd=2
# alone repairs cells but leaves the bank un-writable -> a fill/read wedges. GATE: 0x1C022
# must read 0x313 (0x300 status | 0x10 CommandDone | cmd=3 latched) before any bank touch.
#
# Each ExecuteBootCommand(cmd) = write 0->0x1C022 (idle), >=1us, write cmd, poll bit4 (0x10).
# SAFETY: run only with the SCD watchdog ARMED (cold-init-test init does this).
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
B=0000:02:00.0
R(){ fm6000reg $B "$1" 2>/dev/null | grep -o '[0-9a-f]*$'; }
W(){ fm6000reg $B "$1" "$2" >/dev/null 2>&1; }

# ExecuteBootCommand: $1=cmd. idle->cmd->poll CommandDone(bit4). Register round-trips are
# >>1us so an intervening read guarantees the >=1us idle settle.
exec_boot_cmd(){
    cmd="$1"; W 0x1C022 0x0; R 0x1C022 >/dev/null; W 0x1C022 "$cmd"
    i=0; while [ $i -lt 200 ]; do
        v=$(R 0x1C022); v=$((0x${v:-0}))
        [ $((v & 0x10)) -ne 0 ] && break
        i=$((i+1))
    done
    printf 'cmd=%s -> BOOT_CTRL=0x%08x (done=%s, %d polls)\n' "$cmd" "$v" \
        "$([ $((v & 0x10)) -ne 0 ] && echo YES || echo NO)" "$i"
}

echo "== pre-flight: chip enumerated, BIST done, cmd=2 repaired (BOOT_CTRL likely 0x300) =="
[ -e /sys/bus/pci/devices/$B/vendor ] || { echo "  FM6000 $B absent — abort"; exit 1; }
CMD=$(pcicfg $B 0x04 2>/dev/null | grep -o '0x[0-9a-f]*$'); pcicfg $B 0x04 $(printf '0x%x' $(( ${CMD:-0} | 0x6 ))) >/dev/null 2>&1
echo "  BM_ENGINE_STATUS(0x1D08E)=0x$(R 0x1D08E) (want 0)   BOOT_CTRL(0x1C022)=0x$(R 0x1C022)"

echo "== complete the boot-controller sequence EOS runs: cmd=2 -> cmd=1 -> cmd=3 =="
echo "-- cmd=2 (Repair Bank Memory; idempotent, re-assert) --"; exec_boot_cmd 0x2
echo "-- cmd=1 (FFU slice init) --";                            exec_boot_cmd 0x1
echo "-- cmd=3 (free-list / segment init) --";                 exec_boot_cmd 0x3

echo "== GO/NO-GO gate: BOOT_CTRL must read 0x313 (matches golden EOS) before any bank fill =="
bc=$(R 0x1C022)
echo "  BOOT_CTRL(0x1C022)=0x$bc"
case "$bc" in
    *313) echo "  GATE PASS (0x313) — banks online, proceeding to CRM fill" ;;
    *) echo "  *** GATE FAIL (0x$bc != *313) — cmd=3 did not latch, bank NOT online. Aborting (do NOT fill). ***"; exit 3 ;;
esac

echo "== release module SOFT-RESET (reg 0x9): bring bank-owning modules out of reset =="
# RE: fm6000BootSwitch @0x3ca434 "Bringing remaining modules out of reset" -> RMW reg 0x9
# clearing bits {0,1,2,4}. Until then MCAST-replication/STATS modules are held in reset so
# their memory ports give no bus completion -> CRM engine write stalls the crossbar (our hang).
# Use & ~0x1F (also clear bit3 IdentifySwitch clears) to match EOS end-state. Reg 0x9 = SOFT_RESET.
sr=$(R 0x00009); echo "  SOFT_RESET(0x9) before = 0x$sr"
nsr=$(printf '0x%08x' $(( (0x${sr:-0} & ~0x1F) & 0xFFFFFFFF )))
W 0x00009 $nsr
sr2=$(R 0x00009); echo "  SOFT_RESET(0x9) after  = 0x$sr2  (bits0-4 clear; crossbar alive if this printed)"

echo "== CRM Memory-Set BLIND FILL of the banked memories (establishes ECC; NO read first) =="
echo "-- MCAST_MID 0x240000 (4096 x 128b) --"
fm6000_crm 0x240000 4096 0 3 2>&1 | sed 's/^/   /'
[ $? -ne 0 ] && { echo "   *** MCAST_MID CRM fill FAILED — WD will recover to EOS ***"; exit 2; }
echo "-- MCAST_POST 0x260000 (32768 x 32b) --"; fm6000_crm 0x260000 32768 0 0 2>&1 | sed 's/^/   /'
echo "-- STATS_BANK 0x200000 (16384 x 32b) --"; fm6000_crm 0x200000 16384 0 0 2>&1 | sed 's/^/   /'

echo "== post-fill verify: MCAST array now readable (valid ECC), not 0xffffffff =="
for a in 0x240000 0x240004 0x244040 0x24FFFC; do echo "  $a = 0x$(R $a)"; done

echo "== program byte-mover target: MCAST_DEST[1] = {bit0 = CPU port 0} =="
W 0x240007 0x0; W 0x240006 0x0; W 0x240005 0x0; W 0x240004 0x1   # commit low word last
echo "  MCAST_DEST[1] = 0x$(R 0x240004) (want 00000001)"
echo "== COLD MCAST INIT COMPLETE. Next: insmod fm6000dma + fpdma_probe tx 0xff00 0x0028 =="
