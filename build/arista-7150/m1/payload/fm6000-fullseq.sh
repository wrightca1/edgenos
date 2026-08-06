#!/bin/sh
# Cold bring-up using EOS's COMPLETE cold-boot trace.
#  1. fm6000_coldreplay : clocks / BOOT_CTRL / BIST / scheduler  (supplies the MGMT1-2 writes the
#                          replay set deliberately excludes)
#  2. fm6000_memfill    : the 129 CRM memory fills (tables are uninitialised SRAM cold)
#  3. fm6000_fullreplay : 283,814 writes of EOS's real port + forwarding bring-up, in boot order,
#                          with proper SBus transactions for the JSS 0xF001/0xF002 stream
#  4. SFP laser, then link + inject
B=0000:02:00.0
LOG=/mnt/flash/fullseq.log
: > $LOG
say(){ echo "[fs] $*" >> $LOG; echo "[fs] $*"; sync; }
R(){ fm6000reg $B "$1" 2>/dev/null | sed 's/.*= 0x//'; }
S(){ scdreg "$1" 2>/dev/null | sed 's/.*= 0x//'; }
scdreg 0x0120 0xC0000BB8 >/dev/null 2>&1
( while : ; do scdreg 0x0120 0xC0000BB8 >/dev/null 2>&1; sleep 3; done ) & PET=$!
trap 'kill $PET 2>/dev/null; scdreg 0x0120 0x0 >/dev/null 2>&1' EXIT INT TERM
say "START PIN=$(R 0x1c021) SOFT_RESET=$(R 0x9)"

say "STEP1 coldreplay (clocks + BOOT_CTRL + BIST + scheduler)"
/tmp/fm6000_coldreplay $B >> $LOG 2>&1
say "  rc=$? PIN=$(R 0x1c021) sched=$(R 0x8062)"
[ "$(R 0x1c021)" = "00000208" ] || { say "off-bus; abort"; exit 2; }

say "STEP2 initsbus (JSS SBus master)"
/tmp/fm6000_initsbus $B >> $LOG 2>&1; say "  initsbus rc=$? PIN=$(R 0x1c021)"
# NOTE: the Intel SPICO SerDes firmware is deliberately NOT loaded.
# Proven unnecessary on this platform (2026-08-06): with all 30,002 SPICO IMEM
# SBus transactions stripped from the replay, Et1 still trains to 10G
# (PORT_STATUS=0x8c0, pcsRx=1) and the datapath is fully functional
# (39 frames TX, 29 RX, ICMP 8/8 0% loss). Dropping it removes a 12,000-byte
# third-party firmware blob from the runtime. See docs/PROVENANCE.md.
# Caveat: only validated on a short SR fibre link; the SPICO drives RX
# adaptation, so longer/lossier media may yet need an equaliser strategy.

say "STEP3 memfill (129 memory fills)"
/tmp/fm6000_memfill $B 0 >> $LOG 2>&1
say "  rc=$? PIN=$(R 0x1c021) MCAST=$(R 0x240000) PRIVWM=$(R 0x112800)"

say "STEP4 microcode (parser/FFU/mapper + MOD)"
fm6000_ucode_dbg $B /mnt/flash/ucode_l2.raw   /mnt/flash/u1.log >/dev/null 2>&1; say "  l2 rc=$? PIN=$(R 0x1c021)"
fm6000_ucode_dbg $B /mnt/flash/ucode_tail.raw /mnt/flash/u2.log >/dev/null 2>&1; say "  tail rc=$? PIN=$(R 0x1c021)"

say "STEP5 FULL REPLAY of EOS's port+forwarding bring-up (299803 writes: BOTH ports + ECMP)"
# fwd5.txt = fwd4.txt with the 30,002 SPICO-firmware SBus transactions removed
# (90,006 register writes). Validated end-to-end 2026-08-06. NOT distributable.
/tmp/fm6000_fullreplay /tmp/fwd5.txt $B 0 >> $LOG 2>&1
say "  rc=$? PIN=$(R 0x1c021) PORT_STATUS=$(R 0xe3800) pcsRx=$(R 0xe3826) sched=$(R 0x8062)"

say "STEP6 SFP laser"
V=$(S 0x5010); scdreg 0x5010 $(printf '0x%x' $(( 0x$V & ~0x40 ))) >/dev/null 2>&1
say "  0x5010 $V -> $(S 0x5010)"

say "STEP7 settle + link"
i=1; while [ $i -le 8 ]; do sleep 3
  say "  t=$((i*3))s PORT_STATUS=$(R 0xe3800) pcsRx=$(R 0xe3826) 0xe383f=$(R 0xe383f) PIN=$(R 0x1c021)"
  i=$((i+1)); done
say "=== FULLSEQ DONE ==="
