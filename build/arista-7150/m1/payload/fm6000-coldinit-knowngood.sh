#!/bin/sh
# fm6000-coldinit-knowngood.sh — the KNOWN-GOOD cold clean-room bank-init sequence (phase90).
#
# Run on the M1 cold-probe shell (cold90 build, FM6000_NOFILL=1) at root@<switch>. Makes the FM6000
# MCAST bank read VALID ECC ZEROS from a cold boot (0x240000=0x00000000, chip alive) — the warm golden
# state — with NO vendor code. Watchdog-safe (arms + pets + disarms). Flash-logged for hang forensics.
#
# Prereqs on the box (push live: wget http://<mgmt-net-host>:8000/<f>): fm6000_bist, fm6000_mrl_fixed,
# fm6000_wr128, fm6000reg, scdreg. Build fm6000_bist/mrl from asic/fm6000/fm6000_bist.c + fm6000_mrl.c.
#
# STATUS: reproducible bank-VALID read (exp5/exp8). NOT yet a clean win — non-deterministic (needs the
# sleep settles) and incomplete per-word coverage (a 2nd bank word may still off-bus). Full coverage is the
# open work (InitSBus + ordered CRM fills, or BIST_FULLCFG geometry after InitSBus). See
# notes/COLD-INIT-KNOWN-GOOD-RECIPE.md and COLD-INIT-MASTER-STATUS.md §13.
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
B=0000:02:00.0
mkdir -p /mnt/flash 2>/dev/null; mount -t vfat /dev/sda1 /mnt/flash 2>/dev/null
LOG=/mnt/flash/coldinit.log; : > "$LOG" 2>/dev/null
RG(){ fm6000reg $B "$1" 2>/dev/null | grep -o '[0-9a-f]*$'; }
WG(){ fm6000reg $B "$1" "$2" >/dev/null 2>&1; }
WD(){ scdreg 0x0120 0xC0000BB8 >/dev/null 2>&1; }               # arm/pet SCD watchdog (~30s timeout)
LV(){ RG 0x1c021; }                                             # PIN_STRAP: 0x208 alive, ffffffff off-bus. NEVER use a bank reg (e.g. CAM0 0x0e000) for liveness.
cm(){ echo "$*"; { echo "$*" >> "$LOG"; sync; } 2>/dev/null; }

WD; WG 0x1c01e 0xfffc0000; WG 0x1c01f 0x0009502f                # 1. scan-cfg PLL
cm "A PIN=$(LV) SOFT_RESET=$(RG 0x00009); BM march (BIST trigger, PACED)"
BIST_PACE_US=50 fm6000_bist $B >> "$LOG" 2>&1; sync; sleep 1     # 2. full BM march WITH BIST trigger = the ECC-init (no CRM fill)
WD; cm "B BM march done PIN=$(LV) 0x1D08E=$(RG 0x1d08e); MRL scan"
fm6000_mrl_fixed $B >> "$LOG" 2>&1; sync; sleep 1               # 3. MRL scan-chain memory config (REQUIRED; BM-march alone still off-buses)
WD; cm "C MRL done PIN=$(LV); re-enable clocks + MSB-out + deltas"
WG 0x1c03a 0xffffffff; WG 0x1c03b 0xffffffff                    # 4. MRL commit left block clocks OFF -> re-enable before any bank access
WG 0x00009 0x0                                                  #    MSB out of reset
WG 0x1c022 0x00000313; WG 0x1c038 0x0101e848; WG 0x1c048 0x0008bb2c   # warm-golden control deltas
WD; cm "D PIN=$(LV) SOFT_RESET=$(RG 0x00009)"
[ "$(LV)" != "00000208" ] && { cm "ABORT: off-bus before litmus (MRL non-determinism; recover + retry)"; scdreg 0x0120 0x0 >/dev/null 2>&1; exit 1; }
cm "E MCAST bank read (want 0x00000000 valid ECC, chip alive):"
for a in 240000 240004 240008 24000c 240040; do cm "   0x$a=$(RG 0x$a) PIN=$(LV)"; done
WD; cm "F WRITE test 0x240004=0xABCD1234:"; fm6000_wr128 0x240004 0xABCD1234 0x0 0x0 0x0 >> "$LOG" 2>&1
cm "   readback 0x240004=$(RG 0x240004) PIN=$(LV)"
scdreg 0x0120 0x0 >/dev/null 2>&1; cm "G DONE (watchdog disarmed)"
