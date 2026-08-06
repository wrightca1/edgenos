#!/bin/sh
# Cold bring-up using EOS's COMPLETE cold-boot trace.
#  1. fm6000_coldreplay : clocks / BOOT_CTRL / BIST / scheduler  (supplies the MGMT1-2 writes the
#                          replay set deliberately excludes)
#  2. fm6000_memfill    : the 129 CRM memory fills (tables are uninitialised SRAM cold)
#  3. fm6000_fullreplay : 283,814 writes of EOS's real port + forwarding bring-up, in boot order,
#                          with proper SBus transactions for the JSS 0xF001/0xF002 stream
#  4. SFP laser, then link + inject
B=0000:02:00.0
# Tools ship in the image (/usr/bin) as of 2026-08-06; fall back to /tmp for
# ad-hoc runs where they were wget'd. Replay set is operator-supplied (not
# distributable) - override with FWD=<path>.
BIN=/usr/bin; [ -x $BIN/fm6000_coldreplay ] || BIN=/tmp
FWD="${FWD:-/mnt/flash/fwd4.txt}"; [ -f "$FWD" ] || FWD=/tmp/fwd4.txt
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
$BIN/fm6000_coldreplay $B >> $LOG 2>&1
say "  rc=$? PIN=$(R 0x1c021) sched=$(R 0x8062)"
[ "$(R 0x1c021)" = "00000208" ] || { say "off-bus; abort"; exit 2; }

say "STEP2 initsbus (JSS SBus master)"
$BIN/fm6000_initsbus $B >> $LOG 2>&1; say "  initsbus rc=$? PIN=$(R 0x1c021)"
# The SPICO SerDes firmware is uploaded INLINE by the replay (fwd4.txt), at the
# point EOS does it -- ~14.5% in, right after the MOD microcode. Do NOT load it
# with a separate fm6000_spico step before the replay: the replay later resets
# and starts the SPICO, wiping an early upload, and the SPICO then runs with an
# empty IMEM.
#
# *** SPICO IS REQUIRED for 10GBASE-CR (DAC/copper). ***
# An earlier bisect concluded it was unnecessary -- that was WRONG, because it
# only ever checked Et1 (10GBASE-SR fibre). With the firmware stripped:
#   Et1 (SR)  links fine   -> PORT_STATUS=0x8c0, pcsRx=1
#   Et2 (CR)  does NOT     -> PORT_STATUS=0x815, pcsRx=0
# With fwd4.txt unmodified, BOTH link at 0x8c0/pcsRx=1. See docs/SPICO-RE.md.

say "STEP3 memfill (129 memory fills)"
$BIN/fm6000_memfill $B 0 >> $LOG 2>&1
say "  rc=$? PIN=$(R 0x1c021) MCAST=$(R 0x240000) PRIVWM=$(R 0x112800)"

say "STEP4 microcode (parser/FFU/mapper + MOD)"
fm6000_ucode_dbg $B /mnt/flash/ucode_l2.raw   /mnt/flash/u1.log >/dev/null 2>&1; say "  l2 rc=$? PIN=$(R 0x1c021)"
fm6000_ucode_dbg $B /mnt/flash/ucode_tail.raw /mnt/flash/u2.log >/dev/null 2>&1; say "  tail rc=$? PIN=$(R 0x1c021)"

say "STEP5 FULL REPLAY of EOS's port+forwarding bring-up (299803 writes: BOTH ports + ECMP)"
# fwd5.txt = fwd4.txt with the 30,002 SPICO-firmware SBus transactions removed
# (90,006 register writes). Validated end-to-end 2026-08-06. NOT distributable.
$BIN/fm6000_fullreplay "$FWD" $B 0 >> $LOG 2>&1
say "  rc=$? PIN=$(R 0x1c021) PORT_STATUS=$(R 0xe3800) pcsRx=$(R 0xe3826) sched=$(R 0x8062)"

say "STEP6 SFP laser"
V=$(S 0x5010); scdreg 0x5010 $(printf '0x%x' $(( 0x$V & ~0x40 ))) >/dev/null 2>&1
say "  0x5010 $V -> $(S 0x5010)"

say "STEP7 settle + link"
i=1; while [ $i -le 8 ]; do sleep 3
  say "  t=$((i*3))s PORT_STATUS=$(R 0xe3800) pcsRx=$(R 0xe3826) 0xe383f=$(R 0xe383f) PIN=$(R 0x1c021)"
  i=$((i+1)); done
say "=== FULLSEQ DONE ==="
