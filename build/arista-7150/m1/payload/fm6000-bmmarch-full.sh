#!/bin/sh
# fm6000-bmmarch-full.sh — phase91 BM-march -> CRM -> engine chain.
# Hypothesis: MOD/MCAST beyond a small default-mapped region are inaccessible cold (0x150030+ HANGS the host)
# because their SRAM memory controllers aren't configured. The BM-march (full warm geometry) configures the
# controllers + ECC-inits the physical SRAM, making the full egress range accessible so CRM RMW no longer hangs.
# Chain: InitSBus -> SOFT_RESET=0 -> BM-march(paced) -> accessibility test -> full egress CRM fill -> engine start.
# Watchdog-safe (arm+pet; a host hang -> watchdog reboots, recoverable). PIN_STRAP liveness only.
set -u
BDF=0000:02:00.0
WD=0x0120; ARM=0xC0000BB8
say(){ echo "[bm] $*"; }
pet(){ scdreg $WD $ARM >/dev/null 2>&1; }
PIN(){ set -- $(fm6000reg $BDF 0x1c021 2>/dev/null); echo ${4:-ERR}; }
CRMfill(){ FM6000_CRM_NORB=1 fm6000_crm $1 $2 0 $3 >/dev/null 2>&1; pet; p=$(PIN); say "  CRM $1 n=$2 -> PIN=$p"; [ "$p" = "0xffffffff" ] && return 1 || return 0; }

pet; say "=== start PIN=$(PIN) ==="
fm6000_initsbus $BDF >/dev/null 2>&1; pet; say "after InitSBus PIN=$(PIN)"
fm6000reg $BDF 0x00009 0x0 >/dev/null;  pet; say "after SOFT_RESET=0 PIN=$(PIN)"

# --- BM-march: configure SRAM controllers (full warm geometry) + ECC-init physical SRAM ---
say "--- BM-march (BIST_FULLCFG, paced 50us) ---"
BIST_FULLCFG=1 BIST_PACE_US=50 fm6000_bist $BDF 2>&1 | tail -6; pet
p=$(PIN); say "after BM-march PIN=$p"
[ "$p" = "0xffffffff" ] && { say "*** BM-march OFF-BUSED ***"; scdreg $WD 0x0 >/dev/null 2>&1; exit 1; }

# --- MRL scan: wires the MMIO address decode to the SRAM (BM-march alone leaves it unmapped) ---
say "--- MRL scan (fm6000_mrl_fixed) ---"
fm6000_mrl_fixed $BDF 2>&1 | tail -3; pet
# MRL's final commit sets block clocks OFF (0x1C03A=0x80000040) — MUST re-enable before any bank access
fm6000reg $BDF 0x1c03a 0xffffffff >/dev/null; fm6000reg $BDF 0x1c03b 0xffffffff >/dev/null
fm6000reg $BDF 0x00009 0x0 >/dev/null
fm6000reg $BDF 0x1c038 0x0101e848 >/dev/null; fm6000reg $BDF 0x1c048 0x0008bb2c >/dev/null
pet; p=$(PIN); say "after MRL + clock-reenable PIN=$p"
[ "$p" = "0xffffffff" ] && { say "*** MRL/clock-reenable OFF-BUSED ***"; scdreg $WD 0x0 >/dev/null 2>&1; exit 1; }

# --- freelists (needed before the scheduler; safe here after clocks are back) ---
for c in 1 2 3; do fm6000reg $BDF 0x1c022 $c >/dev/null; done; pet
set -- $(fm6000reg $BDF 0x1c022); say "freelists BOOTCTRL=$4"

# --- THE KEY TEST: is the previously-hanging MOD range now accessible? ---
say "--- accessibility test: CRM fill 0x150000 count=512 (well past the element-16 hang point) ---"
CRMfill 0x150000 512 2 || { say "*** STILL HANGS past elem16 — BM-march did NOT fix MOD accessibility ***"; scdreg $WD 0x0 >/dev/null 2>&1; exit 1; }
say ">>> MOD 0x150000+ NOW ACCESSIBLE after BM-march+MRL! (the chain works)"

# --- full egress ECC-init via chunked CRM, then start the engine ---
say "--- full egress CRM fill (chunked) ---"
for spec in "0x150000 4096 2" "0x154000 4096 2" "0x158000 1024 3" "0x15a000 4096 0" "0x15c000 4096 0" "0x15e000 4096 0" "0x240000 4096 2" "0x260000 32768 0"; do
  set -- $spec; pet
  CRMfill $1 $2 $3 || { say "*** off-bus/hang at $1 ***"; scdreg $WD 0x0 >/dev/null 2>&1; exit 1; }
done
say ">>> ALL egress banks ECC-inited, chip alive PIN=$(PIN)"

# --- THE PAYOFF: start the scheduler engine over clean egress ECC ---
say "--- engine start (SR0 ring init + INIT_COMPLETE + FoundTok probe) ---"
FM6000_SR0=1 fm6000_sched_std $BDF 2>&1 | grep -E "after (RX_INIT|TX_INIT)|CIRCULATION|FOUND=1|PIN_STRAP=0xffff"
pet; scdreg $WD 0x0 >/dev/null 2>&1
say "=== EXPERIMENT COMPLETE, final PIN=$(PIN) ==="
