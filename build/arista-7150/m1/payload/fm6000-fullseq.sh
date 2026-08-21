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

# --- provenance accounting -------------------------------------------------
# "writes remain" measures the FILE LENGTH, not where the writes came from, and
# gen_split/gen_list REPLACE what they remove -- so a generator that emits as
# many writes as it deletes leaves that number flat and looks like it did
# nothing. FFU-BST did exactly that: 8,046 writes moved from fwd4.txt to our own
# tool with no visible change in the count, and none in a pair-diff either,
# because a table's default value is the same whoever writes it.
#
# GENW counts writes in the executed file that came from a GENERATOR. That is
# the number Goal B in docs/BLOB-REMOVAL-PLAN.md actually cares about.
GENW=0
DIRECTW=0        # subset of GENW applied by direct MMIO, not present in the file
gen_emit() {           # $1 = generator, $2.. = args; appends to /tmp/gen.head
	_gtool=$1; shift
	$BIN/$_gtool "$@" > /tmp/gen.out 2>/dev/null || return 1
	GENW=$((GENW + $(wc -l < /tmp/gen.out)))
	cat /tmp/gen.out >> /tmp/gen.head
	rm -f /tmp/gen.out
	return 0
}
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

# STEP4 microcode -- NOT NEEDED. ucode_l2.raw / ucode_tail.raw are no longer an
# operator dependency.
#
# Every one of the 39,415 registers those files write is also written by the
# replay (97.8% to the identical value; the rest the replay overwrites later
# anyway), so the load is redundant -- PROVIDED the microcode blocks land at the
# right point.
#
# That proviso is the whole story, and it exposed a real bug in our own
# generators. Bisected 2026-08-08:
#
#   pure replay,              no STEP4  -> WORKS, 0% loss / 8 rounds
#   generators at loop end,   no STEP4  -> routes=2, et1 rx=0, ping 100%
#   generators placed EARLY,  no STEP4  -> WORKS, 0% loss / 10 rounds
#
# PARSER/L2AR/MOD/MAPPER are written EARLY by the replay, before the port
# bring-up depends on them. gen_list was moving them to the loop end, which is
# too late -- and the defect was invisible because STEP4 separately loaded the
# same registers early. The generators were leaning on the very file they were
# supposed to make unnecessary. gen_list_early fixes the placement.
#
# UCODE=1 restores the load if you want to compare.
if [ "${UCODE:-0}" = "1" ]; then
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
	gen_emit "$2" -n || return 1
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
# gen_drop: remove a generator's addresses from the replay and DO NOT splice its
# output back. The tool is instead run in direct-MMIO mode later, so those writes
# never live in a file at all -- this is the only helper that actually SHRINKS the
# replay rather than changing who authored its lines.
#
# ⚠ ONLY VALID FOR ORDER-INSENSITIVE BLOCKS. gen_split/gen_list exist because
# where a block lands in the sequence changes the outcome (L2ARSEQ: links up,
# ARP fine, unicast 100% loss; L2F+LBS: must be last). A block qualifies here only
# if its addresses receive the same value no matter when it runs -- which for
# FFU-BST is true by construction: its runs were trimmed to addresses that only
# ever receive the default pair.
# drop_range: delete a raw address range from the replay with NO generator behind
# it -- for configuration EdgeNOS does not implement. Unlike gen_drop nothing
# replaces these writes; they simply stop happening.
#
# ⚠ This is DELETION, not generation. Only valid where the block configures a
# feature we do not use, and only ever with a soak behind it: an absent default
# rule can change forwarding in ways a single boot will not show.
drop_range() {         # $1 = low addr (8 hex, no 0x), $2 = high, $3 = label
	# ⚠ NO strtonum() -- it is a gawk extension and busybox awk does not have it.
	# The replay's addresses are fixed-width lowercase hex, so a plain STRING
	# comparison orders them correctly. An earlier version used strtonum, did
	# nothing at all on the switch, and printed no error: the range simply was
	# not dropped and the provenance total was unchanged.
	awk -v lo="$1" -v hi="$2" '{ if ($1 < lo || $1 > hi) print }' "$CUR" > "$NEXT" || return 1
	say "  $3 range dropped ($(wc -l < "$NEXT") writes remain, nothing replaces it)"
	CUR="$NEXT"; NEXT=/tmp/fwd.b; [ "$CUR" = /tmp/fwd.b ] && NEXT=/tmp/fwd.a
	return 0
}

gen_drop() {           # $1 = generator, $2 = label
	$BIN/$1 -a > /tmp/gen.addr 2>/dev/null || return 1
	[ -s /tmp/gen.addr ] || return 1
	awk 'NR==FNR { d[$1]; next } !($1 in d)' /tmp/gen.addr "$CUR" > "$NEXT" || return 1
	# these writes ARE ours -- the tool applies them by direct MMIO. Count them,
	# and count them separately so the report can state the true total: the chip
	# receives (executed file + directly applied), not just the file.
	_d=$(wc -l < /tmp/gen.addr); GENW=$((GENW + _d)); DIRECTW=$((DIRECTW + _d))
	rm -f /tmp/gen.addr
	say "  $2 dropped from the replay ($(wc -l < "$NEXT") writes remain, applied directly later)"
	CUR="$NEXT"; NEXT=/tmp/fwd.b; [ "$CUR" = /tmp/fwd.b ] && NEXT=/tmp/fwd.a
	return 0
}

gen_list() {           # $1 = generator, $2 = label
	$BIN/$1 -a > /tmp/gen.addr 2>/dev/null || return 1
	[ -s /tmp/gen.addr ] || return 1
	_a1=$(grep -n '^001a0c00 ' "$CUR" | tail -1 | cut -d: -f1)
	[ -n "$_a1" ] || return 1
	awk 'NR==FNR { d[$1]; next } !($1 in d)' /tmp/gen.addr "$CUR" > /tmp/gen.body
	_a1=$(grep -n '^001a0c00 ' /tmp/gen.body | tail -1 | cut -d: -f1)
	head -n $((_a1 - 1)) /tmp/gen.body > /tmp/gen.head
	gen_emit "$1" -n || return 1
	tail -n +$_a1 /tmp/gen.body >> /tmp/gen.head
	mv /tmp/gen.head "$NEXT"; rm -f /tmp/gen.addr /tmp/gen.body
	say "  $2 generated by us ($(wc -l < "$NEXT") writes remain)"
	CUR="$NEXT"; NEXT=/tmp/fwd.b; [ "$CUR" = /tmp/fwd.b ] && NEXT=/tmp/fwd.a
	return 0
}

# gen_list variant that inserts at the block's FIRST recorded write instead of
# at the end of the loop.
#
# ⚠ This distinction is load-bearing for the microcode blocks. PARSER, L2AR, MOD
# and MAPPER are written EARLY by the replay, before the port bring-up depends
# on them. gen_list moves them to the loop end, which is too late -- and that
# defect was invisible because STEP4 separately loaded the same registers early
# from ucode_*.raw. Proven by bisect:
#
#   pure replay,        no STEP4  -> works, 0% loss on 8 rounds
#   generators (late),  no STEP4  -> routes=2, et1 rx=0, ping 100%
#
# So the generators were relying on the microcode load they were supposed to be
# making unnecessary.
# Lift a block's PRE-LOOP writes only, leaving every in-loop write untouched.
#
# L2AR needed this. It is two things in one address range: ~25,400 writes before
# the 0x1a0c00 loop (a bulk table load, in two contiguous runs) and 3,684 inside
# it, in small bursts interleaved with the port bring-up. Lifting the whole
# block moved the in-loop bursts and killed unicast forwarding; lifting only the
# write-once registers reached just 4,606. Splitting on the loop boundary takes
# the bulk and leaves the interleaved part alone.
gen_preloop() {        # $1 = prefix to drop, $2 = generator, $3 = label
	_a0=$(grep -n '^001a0c00 ' "$CUR" | head -1 | cut -d: -f1)
	[ -n "$_a0" ] || return 1
	head -n $((_a0 - 1)) "$CUR" | grep -v "^$1" > /tmp/gen.head
	gen_emit "$2" -n || return 1
	tail -n +$_a0 "$CUR" >> /tmp/gen.head
	mv /tmp/gen.head "$NEXT"
	say "  $3 pre-loop generated by us ($(wc -l < "$NEXT") writes remain)"
	CUR="$NEXT"; NEXT=/tmp/fwd.b; [ "$CUR" = /tmp/fwd.b ] && NEXT=/tmp/fwd.a
	return 0
}

gen_list_early() {     # $1 = generator, $2 = label
	$BIN/$1 -a > /tmp/gen.addr 2>/dev/null || return 1
	[ -s /tmp/gen.addr ] || return 1
	_f=$(awk 'NR==FNR { d[$1]; next } ($1 in d) { print FNR; exit }' /tmp/gen.addr "$CUR")
	[ -n "$_f" ] || return 1
	head -n $((_f - 1)) "$CUR" > /tmp/gen.head
	gen_emit "$1" -n || return 1
	# No process substitution: this runs under busybox ash.
	tail -n +$_f "$CUR" > /tmp/gen.rest
	awk 'NR==FNR { d[$1]; next } !($1 in d)' /tmp/gen.addr /tmp/gen.rest >> /tmp/gen.head
	mv /tmp/gen.head "$NEXT"; rm -f /tmp/gen.addr /tmp/gen.rest
	say "  $2 generated by us, early ($(wc -l < "$NEXT") writes remain)"
	CUR="$NEXT"; NEXT=/tmp/fwd.b; [ "$CUR" = /tmp/fwd.b ] && NEXT=/tmp/fwd.a
	return 0
}

gen_after() {          # $1 = prefix to drop, $2 = generator, $3 = label
	_a1=$(grep -n '^001a0c00 ' "$CUR" | tail -1 | cut -d: -f1)
	[ -n "$_a1" ] || return 1
	head -n $((_a1 - 1)) "$CUR" | grep -v "^$1" > /tmp/gen.head
	gen_emit "$2" -n || return 1
	tail -n +$_a1 "$CUR" | grep -v "^$1" >> /tmp/gen.head
	mv /tmp/gen.head "$NEXT"
	say "  $3 generated by us ($(wc -l < "$NEXT") writes remain)"
	CUR="$NEXT"; NEXT=/tmp/fwd.b; [ "$CUR" = /tmp/fwd.b ] && NEXT=/tmp/fwd.a
	return 0
}


# ---- STANDALONE: run every generator directly, with no replay file ----------
#
# ★ WHY THIS EXISTS. Until now the whole bring-up was a TRANSFORMATION OF THE
# VENDOR FILE: gen_list filters our addresses out of the replay and splices our
# writes into its line stream at an anchor. That means the generators were
# substitutions inside somebody else's sequence -- remove fwd4.txt and nothing
# ran at all, however high the "provenance" number got. A 98.6% figure describes
# how many of the executed writes are ours; it does NOT mean the switch can boot
# without the file. This mode is what closes that gap.
#
# The order below is not invented. It is the order the splices already imply,
# extracted from the gen_list/gen_list_early/gen_drop/gen_split calls in this
# script, so a standalone boot performs the same blocks in the same sequence the
# working boot does.
#
# ⚠ WHAT THIS DOES NOT DO. It does not supply the residual writes that no
# generator covers (~1,800 at the time of writing: SBUS's indirect port, FFU's
# multi-write remainder, and the monotonic bitmaps that accumulate as ports come
# up). Much of that residual is runtime state which the hardware produces itself
# once configured -- the L2L sweeper is the clearest case -- so the open question
# this mode exists to ANSWER is how much of it actually matters at boot.
# Treat a standalone boot as an experiment until it is shown to forward.
STANDALONE_ORDER="
fm6000_cminit fm6000_safinit fm6000_ffuinit fm6000_l2linit
fm6000_parserinit fm6000_modinit fm6000_eplseq fm6000_l2arseq
fm6000_l2arpre fm6000_l2arinit fm6000_mapperpre fm6000_mgmt2pre
fm6000_hashinit fm6000_cmwm fm6000_mapper fm6000_smalltables
fm6000_cmrest fm6000_parserfields fm6000_esched fm6000_modports
fm6000_erl fm6000_sweeperinit fm6000_cmminit fm6000_monitorinit
fm6000_statsarinit fm6000_eaclinit fm6000_laginit fm6000_glortinit
fm6000_tbl3init fm6000_crmdrop fm6000_l3arinit fm6000_l3arslice1
fm6000_l3arslice4 fm6000_l3arslice3 fm6000_l3arslice2 fm6000_l3artables
fm6000_sweepinit fm6000_mgmt2init fm6000_eplinit fm6000_mapperinit
fm6000_ffubstinit
"
run_standalone() {
	_ran=0; _miss=0; _bad=0
	for _t in $STANDALONE_ORDER; do
		if [ -x "$BIN/$_t" ]; then
			# ⚠ TWO ARGUMENT CONVENTIONS. The older tools take the BDF as a
			# bare positional; the generators written later take "-b <bdf>"
			# and answer a bare one with usage + exit 2. Passing the wrong
			# form makes a generator look like it RAN when it did nothing --
			# the first standalone boot had 14 of 41 silently no-op that way.
			# Try the flag form, and fall back on exit 2 only.
			$BIN/$_t -b $B >> $LOG 2>&1
			_rc=$?
			if [ "$_rc" -eq 2 ]; then
				$BIN/$_t $B >> $LOG 2>&1
				_rc=$?
			fi
			_ran=$((_ran + 1))
			[ "$_rc" -ne 0 ] && { _bad=$((_bad + 1)); say "    $_t rc=$_rc"; }
		else
			_miss=$((_miss + 1))
		fi
	done
	say "  STANDALONE: ran $_ran generators directly ($_bad non-zero, $_miss absent)"
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
		{ [ "${FFU_DIRECT:-1}" = "1" ] && gen_drop fm6000_ffuinit FFU || gen_list fm6000_ffuinit FFU; }
	# L2L: the purest table in the replay -- 24,568 of 24,592 registers are
	# written exactly once. Same write-once split as FFU.
	# L2L is WRITE-ONCE: measured against the replay, all 24,568 of its addresses
	# receive exactly one distinct value, so writing them early lands identical
	# state and the block is a gen_drop candidate. L2L_DIRECT=1 drops the lines
	# and applies them by direct MMIO pre-replay; =0 keeps the old splice.
	# ⚠ Write-once is NECESSARY, not sufficient -- a block reset during the replay
	# would wipe an early write. That is what the soak is for.
	if [ "${L2LGEN:-1}" = "1" ] && [ -x "$BIN/fm6000_l2linit" ]; then
		if [ "${L2L_DIRECT:-1}" = "1" ]; then
			gen_drop fm6000_l2linit L2L
		else
			gen_list fm6000_l2linit L2L
		fi
	fi
	# Second tranche -- all lookup state the pipeline reads. Each keeps its
	# multi-write control registers in the replay (gen_list, write-once only).
	# TRANCHE2=0 to drop the whole set if one of them regresses.
	if [ "${TRANCHE2:-1}" = "1" ]; then
		# Microcode-adjacent blocks go EARLY (see gen_list_early); the rest
		# can land at the loop end.
		for _g in parser mod; do
			[ -x "$BIN/fm6000_${_g}init" ] && \
				gen_list_early "fm6000_${_g}init" "$(echo $_g | tr a-z A-Z)"
		done
		# EPL: emit the exact recorded SEQUENCE at its first write, rather
		# than collapsing it. EPLSEQ=1 to try; off by default until proven.
		[ "${EPLSEQ:-1}" = "1" ] && [ -x "$BIN/fm6000_eplseq" ] && \
			gen_list_early fm6000_eplseq EPL
		# L2AR: sequence relocation FAILS -- off by default.
		#
		# L2AR is 84% multi-write, so the EPL treatment looked like it should
		# apply: lift the whole 29,110-write sequence instead of only the 4,606
		# write-once registers. Cold-boot tested 2026-08-08 placed early:
		#
		#   links up 0xcc0/0x8c0, OSPF fine (35 routes, rx growing 95->138),
		#   ARP resolves -- but unicast ping is 100% loss across 14 rounds.
		#
		# So multicast punt survives and unicast forwarding does not. L2AR is L2
		# ACTION RESOLUTION: it decides forward/trap/drop, and its interleaving
		# with the rest of the loop evidently matters in a way EPL's does not.
		# Being a sequence made it a candidate; it did not make it relocatable.
		#
		# The write-once generator (4,606 registers) works and stays the
		# default. L2ARSEQ=1 to retry the full sequence.
		if [ "${L2ARSEQ:-0}" = "1" ] && [ -x "$BIN/fm6000_l2arseq" ]; then
			gen_list_early fm6000_l2arseq L2AR
		elif [ "${L2ARPRE:-1}" = "1" ] && [ -x "$BIN/fm6000_l2arpre" ]; then
			gen_preloop '0014' fm6000_l2arpre L2AR
		else
			[ -x "$BIN/fm6000_l2arinit" ] && gen_list_early fm6000_l2arinit L2AR
		fi
		# MAPPER and MGMT2 take the same pre-loop split as L2AR: 5,662 of
		# MAPPER's 6,644 writes and all 1,541 of MGMT2's are before the loop.
		# The write-once generators only reached 361 and 502 respectively.
		[ "${PRESPLIT:-1}" = "1" ] && [ -x "$BIN/fm6000_mapperpre" ] && \
			gen_preloop '0012' fm6000_mapperpre MAPPER
		# MGMT2 pre-split: OFF pending bisect -- it holds chip-level control
		# (PIN lives at 0x1c021) and coldreplay writes this range during clock
		# and BOOT_CTRL setup, so relocating it is far more invasive than a
		# table lift.
		[ "${MGMT2PRE:-0}" = "1" ] && [ -x "$BIN/fm6000_mgmt2pre" ] && \
			gen_preloop '0001[cdef]' fm6000_mgmt2pre MGMT2

		# Small control blocks, surveyed 2026-08-08. EACL and LAG are 100%
		# write-once, GLORT 97%, STATS_AR 97%; MGMT2/MONITOR are mixed and
		# SWEEPER/CMM are mostly control, so --mode once lifts only the safe
		# part of each. SMALLGEN=0 drops the whole set.
		# HASH: 2,048 addresses, all in the replay, none multi-valued. It was
		# built into the image but never wired in, so this is a new lift rather
		# than a conversion.
		[ "${HASH_DIRECT:-1}" = "1" ] && [ -x "$BIN/fm6000_hashinit" ] && \
			gen_drop fm6000_hashinit HASH
		if [ "${SMALLGEN:-1}" = "1" ]; then
			# All seven measured WRITE-ONCE against the replay (no address
			# receives a second value), so they are gen_drop candidates: the
			# lines leave the replay and the tool writes them directly.
			# CM watermarks -- the six per-port tables (0x112800-0x115fff), 6,512
		# writes. fm6000_cmminit covers only 72 addresses; these tables were
		# untouched. Byte-verified against the image in both directions.
		# ⚠ Watermarks decide when the chip drops and when it PAUSEs; a wrong
		# value shows up as loss under load, not as a failed transit test.
		# ⚠ gen_list_early, NOT gen_list. Measured 2026-08-21: with gen_list the
		# watermarks land at the END of the replay loop and the switch drops
		# ~2.8% of frames under load (alpha36 forwards 2000/2000; alpha37 forwards
		# 1934/1951/1946). The values are byte-identical to EOS's, so the fault is
		# WHEN they are written, not what -- the same defect gen_list_early exists
		# to fix for the microcode blocks. Watermarks must be in place before the
		# ports start forwarding.
		[ "${CMWM:-1}" = "1" ] && [ -x "$BIN/fm6000_cmwm" ] && \
			gen_list_early fm6000_cmwm CM-watermarks
		# MAPPER: the per-port QoS maps (identity), SRC_PORT_TABLE, MAC CAMs and
		# L4 compares. 6,283 replayed writes over 565 addresses; byte-verified.
		[ "${MAPPERTBL:-1}" = "1" ] && [ -x "$BIN/fm6000_mapper" ] && \
			gen_list fm6000_mapper MAPPER-tables
		# Small write-once tables: L2F forwarding (4K/256/profile), LBS loopback
		# suppression, ALU command/operand, policer QoS maps, SSCHED rings and
		# init handshakes. 833 replayed writes over 833 addresses; byte-verified.
		# Addresses are COMPUTED from the SDK geometry, not transcribed -- see
		# asic/fm6000/tools/gen_smalltables.py.
		# ⚠ gen_list_early, NOT gen_list. This set contains SSCHED_TX/RX_INIT_TOKEN
		# and _INIT_COMPLETE, which are scheduler init handshakes, and the L2F
		# forwarding tables. All of it must be in place before ports forward, so
		# moving it to the loop end is the same mistake the CM watermarks made --
		# one that shows up as loss under load rather than as a failed transit.
		[ "${SMALLTBL:-1}" = "1" ] && [ -x "$BIN/fm6000_smalltables" ] && \
			gen_list_early fm6000_smalltables small-tables
		# CM remainder: the traffic-class / port-class / memory-partition MAPS
		# and the shared-partition watermarks and pause thresholds -- the 397
		# write-once CM addresses fm6000_cmwm and fm6000_cminit both leave
		# uncovered. Byte-verified 397/397; addresses computed from the SDK
		# geometry (asic/fm6000/tools/gen_cmrest.py).
		# ⚠ gen_list_early for the same reason as the watermarks: these decide
		# when the chip drops and when it PAUSEs, so they must be in place
		# before the ports start forwarding. Writing them at the loop end is
		# the defect gen_list_early exists to avoid.
		[ "${CMREST:-1}" = "1" ] && [ -x "$BIN/fm6000_cmrest" ] && \
			gen_list_early fm6000_cmrest CM-rest
		# PARSER_INIT_FIELDS: the per-port parser seed (source GLORT + configured
		# flags), 194 write-once addresses fm6000_parserinit leaves uncovered.
		# Byte-verified 194/194; addresses computed from the SDK geometry
		# (asic/fm6000/tools/gen_parserfields.py).
		# ⚠ gen_list_early, matching the rest of PARSER: the parser is
		# microcode-adjacent and its seed must be in place before frames are
		# parsed, not written at the end of the replay loop.
		[ "${PARSERFLD:-1}" = "1" ] && [ -x "$BIN/fm6000_parserfields" ] && \
			gen_list_early fm6000_parserfields PARSER-fields
		# ESCHED: the egress scheduler's per-port config. One rule, not a table:
		# CFG_1 = CFG_2 = 0xffffff for each of the 52 front-panel data ports
		# (the configured set minus the CPU/management ports 0, 1, 3).
		# Byte-verified 104/104, and the derived port set checked BOTH ways --
		# no emitted address the replay never wrote, none left unclaimed.
		# ⚠ gen_list_early: scheduler credit must be set before ports forward.
		[ "${ESCHEDCFG:-1}" = "1" ] && [ -x "$BIN/fm6000_esched" ] && \
			gen_list_early fm6000_esched ESCHED-cfg
		# MOD per-front-panel-port frame settings: MIN_LENGTH = 64 bytes and
		# TX_PORT_TAG = 0 (no egress tag), for the 52 data ports. A rule, not a
		# table. Byte-verified 104/104 with the derived port set checked both
		# ways (asic/fm6000/tools/gen_modports.py).
		[ "${MODPORTS:-1}" = "1" ] && [ -x "$BIN/fm6000_modports" ] && \
			gen_list_early fm6000_modports MOD-ports
		# ERL: the egress rate limiter, INCLUDING its two-phase init. This is
		# the first block with no write-once part at all -- every one of its 967
		# addresses is written twice, 636 with two different values -- so it is
		# emitted as an ordered SEQUENCE, not collapsed to a final value:
		#   phase 1  0x40001000 to all 76 ports x 12 classes
		#   phase 2  0x80001000 to the 52 front-panel ports; port 0 shaped
		# Verified 967/967 byte-identical AND every address's two-write value
		# sequence checked against the replay (gen_erl.py --counts).
		# ⚠ gen_list_early puts phase 1 back where EOS had it (~line 9,559) and
		# moves phase 2 ~57,000 writes earlier. End state identical, phase order
		# per address preserved; the rate limits simply install sooner. Because
		# this is a RATE LIMITER, it is validated under load against the EOS
		# reference, not by a single ping.
		[ "${ERLCFG:-1}" = "1" ] && [ -x "$BIN/fm6000_erl" ] && \
			gen_list_early fm6000_erl ERL
		for _g in sweeper cmm monitor statsar eacl lag glort; do
				[ -x "$BIN/fm6000_${_g}init" ] || continue
				if [ "${SMALL_DIRECT:-1}" = "1" ]; then
					gen_drop "fm6000_${_g}init" "$(echo $_g | tr a-z A-Z)"
				else
					gen_list "fm6000_${_g}init" "$(echo $_g | tr a-z A-Z)"
				fi
			done
		fi
		# Three tables selected by NAME rather than address range (regmap.py):
		# HASH_LAYER3_PTABLE, PARSER_INIT_STATE, CM_PORT_TXMP_IP_WM. Each is
		# written more than once per register so the write-once filters skipped
		# them, but the repeats carry changed values rather than strobes, so the
		# final value is what matters. CAMs are deliberately excluded -- they are
		# paired with the 0x3f0000 commit strobe.
		[ "${TBL3:-1}" = "1" ] && [ -x "$BIN/fm6000_tbl3init" ] && \
			gen_list fm6000_tbl3init TBL3
		# FFU BST_ACTION default fill: 8,046 writes carrying two values only,
		# 0x00700000 on even words and 0 on odd, across four contiguous runs.
		# That is a table memset, not a program, so generating it serves BOTH
		# goals (docs/BLOB-REMOVAL-PLAN.md) -- unlike re-encoding EOS's rules.
		# Verified offline by SIMULATING gen_list: the set it removes is exactly
		# the set this tool emits (8,046 pairs, values 0x700000 and 0 only).
		# ⚠ gen_list filters by ADDRESS, so the runs exclude the 98 addresses
		# that also receive route content -- without that trim it stripped 8,276
		# lines to replace 8,144 and would have emptied real route entries.
		# ⛔ FFU-BST REMOVED. fm6000_ffubstinit generated the BST default fill --
		# 8,046 writes of two values -- and every one of its addresses is a SUBSET
		# of fm6000_ffuinit's 8,680, with IDENTICAL values. ffuinit already covered
		# them. It only ever looked like it removed 8,046 lines because it ran
		# after ffuinit's splice and undid part of it; once ffuinit moved to
		# gen_drop it removed nothing and merely wrote the same addresses twice,
		# which is where the +8,046 accounting drift came from.
		# Kept in the tree as a standalone tool; not wired into the boot path.
		# CRM command interface: DROPPED, not generated. It drives the CRM
		# memory-fill engine, which fm6000_memfill replaced -- dead code on our
		# boot path. gen_list removes the addresses and the tool emits nothing.
		[ "${CRMDROP:-1}" = "1" ] && [ -x "$BIN/fm6000_crmdrop" ] && \
			gen_list fm6000_crmdrop CRM-drop
		# ⚠ hash was here. It is now lifted by gen_drop earlier in the chain, and
		# leaving it in this loop RE-SPLICED the 2,048 lines that had just been
		# dropped -- the replay grew by 2,048 and the provenance total went wrong.
		# The tool name is built as "fm6000_${_g}init", so grepping for
		# "fm6000_hashinit" does not find this call site. l3ar stays: it has
		# multi-valued addresses and is NOT gen_drop-eligible.
		# ⛔⛔ L3AR SLICE DELETION -- DISABLED, PREMISE DISPROVEN BY THE DATASHEET.
		#
		# l3ar_program.py states slices 1-4 are "csGlort assignment, policers,
		# storm control and L3 QoS: separate functions EdgeNOS does not author".
		# That claim carries NO citation, and the datasheet contradicts it. 5.10.1:
		#
		#     Total number of TCAM slice sets: 5
		#     Number of rules per precedence set: 32
		#     Number of serial application stages: 5
		#     "Changes accumulate serially from one application stage to the next."
		#
		# The five slices are SERIAL STAGES of one resolution, not five independent
		# feature blocks. Deleting slices 1-4 removes four fifths of L3 action
		# resolution.
		#
		# Measured: CAM-only deletion (alpha29) forwarded fine over 4 boots, which
		# is what made the wrong premise look right. Adding the action-RAM deletion
		# (alpha30) broke the dataplane outright -- both ports clean-locked, OSPF
		# down to 2 routes, unicast to the switch 100% loss. Same signature as the
		# L2ARSEQ failure.
		#
		# ⚠ And it reframes alpha29: deleting the CAM writes does NOT disable those
		# slices, it leaves their TCAM entries UNINITIALISED. Key=0/KeyInvert=0 is
		# the never-match state and has to be WRITTEN -- which is exactly why
		# l3ar_program.py emits explicit zero writes for the rules it does not
		# author. alpha29 worked because EOS's action RAM was still resident behind
		# whatever those uninitialised entries matched, not because the slices were
		# unused.
		#
		# To disable a slice properly: WRITE never-match keys, do not omit writes.
		# The ACTION RAMs for those same slices are unreachable once their CAM
		# entries are gone -- an action that nothing can select. Register header:
		#   RAM1 0x11200 stride 0x40   RAM2 0x11400 stride 0x40
		#   RAM3 0x11600 stride 0x20   RAM4 0x11800 stride 0x40
		#   RAM5 0x11A00 stride 0x40
		# Slices 1-4 only; slice 0 stays (it is the one we author) and the profile
		# tables at 0x11C00+ are GLOBAL, not per-slice, so they stay too.
		# ⛔ DEFAULT OFF. The premise for these deletions was WRONG -- see below.
		if [ "${L3ARCAM_DROP:-0}" = "1" ]; then
			drop_range 00010200 000109ff "L3AR-CAM-slices1-4"
			drop_range 00011240 0001133f "L3AR-RAM1-slices1-4"
			drop_range 00011440 0001153f "L3AR-RAM2-slices1-4"
			drop_range 00011620 0001169f "L3AR-RAM3-slices1-4"
			drop_range 00011840 0001193f "L3AR-RAM4-slices1-4"
			drop_range 00011a40 00011b3f "L3AR-RAM5-slices1-4"
		fi
		for _g in l3ar; do
			[ -x "$BIN/fm6000_${_g}init" ] && \
				gen_list "fm6000_${_g}init" "$(echo $_g | tr a-z A-Z)"
		done
		# L3AR slice 1 -- canonical source GLORT. Authored, not transcribed; see
		# docs/L3AR-STRUCTURE.md and asic/fm6000/tools/gen_l3ar_slice1.py.
		# 810 writes replacing 1,088 replay lines, 6 rules where EOS ships 32.
		#
		# No overlap with fm6000_l3arinit above: that tool is SLICE 0 ONLY and its
		# -a lists slice-0 addresses only, so the two address sets are disjoint.
		#
		# ⚠ This emits csGlort profile entries 5 and 10 and SGLORT entries 0 and 1.
		# Entry 5 (csGlort) and 1 (SGLORT) are ALSO selected by slice 2, which we
		# do not author, so they are emitted with the contents slice 2 expects --
		# interface conformance, not transcription. Changing them breaks slice 2.
		[ "${L3AR_SLICE1:-1}" = "1" ] && [ -x "$BIN/fm6000_l3arslice1" ] && \
			gen_list fm6000_l3arslice1 L3AR-slice1
		# L3AR slice 4 -- QoS classification + ALU46 operand select. Authored,
		# byte-verified against EOS's slice 4 (625/625 words). Disjoint from
		# fm6000_l3arinit (slice 0) and fm6000_l3arslice1: checked, no overlap.
		[ "${L3AR_SLICE4:-1}" = "1" ] && [ -x "$BIN/fm6000_l3arslice4" ] && \
			gen_list fm6000_l3arslice4 L3AR-slice4
		# L3AR slice 3 -- ALUs, policers, VID, W16ABC. Authored; 725/725 live
		# words byte-identical to EOS, 3 dead rules never-match on both sides.
		# Disjoint from slices 0, 1 and 4: checked, no overlap.
		[ "${L3AR_SLICE3:-1}" = "1" ] && [ -x "$BIN/fm6000_l3arslice3" ] && \
			gen_list fm6000_l3arslice3 L3AR-slice3
		# L3AR slice 2 -- VID assignment and trap. The last slice; with this,
		# all five are authored. 725/725 live words byte-identical to EOS,
		# 3 dead rules never-match on both sides, disjoint from 0/1/3/4.
		[ "${L3AR_SLICE2:-1}" = "1" ] && [ -x "$BIN/fm6000_l3arslice2" ] && \
			gen_list fm6000_l3arslice2 L3AR-slice2
		# L3AR slice 0's RAM3/RAM4/RAM5 + the 19 shared profile tables.
		# ⚠ fm6000_l3arinit emits slice 0's CAM and RAM1/RAM2 only -- it predates
		# the retraction of "an L3AR action is just a flag rewrite". Slice 0 is
		# the FORWARDING stage, so its L2-lookup/ALU/policer/QoS/GLORT muxes have
		# been coming from EOS's replay until now. 450 writes, byte-verified.
		[ "${L3AR_TABLES:-1}" = "1" ] && [ -x "$BIN/fm6000_l3artables" ] && \
			gen_list fm6000_l3artables L3AR-tables
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
# FFU-BST applied by direct MMIO, because gen_drop removed its 8,046 lines from
# the replay entirely -- the replay is that much SHORTER, not merely re-authored.
#
# ⚠ THIS MUST RUN BEFORE THE REPLAY, not after. FFU tables are committed by the
# ATOMIC_APPLY strobes at 0x3f0000, and all 16 BST commits happen DURING the
# replay. Applying the defaults afterwards would leave them uncommitted, with the
# BST holding uninitialised SRAM for the whole replay. Writing them first puts
# the table in the state the replay expects and lets its own strobes commit it.
if [ "${L2LGEN:-1}" = "1" ] && [ "${L2L_DIRECT:-1}" = "1" ] && [ -x "$BIN/fm6000_l2linit" ]; then
	$BIN/fm6000_l2linit $B >> $LOG 2>&1
	say "  L2L applied directly (pre-replay) rc=$?"
fi
# The rest of the write-once set, applied by direct MMIO for the same reason.
if [ "${FFU_DIRECT:-1}" = "1" ] && [ -x "$BIN/fm6000_ffuinit" ]; then
	$BIN/fm6000_ffuinit $B >> $LOG 2>&1; say "  FFU applied directly rc=$?"
fi
if [ "${HASH_DIRECT:-1}" = "1" ] && [ -x "$BIN/fm6000_hashinit" ]; then
	$BIN/fm6000_hashinit $B >> $LOG 2>&1; say "  HASH applied directly rc=$?"
fi
if [ "${SMALL_DIRECT:-1}" = "1" ]; then
	for _g in sweeper cmm monitor statsar eacl lag glort; do
		[ -x "$BIN/fm6000_${_g}init" ] && $BIN/fm6000_${_g}init $B >> $LOG 2>&1
	done
	say "  small write-once blocks applied directly"
fi
# STANDALONE takes precedence, and a missing replay selects it automatically --
# a boot with no vendor file should attempt the generators rather than do nothing.
if [ "${STANDALONE:-0}" = "1" ] || [ ! -s "$CUR" ]; then
	say "STEP5-ALT STANDALONE (no replay): generators only"
	run_standalone; RC=$?
else
	$BIN/fm6000_fullreplay "$CUR" $B ${PACE:-1500000} >> $LOG 2>&1; RC=$?
fi
# ⚠ KEEP the generated replay. fm6000_fullreplay's progress counters index THIS
# file, not /mnt/flash/fwd4.txt -- the generators filter blocks out and hand it a
# rewritten copy. Deleting it made every op number in the in-replay Et2 trace
# unresolvable: op 174,080 could not be mapped back to a write. Copy it to flash
# so a trace can be turned into actual register writes offline.
[ "$CUR" != "$FWD" ] && { cp "$CUR" /mnt/flash/fwd-executed.txt 2>/dev/null; sync; }
[ "$CUR" != "$FWD" ] && rm -f /tmp/fwd.a /tmp/fwd.b
say "  rc=$RC PIN=$(R 0x1c021) PORT_STATUS=$(R 0xe3800) pcsRx=$(R 0xe3826) sched=$(R 0x8062)"

# ---- STEP5b: SPICO SerDes firmware, loaded by US from a standalone blob -------
# The firmware is 30,002 SBus transactions = 90,006 writes = 41% of the replay,
# and it is Intel's SerDes microcode, not a table we can generate. Taking it OUT
# of fwd4.txt and pushing it from its own file makes the replay that much smaller
# and puts the firmware alongside ucode_l2.raw as a declared third-party input --
# which is what build-release-swi.sh already documents ("bring your own").
#
# ⚠ THIS ONLY WORKS AFTER THE REPLAY, AND ONLY ON A STRIPPED REPLAY.
# The comment at STEP2 says never to load SPICO with a separate step. That is
# correct for loading BEFORE a replay that still carries the upload -- the replay
# resets and starts the SPICO afterwards and runs it with an empty IMEM. Here the
# upload is stripped (tools/strip-spico.py) and we run LAST, so nothing overwrites
# us: fm6000_spico does its own reset -> upload -> run.
#
# ⚠ The replay still contains the 108 reg-0x0C SPICO control writes, so a stripped
# boot reaches this point having already started an EMPTY SPICO -- Et2 reads
# 0x815/0x0000. That is expected, and this step is what repairs it.
#
# Skipped silently when the blob is absent or the replay still has the firmware
# inline: a missing third-party file must degrade to "copper down", never a
# failed boot.
#
# The inline-firmware test greps the replay for the IMEM SBus command words. An
# SBus write is addressed to 0x0F001 with device+register in the VALUE:
#   cmd = reg | dev<<8 | op<<16 | Exec<<24  ->  dev 0xFD, regs 0x04-0x07, op 0x21
#        = 0121fd04 .. 0121fd07
# Measured: 30,002 matches in the full replay, 0 in the stripped one. Note reg
# 0x0C (0121fd0c) is the SPICO *control* op and is deliberately NOT matched -- it
# stays in the replay either way.
SPICO_BLOB="${SPICO_BLOB:-/mnt/flash/fm6000_spico_code.bin}"
# ⚠ PLACEMENT IS THE OPEN QUESTION. Loading + retraining right here (immediately
# after the replay, before the settle loop) gave Et2 up on 2 of 4 boots -- the
# same as stock. Doing exactly the same thing by hand AFTER FullSEQ finished gave
# 4 of 4. Neither sample separates them (p = 0.429), so this is a function and
# the call site is a variable: SPICO_AFTER_SETTLE=1 (default) runs it after the
# settle loop, =0 runs it here.
spico_step() {
if [ "${SPICOLOAD:-1}" = "1" ] && [ -f "$SPICO_BLOB" ] && [ -x "$BIN/fm6000_spico" ] &&
   ! grep -q '^0000f001 0121fd0[4-7]$' "$FWD" 2>/dev/null; then
	say "STEP5b SPICO firmware from $SPICO_BLOB (replay is stripped)"
	$BIN/fm6000_spico $B "$SPICO_BLOB" >> $LOG 2>&1
	say "  spico rc=$? PIN=$(R 0x1c021)"
	# the lane trained during the replay against an empty IMEM, so retrain it
	# RETRAIN UNTIL THE LOCK IS CLEAN, not merely locked.
	#
	# ⚠ LANE_STATUS 0x940 is necessary but NOT sufficient. fm6000_lanelink is a
	# known producer of HiBer locks: PORT_STATUS bit 8 set, pcsRx 0x67 instead of
	# 0x1, LANE_STATUS a solid 0x940 -- and the port forwards NOTHING. That is
	# exactly what alpha20 boot 5 hit: Et2 "up" 16/16 by LANE_STATUS, rx=0,
	# transit dead. Measured: repeating the retrain clears it (0x0CC0, pcsRx=1)
	# and traffic then flows.
	#
	# So loop on the three-part test, and treat a HiBer lock as a failure to
	# retry rather than a success to report.
	if [ -x "$BIN/fm6000_lanelink" ]; then
		_n=0
		while [ $_n -lt 6 ]; do
			_ps=$(R 0xe4000); _ls=$(R 0xe4038); _rx=$(R 0xe4026)
			# clean = block lock AND not HiBer AND pcsRx==1
			if [ "$_ls" = "00000940" ] && [ "$_rx" = "00000001" ] &&
			   [ $(( 0x$_ps & 0x100 )) -eq 0 ]; then break; fi
			_n=$((_n+1))
			$BIN/fm6000_lanelink 2 >> $LOG 2>&1
			sleep 12          # training is asynchronous
		done
		say "  et2 retrain attempts=$_n et2=$(R 0xe4000)/$(R 0xe4038) pcsRx=$(R 0xe4026)"

		# PORT 3 (Et3, EPL14 lane 1) -- cold SerDes enable attempt.
		#
		# ⚠ EOS's replay contains SerDes SBus ops for exactly TWO devices:
		# 0x49 (port 1) and 0x45 (port 2), 44 and 45 ops. Device 0x4a -- port 3 --
		# gets ZERO. Its EPL/MAC/PCS registers ARE written (391 of them), which is
		# why they compare byte-identical to a working lane; only the SerDes half
		# is missing. That asymmetry, not a misconfiguration, is why port 3 has
		# never linked. See docs/PORT3-BRINGUP.md.
		#
		# The two captured sequences are IDENTICAL in every written value and
		# differ only by one extra poll of reg 0x1f, so the data is lane
		# independent and fm6000_lanelink already issues it correctly to 0x4a.
		# Post-boot it does not bring the lane up. This runs it at boot instead,
		# which is the one condition never tested -- the chip here is freshly
		# programmed and no other lane is carrying traffic yet.
		#
		# ⛔ MEASURED AND IT DOES NOT WORK -- DEFAULT OFF. alpha33 ran exactly
		# this at boot, in the same phase where et2's retrain succeeds:
		#   [fs]   et3 attempts=3 et3=00000015/00000000 pcsRx=00000000
		# et1 and et2 came up clean on the same boot (0cc0/0940, 08c0/0940), so
		# it is harmless -- but it costs ~24s per boot and never links. Running
		# the captured sequence AT BOOT was the last untested condition, and it
		# is now ruled out: timing and context are not the missing piece.
		# The hook is kept for testing fm6000_serdes_enable when it exists.
		# Set PORT3=1 to re-enable.
		if [ "${PORT3:-0}" = "1" ] && [ -x "$BIN/fm6000_lanelink" ]; then
			_p3=0
			while [ $_p3 -lt 3 ]; do
				[ "$(R 0xe38b8)" = "00000940" ] && break
				_p3=$((_p3+1))
				$BIN/fm6000_lanelink 3 >> $LOG 2>&1
				sleep 8
			done
			say "  et3 attempts=$_p3 et3=$(R 0xe3880)/$(R 0xe38b8) pcsRx=$(R 0xe38a6)"
		fi
	fi
fi
}
[ "${SPICO_AFTER_SETTLE:-1}" = "1" ] || spico_step
# ⚠ Et2 is logged HERE as well as in STEP7, and the distinction is the whole
# question. Et1 arrives at STEP5 already linked (0x08c0/pcsRx=1) -- it does not
# come up during the settle loop, it is up before the loop starts. So for Et2,
# which links on only ~half of identical boots:
#
#   Et2 already up at STEP5   -> the divergence happens INSIDE the replay, and
#                                the settle loop is watching an outcome decided
#                                minutes earlier. That is why every register
#                                diff taken after settling looked identical.
#   Et2 climbs during STEP7   -> a training-time race.
#
# Those want completely different fixes and the STEP7-only trace cannot tell
# them apart. One line closes it.
say "        et2 PORT_STATUS=$(R 0xe4000) pcsRx=$(R 0xe4026) LANE_STATUS=$(R 0xe4038)"

say "STEP6 SFP laser"
V=$(S 0x5010); scdreg 0x5010 $(printf '0x%x' $(( 0x$V & ~0x40 ))) >/dev/null 2>&1
say "  0x5010 $V -> $(S 0x5010)"

say "STEP7 settle + link"
# ⚠ Et2 is sampled here as well as Et1, and that is not cosmetic. Et2's link
# comes up on roughly half of otherwise identical boots -- measured over 10
# controlled boots, see docs/PORT3-BRINGUP.md -- and at ~12 min a boot an A/B
# comparison needs about 31 boots PER ARM to detect even a 50-point difference.
# So the outcome cannot be chased by booting; it has to be caught happening.
# Every boot now leaves a trace of both ports, so a good boot and a bad one can
# be diffed after the fact for where they diverge.
#   Et1 EPL14 lane0  PORT_STATUS 0xe3800  pcsRx 0xe3826  LANE_STATUS 0xe3838
#   Et2 EPL16 lane0  PORT_STATUS 0xe4000  pcsRx 0xe4026  LANE_STATUS 0xe4038
i=1; while [ $i -le 8 ]; do sleep 3
  say "  t=$((i*3))s PORT_STATUS=$(R 0xe3800) pcsRx=$(R 0xe3826) 0xe383f=$(R 0xe383f) PIN=$(R 0x1c021)"
  say "        et2  PORT_STATUS=$(R 0xe4000) pcsRx=$(R 0xe4026) LANE_STATUS=$(R 0xe4038)"
  i=$((i+1)); done
say "  final et1=$(R 0xe3800)/$(R 0xe3838)  et2=$(R 0xe4000)/$(R 0xe4038)"
# default placement: after the settle loop, reproducing the manual sequence that
# measured 4/4. The line above therefore reports Et2 BEFORE the firmware load.
if [ "${SPICO_AFTER_SETTLE:-1}" = "1" ]; then
	spico_step
	say "  post-spico et1=$(R 0xe3800)/$(R 0xe3838)  et2=$(R 0xe4000)/$(R 0xe4038)"
fi
# ⚠ $CUR is GONE by now -- line 402 deletes /tmp/fwd.a|b after copying the
# executed file to flash. Read the preserved copy, and fall back to $CUR only if
# the copy was never made (CUR == FWD, i.e. no generator ran).
_tot=$(wc -l < /mnt/flash/fwd-executed.txt 2>/dev/null || wc -l < "$CUR" 2>/dev/null || echo 0)
# ⚠ the chip receives the executed FILE plus everything applied directly. Adding
# DIRECTW is what makes "ours / total" comparable across builds -- without it,
# moving a block from splice to direct made the un-generated count appear to GROW.
_tot=$((_tot + DIRECTW))
[ "${_tot:-0}" -gt 0 ] 2>/dev/null || _tot=0
if [ "$_tot" -gt 0 ]; then
	say "  provenance: $GENW of $_tot executed writes come from our generators ($((GENW * 100 / _tot))%)"
else
	say "  provenance: $GENW writes come from our generators (total unavailable)"
fi
say "=== FULLSEQ DONE ==="
