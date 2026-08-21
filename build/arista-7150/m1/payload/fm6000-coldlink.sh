#!/bin/sh
# fm6000-coldlink.sh - COLD clean-room bring-up of Et1 (EPL14 / serdes 68) to a trained 10GBASE-SR link.
#
# Chain:  cold init + scheduler  ->  SBus  ->  SPICO  ->  SERDES_CFG RefSel  ->  coeff preset
#         ->  replay of the captured EOS bring-up window  ->  SFP laser  ->  poll PORT_STATUS
#
# The window replay (fm6000_linkup) is LIVE-VALIDATED WARM: run against an EOS-shut port (which is
# register-for-register the cold state) it produces PORT_STATUS=0x8c0 + PCS block lock + far-end carrier.
# See notes/reference/scd-dumps/fm6000-et1-bringup-window-rw.txt and the shut==cold memory.
#
# SAFETY: arms the SCD watchdog FIRST and pets it in the background for the whole run, so any hard host
# hang self-recovers in ~30s instead of needing a physical power-cycle. Never remove that.
# SPDX-License-Identifier: GPL-2.0-or-later

B=0000:02:00.0
HOST=http://<mgmt-net-host>:8001/fm6000link
LOG=/mnt/flash/coldlink.log

: > $LOG
say() { echo "[coldlink] $*"; echo "[coldlink] $*" >> $LOG; sync; }
R()   { fm6000reg $B "$1" 2>/dev/null | sed 's/.*= 0x//'; }
S()   { scdreg "$1" 2>/dev/null | sed 's/.*= 0x//'; }

# ---- 1. WATCHDOG FIRST, then a background petter (MANDATORY - see fm6000-watchdog-must-pet memory) ----
scdreg 0x0120 0xC0000BB8 >/dev/null 2>&1
( while : ; do scdreg 0x0120 0xC0000BB8 >/dev/null 2>&1; sleep 3; done ) &
PET=$!
trap 'kill $PET 2>/dev/null; scdreg 0x0120 0x0 >/dev/null 2>&1' EXIT INT TERM
say "watchdog ARMED (0x0120=0xC0000BB8), petter pid=$PET"

PIN=$(R 0x1c021)
say "START PIN_STRAP=$PIN SOFT_RESET=$(R 0x00009)  (want PIN=00000208)"
if [ "$PIN" != "00000208" ]; then say "chip NOT alive -- ABORT"; exit 1; fi

# ---- 2. fetch the toolchain ----
for f in fm6000_coldreplay fm6000_initsbus fm6000_spico fm6000_serdes_cfg fm6000_linkup fm6000_spico_code.bin; do
	wget -q -O /tmp/$f $HOST/$f && chmod +x /tmp/$f 2>/dev/null
	say "fetched $f = $(stat -c %s /tmp/$f 2>/dev/null) bytes"
done
if [ ! -s /tmp/fm6000_linkup ]; then say "fetch FAILED -- ABORT"; exit 1; fi

# ---- 3. cold memory init + scheduler (phase107 recipe) ----
say "STEP1 coldreplay: masks + BIST + clocks + BOOT_CTRL 0x313 + scheduler tokens"
/tmp/fm6000_coldreplay $B >> $LOG 2>&1
say "  rc=$? PIN=$(R 0x1c021) 0x8062=$(R 0x8062) 0x8022=$(R 0x8022)  (running: 00200200 / c0300200)"
[ "$(R 0x1c021)" = "00000208" ] || { say "off-bus after coldreplay -- ABORT"; exit 2; }

# ---- 4. JSS SBus master ----
say "STEP2 initsbus"
/tmp/fm6000_initsbus $B >> $LOG 2>&1
say "  rc=$? PIN=$(R 0x1c021) F000=$(R 0x0f000) F004=$(R 0x0f004)"

# ---- 5. SPICO microcontroller (6000-word IMEM upload, ~30s; the petter covers it) ----
say "STEP3 spico load (~30s)"
/tmp/fm6000_spico $B /tmp/fm6000_spico_code.bin >> $LOG 2>&1
say "  rc=$? PIN=$(R 0x1c021)"

# ---- 6. SERDES_CFG: RefSel=0x1b - the phase97 TxRdy clock-domain lever ----
say "STEP4 SERDES_CFG RefSel (cold default 0x0aaaa005 has RefSel=0, golden 0x0aaa86c0 has RefSel=0x1b)"
fm6000reg $B 0xe3834 0x0aaa86c0 >/dev/null 2>&1
fm6000reg $B 0xe3835 0x00000001 >/dev/null 2>&1
say "  0xe3834=$(R 0xe3834) 0xe3835=$(R 0xe3835) 0xe383f=$(R 0xe383f)  (want TxRdy/RxRdy -> 0x60)"

# ---- 7. per-serdes 24-register coefficient preset ----
say "STEP5 serdes_cfg 68 (24 coeffs via SPICO DMEM)"
/tmp/fm6000_serdes_cfg $B 68 >> $LOG 2>&1
say "  rc=$? PIN=$(R 0x1c021)"

# ---- 8. THE VALIDATED WINDOW REPLAY ----
say "STEP6 linkup: 459-op EOS bring-up window (EPL14 + pipeline + SBus + SPICO DFE)"
/tmp/fm6000_linkup $B 20 >> $LOG 2>&1
LRC=$?
say "  rc=$LRC PORT_STATUS=$(R 0xe3800) 0xe383f=$(R 0xe383f) pcsRx=$(R 0xe3826)"

# ---- 8b. EPL_CFG_B: Port0PcsSel=3 = 10GBASE-R.  *** THE COLD-ONLY MISSING WRITE ***
# The shut/no-shut window never rewrites this - EOS sets the PCS type at BOOT (port speed config),
# not during a port bounce. Cold it reads 0x00080000 (PcsSel=0) so the PCS never block-locks and our
# TX emits nothing the far end can lock to. Writing 0x00090003 alone takes PORT_STATUS 0x15 -> 0x8c0
# and pcsRx 0 -> 1, live-verified cold with far-end carrier. Do NOT also write EPL_CFG_A (0xe3b01):
# changing RefClockASource on a running lane hard-hangs the single-CPU M1 host (watchdog recovers).
say "STEP6b EPL_CFG_B Port0PcsSel=3 (10GBASE-R) - the cold-only missing write"
say "  before CFG_B=$(R 0xe3b02)  (cold default 00080000, want 00090003)"
fm6000reg $B 0xe3b02 0x00090003 >/dev/null 2>&1
say "  after  CFG_B=$(R 0xe3b02) PIN=$(R 0x1c021)"
[ "$(R 0x1c021)" = "00000208" ] || { say "off-bus on CFG_B -- ABORT"; exit 3; }

# ---- 9. SFP laser ON (EOS leaves tx-disable asserted; bit6 active-high) ----
V=$(S 0x5010)
say "STEP7 SFP: SCD 0x5010=$V (bit6=1 means laser OFF)"
NEW=$(printf '0x%x' $(( 0x$V & ~0x40 )))
scdreg 0x5010 $NEW >/dev/null 2>&1
say "  wrote $NEW -> now $(S 0x5010)  (bit2=present active-low, bit0=rxlos)"

# ---- 10. settle and poll for the link ----
say "STEP8 polling for link (golden: PORT_STATUS=0x8c0, 0xe383f=0x60, pcsRx=1)"
i=1
while [ $i -le 10 ]; do
	sleep 3
	say "  t=$((i*3))s PORT_STATUS=$(R 0xe3800) 0xe383f=$(R 0xe383f) pcsRx=$(R 0xe3826) sd=$(R 0xe383e) PIN=$(R 0x1c021)"
	i=$((i+1))
done

PS=$(R 0xe3800)
say "RESULT PORT_STATUS=$PS  (bit6 RxLinkUp + bit7 HeartbeatOk set = LINK)"
say "DONE - log at $LOG"
