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

# STEP4 microcode. REQUIRED -- do not remove, despite appearances.
#
# ucode_l2.raw + ucode_tail.raw write 39,415 registers, and every single one of
# them is ALSO written by the replay: 38,564 (97.8%) to the identical value, and
# the other 851 to a value the replay overwrites later in STEP5 anyway. On paper
# the load is pure redundancy.
#
# It is not. Cold-boot tested 2026-08-08 with STEP4 skipped:
#
#     links came up 0xcc0/0x8c0, and the dataplane was dead --
#     routes=2, et1 rx=0, ping 100% loss on all 10 rounds.
#
# So writing the same registers to the same values is NOT equivalent to loading
# the microcode. fm6000_ucode_dbg's access sequence matters -- the TCAM/action
# SRAM evidently needs its own write protocol, not just the right end state.
# Same lesson as EPL (a procedure, not state) and the FFU commit strobes.
#
# UCODE=0 to skip it and reproduce the failure.
if [ "${UCODE:-1}" = "1" ]; then
	say "STEP4 microcode (parser/FFU/mapper + MOD)"
	fm6000_ucode_dbg $B /mnt/flash/ucode_l2.raw   /mnt/flash/u1.log >/dev/null 2>&1; say "  l2 rc=$? PIN=$(R 0x1c021)"
	fm6000_ucode_dbg $B /mnt/flash/ucode_tail.raw /mnt/flash/u2.log >/dev/null 2>&1; say "  tail rc=$? PIN=$(R 0x1c021)"
else
	say "STEP4 microcode SKIPPED (redundant: the replay writes all 39,415 registers)"
fi

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
# GENBLK: program whole register blocks with OUR OWN code instead of replaying
# EOS's writes for them. Each generator writes that block's END STATE; the
# recorded writes for the block are filtered out of the replay.
#
#   fm6000_cminit    CM      0x110000-0x11ffff   47,742 writes ->  8,180
#   fm6000_safinit   SAF     0x0a0000-0x0a0fff   34,668 writes ->    168
#   fm6000_sweepinit L2F+LBS 0x180000-0x1fffff   74,674 writes -> 74,382
#                            + 0x014000-0x014fff
#
# sweepinit is different in kind: it does not shrink the write count, it
# reproduces the 336-iteration port sweep from OUR port table instead of
# replaying EOS's. The LBS write paired with each L2F entry is very likely the
# commit for it (dropping LBS gives routes=1, rx=0), so the pairing is
# reproduced rather than collapsed to an end state.
#
# Both are spliced at the first IN-LOOP write for their block, so the writes
# that PRECEDE the 0x1a0c00 loop are kept verbatim (1,271 for CM, 171 for SAF).
# That is the arrangement that was cold-boot validated; a blanket grep -v over
# the whole file drops those too and yields a sequence nothing has booted.
# Verified offline: the end state of all 93,662 registers is unchanged.
#
# GENBLK=0 replays EOS's writes unchanged.
gen_split() {          # $1 = prefix to drop, $2 = generator, $3 = label
	_a0=$(grep -n '^001a0c00 ' "$CUR" | head -1 | cut -d: -f1)
	[ -n "$_a0" ] || return 1
	_f=$(tail -n +$_a0 "$CUR" | grep -n "^$1" | head -1 | cut -d: -f1)
	[ -n "$_f" ] || return 1
	_f=$((_a0 + _f - 1))
	head -n $((_f - 1)) "$CUR" > /tmp/gen.head
	tail -n +$_f "$CUR" | grep -v "^$1" > /tmp/gen.tail
	$BIN/$2 -n >> /tmp/gen.head 2>/dev/null || return 1
	cat /tmp/gen.head /tmp/gen.tail > /tmp/gen.new && mv /tmp/gen.new "$NEXT"
	rm -f /tmp/gen.head /tmp/gen.tail
	say "  $3 generated by us ($(wc -l < "$NEXT") writes remain)"
	CUR="$NEXT"; NEXT=/tmp/fwd.b; [ "$CUR" = /tmp/fwd.b ] && NEXT=/tmp/fwd.a
	return 0
}

# Like gen_split, but the generated writes go AFTER the last loop iteration
# instead of at the block's first in-loop write. That distinction is the whole
# reason L2F/LBS works: the sweep is EOS recomputing the port map after every
# port state change, so the map must land once the ports are configured, not
# before. Hoisting it to the front produced routes=2, rx=2, ping 100%.
# ⚠ ORDERING IS LOAD-BEARING. Both helpers locate the loop by grepping for the
# anchor line '001a0c00', and that address is itself an L2F register -- so the
# L2F filter DELETES IT. Any generator that runs after L2F therefore cannot find
# the loop and splices at an arbitrary point, silently. That is not theoretical:
# it produced an FFU "result" whose replay grew by 5,152 writes.
#
# So L2F must stay LAST in the chain. Anything added later needs a stable anchor
# instead of this one.
# Filter by an EXPLICIT ADDRESS LIST from the generator itself (-a), not a
# prefix. Needed where a block is part table, part control: FFU has 1,963
# multi-write registers, some of them commit strobes (0x3f0000 pulses 59 times),
# and those must keep their sequence. Removing them by prefix performs one
# commit instead of 59 and the CPU-punt traps never apply.
gen_list() {           # $1 = generator, $2 = label
	$BIN/$1 -a > /tmp/gen.addr 2>/dev/null || return 1
	[ -s /tmp/gen.addr ] || return 1
	_a1=$(grep -n '^001a0c00 ' "$CUR" | tail -1 | cut -d: -f1)
	[ -n "$_a1" ] || return 1
	awk 'NR==FNR { d[$1]; next } !($1 in d)' /tmp/gen.addr "$CUR" > /tmp/gen.body
	_a1=$(grep -n '^001a0c00 ' /tmp/gen.body | tail -1 | cut -d: -f1)
	head -n $((_a1 - 1)) /tmp/gen.body > /tmp/gen.head
	$BIN/$1 -n >> /tmp/gen.head 2>/dev/null || return 1
	tail -n +$_a1 /tmp/gen.body >> /tmp/gen.head
	mv /tmp/gen.head "$NEXT"; rm -f /tmp/gen.addr /tmp/gen.body
	say "  $2 generated by us ($(wc -l < "$NEXT") writes remain)"
	CUR="$NEXT"; NEXT=/tmp/fwd.b; [ "$CUR" = /tmp/fwd.b ] && NEXT=/tmp/fwd.a
	return 0
}

gen_after() {          # $1 = prefix to drop, $2 = generator, $3 = label
	_a1=$(grep -n '^001a0c00 ' "$CUR" | tail -1 | cut -d: -f1)
	[ -n "$_a1" ] || return 1
	head -n $((_a1 - 1)) "$CUR" | grep -v "^$1" > /tmp/gen.head
	$BIN/$2 -n >> /tmp/gen.head 2>/dev/null || return 1
	tail -n +$_a1 "$CUR" | grep -v "^$1" >> /tmp/gen.head
	mv /tmp/gen.head "$NEXT"
	say "  $3 generated by us ($(wc -l < "$NEXT") writes remain)"
	CUR="$NEXT"; NEXT=/tmp/fwd.b; [ "$CUR" = /tmp/fwd.b ] && NEXT=/tmp/fwd.a
	return 0
}

CUR="$FWD"; NEXT=/tmp/fwd.a
if [ "${GENBLK:-1}" = "1" ]; then
	[ -x "$BIN/fm6000_cminit" ]  && gen_split '0011'  fm6000_cminit  CM
	[ -x "$BIN/fm6000_safinit" ] && gen_split '000a0' fm6000_safinit SAF
	# FFU: OFF. 14,549 -> 10,643 works for the DATAPATH but kills the OSPF
	# adjacency. Cold-boot tested 2026-08-07 in both placements (after the loop,
	# and hoisted to the block's first in-loop write): links come up 0x8c0 and
	# unicast forwarding is fine -- in fact ping ran 0/0/0/10% loss, far better
	# than the stock replay's collapse -- but routes stay at 2, i.e. no
	# adjacency, so OSPF hellos are not reaching the CPU.
	#
	# The FFU holds the CPU-punt traps, so the likely reading is that it is not
	# purely state: installing a trap has an ordering requirement the end state
	# does not capture. Note the side-observation though -- with FFU generated,
	# unicast loss did NOT collapse the way it does on the stock replay. That is
	# a lead on the pre-existing adjacency collapse, not just a failure.
	#
	# FFUGEN=1 to re-test.
	[ "${FFUGEN:-1}" = "1" ] && [ -x "$BIN/fm6000_ffuinit" ] && \
		gen_list fm6000_ffuinit FFU
	# L2L: the purest table in the replay -- 24,568 of 24,592 registers are
	# written exactly once. Same write-once split as FFU.
	[ "${L2LGEN:-1}" = "1" ] && [ -x "$BIN/fm6000_l2linit" ] && \
		gen_list fm6000_l2linit L2L
	# Second tranche -- all lookup state the pipeline reads. Each keeps its
	# multi-write control registers in the replay (gen_list, write-once only).
	# TRANCHE2=0 to drop the whole set if one of them regresses.
	if [ "${TRANCHE2:-1}" = "1" ]; then
		for _g in l2ar parser mod l3ar hash mapper; do
			[ -x "$BIN/fm6000_${_g}init" ] && \
				gen_list "fm6000_${_g}init" "$(echo $_g | tr a-z A-Z)"
		done
	fi

	# L2F+LBS end state, written ONCE AFTER the whole loop. Cold-boot validated
	# 2026-08-07: both links up, OSPF adjacency, 35 kernel routes, 13 in silicon
	# -- indistinguishable from the stock replay run back-to-back.
	[ -x "$BIN/fm6000_l2finit" ] && gen_after '\(001[89abcdef]\|00014\)' fm6000_l2finit L2F+LBS
	# EPL: OFF. 22,051 -> 1,027 looked like the biggest remaining win (21x
	# redundancy, 32 distinct values) and it WEDGES THE CHIP. Cold-boot tested
	# 2026-08-07: bring-up ran, then EPL reads returned 0xffffffff and the box
	# wedged hard enough for the watchdog to reboot it.
	#
	# The caveat written into fm6000_eplinit.c turned out to be the answer: EPL
	# IS the port bring-up, and the intermediate states are real -- a SerDes
	# needs the steps, not the destination. Unlike L2F/LBS this is not fixable
	# by moving where the writes land; the 21x redundancy is a sequence, not
	# repetition. EPLGEN=1 to re-test.
	[ "${EPLGEN:-0}" = "1" ] && [ -x "$BIN/fm6000_eplinit" ] && \
		gen_after '000[ef]' fm6000_eplinit EPL

	# fm6000_sweepinit: OFF -- it BREAKS FORWARDING. Cold-boot tested
	# 2026-08-07: link came up 0xcc0 but routes=2, rx=2, ping 100% loss.
	#
	# Why: unlike CM and SAF, the sweep cannot be hoisted. Each of the 336 loop
	# iterations is 220 sweep writes with 52 EPL/CM/L2L writes INTERLEAVED in the
	# middle, and gen_split moves every sweep write to one point -- so all
	# 336x52 per-port writes end up after the entire sweep instead of inside it.
	# The chip depends on that interleaving.
	#
	# Generating L2F/LBS therefore means generating the WHOLE loop body, per-port
	# work included, not just the sweep slice. SWEEPGEN=1 to re-test.
	[ "${SWEEPGEN:-0}" = "1" ] && [ -x "$BIN/fm6000_sweepinit" ] && \
		gen_split '\(001[89abcdef]\|00014\)' fm6000_sweepinit L2F+LBS
fi
$BIN/fm6000_fullreplay "$CUR" $B ${PACE:-1500000} >> $LOG 2>&1; RC=$?
[ "$CUR" != "$FWD" ] && rm -f /tmp/fwd.a /tmp/fwd.b
say "  rc=$RC PIN=$(R 0x1c021) PORT_STATUS=$(R 0xe3800) pcsRx=$(R 0xe3826) sched=$(R 0x8062)"

say "STEP6 SFP laser"
V=$(S 0x5010); scdreg 0x5010 $(printf '0x%x' $(( 0x$V & ~0x40 ))) >/dev/null 2>&1
say "  0x5010 $V -> $(S 0x5010)"

say "STEP7 settle + link"
i=1; while [ $i -le 8 ]; do sleep 3
  say "  t=$((i*3))s PORT_STATUS=$(R 0xe3800) pcsRx=$(R 0xe3826) 0xe383f=$(R 0xe383f) PIN=$(R 0x1c021)"
  i=$((i+1)); done
say "=== FULLSEQ DONE ==="
