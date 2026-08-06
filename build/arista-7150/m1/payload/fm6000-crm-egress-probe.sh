#!/bin/sh
# fm6000-crm-egress-probe.sh — DECISIVE test of the phase91 reframe:
# does a CRM Memory-Set fill of the egress banks (which off-bused at count>=16 in prior cold work)
# now SUCCEED once the precondition we just found (InitSBus + SOFT_RESET=0 all bits cleared) is met?
#
# Runs on the cold90 M1 probe shell. Watchdog-safe (arm+pet; disarm at end). PIN_STRAP liveness only
# (never a bank read). fm6000_crm CLI: fm6000_crm <base> <count> [value] [size]; size 2 = 96b (width3).
set -u
BDF=0000:02:00.0
WD=0x0120; ARM=0xC0000BB8
say(){ echo "[crm] $*"; }
pet(){ scdreg $WD $ARM >/dev/null 2>&1; }
PIN(){ set -- $(fm6000reg $BDF 0x1c021 2>/dev/null); echo ${4:-ERR}; }
CRMfill(){ # base count size  -> prints result + liveness; returns 1 on off-bus
  fm6000_crm $1 $2 0 $3 2>&1 | tail -1; pet
  p=$(PIN); say "  after CRM fill base=$1 count=$2 -> PIN=$p"
  [ "$p" = "0xffffffff" ] && return 1 || return 0
}

export FM6000_CRM_NORB=1   # skip fm6000_crm's off-busing bank readback; verify via PIN instead
pet; say "=== start PIN=$(PIN) ==="

# --- precondition (the phase91 discovery) ---
fm6000_initsbus $BDF >/dev/null 2>&1; pet; say "after InitSBus PIN=$(PIN)"
fm6000reg $BDF 0x00009 0x0 >/dev/null;   pet; say "after SOFT_RESET=0 PIN=$(PIN)"
for c in 1 2 3; do fm6000reg $BDF 0x1c022 $c >/dev/null; done; pet
set -- $(fm6000reg $BDF 0x1c022); say "BOOTCTRL(freelists)=$4"

# --- full ECC-init of all egress banks via CHUNKED (paced count<=16) CRM Memory-Set ---
# base count size(0=32b,2=96b,3=128b) — matches the 129-fill table widths for MOD/MCAST.
say "--- CRM-filling all egress banks (chunked, SR0) ---"
for spec in "0x150000 4096 2" "0x154000 4096 2" "0x158000 1024 3" "0x15a000 4096 0" "0x15c000 4096 0" "0x15e000 4096 0" "0x240000 4096 2" "0x260000 32768 0"; do
  set -- $spec
  CRMfill $1 $2 $3 || { say "*** OFF-BUS/HANG at $1 count=$2 ***"; scdreg $WD 0x0 >/dev/null 2>&1; exit 1; }
done
say ">>> ALL egress banks (MOD+MCAST) CRM-filled clean, chip alive PIN=$(PIN)"

# --- THE PAYOFF: now start the scheduler engine over the ECC-clean egress memory ---
# fm6000_sched_std (FM6000_SR0=1) does SOFT_RESET=0 + tick + ring init + INIT_COMPLETE + FoundTok probe.
# If the CRM egress fills worked, TX_INIT_COMPLETE should NO LONGER off-bus, and the ring should circulate.
say "--- starting scheduler engine (ring init + INIT_COMPLETE) over ECC-clean egress ---"
FM6000_SR0=1 PATH=/tmp:$PATH /tmp/fm6000_sched_std $BDF 2>&1 | grep -E "after (RX_INIT|TX_INIT)|CIRCULATION|FOUND=1|PIN_STRAP=0xffff|done"
pet
scdreg $WD 0x0 >/dev/null 2>&1
say "=== EXPERIMENT COMPLETE, final PIN=$(PIN) ==="
