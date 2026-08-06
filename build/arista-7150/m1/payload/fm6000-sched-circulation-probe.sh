#!/bin/sh
# fm6000-sched-circulation-probe.sh — DECISIVE cold experiment: does the SSCHED ring circulate cold?
#
# Runs on the cold90 M1 probe shell (ssh root@10.1.1.77). Watchdog-safe (arms + pets + disarms).
# SSCHED-only diagnostic — does NOT read ESCHED/MCAST/MOD, so no off-bus-risk from the probe itself.
#
# Sequence: datasheet Table 4-1 boot init (memories writable + clocks up) -> InitSBus -> byte-exact
# SSCHED ring init (fixed port-78 token 0x24e) -> REPLACE_TOKEN find-probe -> report CIRCULATION.
#
# RESULT INTERPRETATION (grep the log for "CIRCULATION:"):
#   FOUND     -> ring advances cold; the token fix / init worked. Next: pursue ESCHED bring-up.
#   NOT FOUND -> tick/clock does NOT advance the ring cold = the real wall (pivot to scheduler clock).
set -u
BDF=0000:02:00.0
LOG=/mnt/flash/sched-probe.log
WD=0x0120; WDARM=0xC0000BB8
say(){ echo "[$(date +%T)] $*" | tee -a "$LOG"; sync; }
pet(){ scdreg $WD $WDARM >/dev/null 2>&1; }

: > "$LOG"; say "=== sched circulation probe start ==="
pet   # arm watchdog (~30s; pet between every step)

WG(){ fm6000reg $BDF $1 $2 >/dev/null 2>&1; }
RG(){ fm6000reg $BDF $1; }
bootcmd(){ # $1=cmd ; write 0x1c022, poll CommandDone bit4
  WG 0x1c022 $1; i=0
  while [ $i -lt 20 ]; do
    v=$(RG 0x1c022 2>/dev/null); pet
    case "$v" in *1[0-9a-f]|*1[0-9a-f]" "*) ;; esac  # bit4 heuristic; log raw
    i=$((i+1)); done
  say "bootcmd $1 -> 0x1c022=$(RG 0x1c022)"
}

# liveness
say "PIN_STRAP=$(RG 0x1c021)"

# --- datasheet Table 4-1 boot init (known-good) ---
WG 0x1c01e 0xfffc0000; WG 0x1c01f 0x0009502f; pet
WG 0x1c03b 0xffffffff; WG 0x1c03a 0xffffffff; pet   # Step5 normal-mode + block clocks
WG 0x00009 0x0; pet                                  # Step7 enable modules (SOFT_RESET=0)
bootcmd 1; bootcmd 2; bootcmd 3                       # Steps8-10 FFU/bankrepair/freelists -> 0x313
say "BOOT_CTRL=$(RG 0x1c022)  PIN=$(RG 0x1c021)"
pet

# --- InitSBus (SBus/SPICO engine; part of real boot before scheduler) ---
if command -v fm6000_initsbus >/dev/null 2>&1; then
  fm6000_initsbus $BDF 2>&1 | tee -a "$LOG"; pet
  say "after InitSBus PIN=$(RG 0x1c021)"
fi

# --- byte-exact SSCHED ring init + SAFE circulation probe ---
say "--- running fm6000_sched_std (ring init + FoundTok probe) ---"
fm6000_sched_std $BDF 2>&1 | tee -a "$LOG"; pet

say "=== done. verdict: ==="; grep -E "CIRCULATION:|PROBE " "$LOG" | tee -a "$LOG"
scdreg $WD 0x0 >/dev/null 2>&1   # disarm watchdog
say "watchdog disarmed. PIN=$(RG 0x1c021)"
