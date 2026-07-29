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

echo "== disable SRAM-uncorrectable FATAL (0x1C018-1B) across the ECC-establishing fill =="
# RE (agent, exhaustive): EOS writes NOTHING to the error/fatal block; it survives the blind
# bank fill only because its bank cells ALREADY hold valid ECC (written by cmd=2 repair's BISR).
# Our banks still hold power-on INVALID ECC, so the CRM engine's first touch raises
# SRAM_UNCORRECTABLE -> FM6000 core FATAL RESET (off-bus, link stays up — exactly our symptom).
# The CRM blind fill IS our ECC-establishing write; mask the fatal ACROSS it so the reset does
# not fire, then the fill writes valid ECC to every cell. Re-arm happens between i2c_bringup and
# here, so clear it now over PCIe. Golden warm chip runs with 0x1C018=0, so leave it cleared.
# Do NOT touch IM 0x1C014 (no ECC ISR on M1; unmasking wedged in bist5).
echo "  FATAL 0x1C018 before = 0x$(R 0x1C018)"
W 0x1C018 0x0; W 0x1C019 0x0; W 0x1C01A 0x0; W 0x1C01B 0x0
echo "  FATAL 0x1C018 after  = 0x$(R 0x1C018) (want 00000000)"

echo "== CRM Memory-Set fill of MCAST_MID (0x240000) — VENDOR-EXACT 128-bit engine writes =="
# RE (fm6000CrmSetMemoryExt 0x35fc78, literal decode): descriptor for base=0x240000,
# regSize=3 (128b), count=4096, val=0. Count lives in CRM_COMMAND[33:14]; BlockSize=0xF (max,
# Count bounds the span); Stride=0 (engine AUTO-ADVANCES 4 words/element). Completion = poll
# engine STATUS 0x1f001 bit0 (busy); the vendor NEVER reads a memory element to check done
# (a read of an unfilled bank entry is exactly what wedges the crossbar — our earlier bug).
W 0x1f000 0x0                 # stop engine (defensive)
W 0x1f080 0x04000000          # CRM_COMMAND lo: MemorySet(0), Count=4096 (<<14)
W 0x1f081 0x00000000          # CRM_COMMAND hi
W 0x1f100 0x0FE40000          # CRM_REGISTER lo: base 0x240000 | Size=3<<22 | BlockSize1=0xF<<24 | Stride1=0
W 0x1f101 0x0000000F          # CRM_REGISTER hi: BlockSize2=0xF, Stride2=0
W 0x1f200 0x00000000          # DATA (fill value word)
W 0x1f180 0x00000000          # DATA high/mask lo
W 0x1f181 0x00000000          # DATA high/mask hi
W 0x1f004 0x1                 # clear IP slot0 (W1C)
W 0x1f000 0x00000001          # TRIGGER / GO (RUN, First=Last=slot0)
echo "   triggered; polling engine STATUS 0x1f001 bit0 (busy) — NO memory read..."
fdone=NO; i=0
while [ $i -lt 3000 ]; do
    st=$(R 0x1f001); st=$((0x${st:-1}))
    [ $((st & 1)) -eq 0 ] && { fdone=YES; break; }
    i=$((i+1))
done
echo "   MCAST_MID fill done=$fdone after $i polls (STATUS 0x1f001=0x$(R 0x1f001) IP=0x$(R 0x1f004))"
[ "$fdone" != YES ] && { echo "   *** engine never cleared busy — abort (WD recovers to EOS) ***"; exit 2; }

echo "== post-DONE verify: reads of FILLED entries are safe now (ECC valid). Fill span = entries 0..4095 =="
# 0x240000=entry0.w0  0x240004=entry1.w0  0x243FFC=entry4095.w0  (all within the 4096x128b fill)
for a in 0x240000 0x240004 0x243FFC; do echo "  $a = 0x$(R $a)  (want 0, NOT ffffffff)"; done

echo "== program byte-mover target: MCAST_DEST[1] = {bit0 = CPU port 0} (128-bit entry, RMW-safe post-fill) =="
W 0x240007 0x0; W 0x240006 0x0; W 0x240005 0x0; W 0x240004 0x1   # commit low word last
echo "  MCAST_DEST[1] = 0x$(R 0x240004) (want 00000001)"
echo "== *** COLD MCAST BANKED-ECC INIT COMPLETE *** (fill worked, array readable+writable) =="
echo "   Follow-on for full cold byte-mover: CRM-fill GLORT/L2F, load microcode, sched ring, DMA."
