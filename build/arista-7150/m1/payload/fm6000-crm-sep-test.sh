#!/bin/sh
# Disambiguate the MOD hang: ADDRESS-specific (0x150030 bad) vs OPERATION-specific (2nd CRM run in one
# process hangs). Every prior WORKING count=16 was a separate fm6000_crm process; count=256 and chunked
# (multi-run-in-one-process) hung. Here: run SEPARATE fm6000_crm processes at increasing addresses; PIN
# after each. All survive -> op-specific (fix = per-chunk process). One hangs -> address-specific.
set -u
BDF=0000:02:00.0
WD=0x0120; ARM=0xC0000BB8
say(){ echo "[sep] $*"; }
pet(){ scdreg $WD $ARM >/dev/null 2>&1; }
PIN(){ set -- $(fm6000reg $BDF 0x1c021 2>/dev/null); echo ${4:-ERR}; }

pet; say "start PIN=$(PIN)"
fm6000_initsbus $BDF >/dev/null 2>&1; pet
fm6000reg $BDF 0x00009 0x0 >/dev/null; pet
for c in 1 2 3; do fm6000reg $BDF 0x1c022 $c >/dev/null; done; pet
set -- $(fm6000reg $BDF 0x1c022); say "precondition done BOOTCTRL=$4 PIN=$(PIN)"

# separate single-op processes at increasing MOD element addresses (width3 => stride 3 words = 0x30/16elem)
for a in 0x150000 0x150030 0x150060 0x150090 0x150300 0x151000 0x152000; do
  FM6000_CRM_NORB=1 fm6000_crm $a 16 0 2 >/dev/null 2>&1
  pet; p=$(PIN); say "SEP-OP base=$a count=16 -> PIN=$p"
  [ "$p" = "0xffffffff" ] && { say "*** OFF-BUS/HANG at $a (ADDRESS-specific) ***"; scdreg $WD 0x0 >/dev/null 2>&1; exit 1; }
done
say ">>> ALL separate-process ops survived => the hang is OPERATION-specific (2nd CRM run in one process), NOT address"
scdreg $WD 0x0 >/dev/null 2>&1
say "=== done PIN=$(PIN) ==="
