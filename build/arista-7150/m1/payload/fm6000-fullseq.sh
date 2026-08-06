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
# PACE THE REPLAY -- and note the units. fm6000_fullreplay sleeps `pace` us once
# every 16384 ops, so over ~390k writes it fires only ~23 times. PACE=2000 adds
# 46 ms in total, i.e. effectively nothing; the default below adds ~34 s, which
# is a real slowdown.
#
# Why: the 10GBASE-CR (DAC/copper) link on Et2 comes up only intermittently when
# the replay is blasted at full speed. Et1 (10GBASE-SR fibre) is tolerant and
# links either way -- an optical RX needs no equalisation, a copper RX does, and
# equalisation needs settle time. EOS gets that for free because it polls and
# waits between steps.
#
# EVIDENCE IS THIN -- do not treat this as settled: unpaced 2/5, genuinely paced
# 1/1. The failure is LATCHED at bring-up: once Et2 misses lock, replaying EOS's
# own port-bounce -- 6 times, including the full 2,632-write version with the
# SBus lane reset -- does not recover it. See docs/ET2-COPPER-LINK.md.
# SAFGEN=1 (default): program the SAF store-and-forward matrix with OUR OWN code
# instead of replaying EOS's 34,668-write incremental accumulation. fm6000_safinit
# writes the 168-register end state, derived from our own board port table.
#
# The split reproduces EXACTLY the file cold-boot validated on 2026-08-07:
# everything up to the first IN-LOOP SAF write verbatim (which keeps the 171 SAF
# writes that precede the loop), then our generator, then the remainder with the
# recorded SAF writes dropped. Byte-identity was checked offline -- do NOT
# "simplify" this to a plain `grep -v` over the whole file: that also drops those
# 171 and produces a sequence nothing has ever booted.
#
# Set SAFGEN=0 to replay EOS's accumulation unchanged.
if [ "${SAFGEN:-1}" = "1" ] && [ -x "$BIN/fm6000_safinit" ]; then
	A0=$(grep -n '^001a0c00 ' "$FWD" | head -1 | cut -d: -f1)
	F1=$(tail -n +${A0:-1} "$FWD" | grep -n '^000a0' | head -1 | cut -d: -f1)
	if [ -n "$A0" ] && [ -n "$F1" ]; then
		F1=$((A0 + F1 - 1))
		head -n $((F1 - 1)) "$FWD" > /tmp/fwd.p1
		tail -n +$F1 "$FWD" | grep -v '^000a0' > /tmp/fwd.p2
		say "  SAF is ours: $(wc -l < /tmp/fwd.p1) + safinit(168) + $(wc -l < /tmp/fwd.p2)"
		$BIN/fm6000_fullreplay /tmp/fwd.p1 $B ${PACE:-1500000} >> $LOG 2>&1
		$BIN/fm6000_safinit $B >> $LOG 2>&1; say "  safinit rc=$?"
		$BIN/fm6000_fullreplay /tmp/fwd.p2 $B ${PACE:-1500000} >> $LOG 2>&1
		rm -f /tmp/fwd.p1 /tmp/fwd.p2
	else
		say "  SAFGEN: loop/SAF split not found; replaying unchanged"
		$BIN/fm6000_fullreplay "$FWD" $B ${PACE:-1500000} >> $LOG 2>&1
	fi
else
	$BIN/fm6000_fullreplay "$FWD" $B ${PACE:-1500000} >> $LOG 2>&1
fi
say "  rc=$? PIN=$(R 0x1c021) PORT_STATUS=$(R 0xe3800) pcsRx=$(R 0xe3826) sched=$(R 0x8062)"

say "STEP6 SFP laser"
V=$(S 0x5010); scdreg 0x5010 $(printf '0x%x' $(( 0x$V & ~0x40 ))) >/dev/null 2>&1
say "  0x5010 $V -> $(S 0x5010)"

say "STEP7 settle + link"
i=1; while [ $i -le 8 ]; do sleep 3
  say "  t=$((i*3))s PORT_STATUS=$(R 0xe3800) pcsRx=$(R 0xe3826) 0xe383f=$(R 0xe383f) PIN=$(R 0x1c021)"
  i=$((i+1)); done
say "=== FULLSEQ DONE ==="
