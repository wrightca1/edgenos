# QSFP 40G cold-init capture plan (Cumulus → EdgeNOS replicate)

**Why:** Pure-software RX-cal in OpenMDK converges at 2/4 because the cal seed +
TX-FIR are computed from board calibration tables (SOC PVT / per-port) that
OpenMDK never builds (see `project_wc_fw_pause_protocol_2026_06_04`,
`docs/WC_FIRMWARE_PAUSE_PROTOCOL.md`). Cumulus computes them. So: capture the
**exact per-lane cal values Cumulus produces** for swp49–52 and replay them
verbatim in EdgeNOS's warpcore init — sidestepping the missing tables.

**Target:** swp49↔swp50 loopback reaches 4/4 AM-lock (`am_lock=0xf`).
**Box:** reflash to Cumulus 2.5.0 (see `reference_cumulus_license_install`,
`feedback_onie_install_no_workarounds`). PPC CPU → GDB can zombie switchd
(`feedback_gdb_switchd`): ONE clean GDB session, scripted, then detach.
**Live Cumulus is READ-ONLY** (`feedback_readonly_cumulus`): capture/read only,
no register writes.

## What to capture — two independent methods, do BOTH (cross-check)

### Method A (primary, safest): read converged cal state via bcmcmd, post-lock
Once Cumulus brings swp49–52 to 4/4, the per-lane cal registers HOLD the
converged values. Pure reads. Cumulus has the BCM diag shell:

```
# confirm 40G + 4/4 first
bcmcmd "ps"                     # port status: xe ports at 40G, link up
bcmcmd "phy diag xe dsc"        # per-lane DSC/RX-cal dump (CTLE, DFE taps, lock)
bcmcmd "phy diag xe"            # general phy state
```
Then read the exact cal registers per lane (the ones EdgeNOS writes). For each
QSFP logical port (find them via `ps`), per lane 0..3, read via the WC PHY raw
MMD read. Cumulus bcmcmd raw clause-45 PHY read:
```
# syntax: phy raw c45 <port> <devad> <reg>   (confirm exact syntax on-box: 'phy raw ?')
# AER select then read, OR use phy diag which already walks lanes
```
Registers to capture per lane (devad 1 / 0x8xxx block):
- 0x8308 (CTLE seed/idx), 0x833c (cal-valid / bit7)
- 0x8300 (DFE+slicer enable), 0x8302 (RX cal FSM state)
- 0x833d, 0x833e (LMS), 0x8150, 0x832b (deskew/align)
- 0x8301 (rate/ctrl), 0x8357, 0x8345, 0x83c0, 0x8104, 0x82e3/0x82e6/0x82e8
- TX-FIR: 0x8067 (lane0), 0x8077 (1), 0x8087 (2), 0x8097 (3), 0x80b4, 0x80ba
- fw mode: 0x81f2 (4 nibbles, one per lane), 0x820e (UC_CTRL state)
Also dump the DFE tap values from `phy diag xe dsc` (these are the adapted RX EQ
— useful even if we seed-only).

### Method B (definitive): GDB passive capture of the cold-init MIIM writes
Captures the actual WRITE sequence + seed *data* (steady-state traces miss the
0x8308/0x833c data writes — see project_qsfp_rxkick_attempt). Cold init happens
at switchd start, so capture a forced re-init:

1. Let switchd come up fully (4/4). Find switchd PID + libopennsl map base:
   ```
   pidof switchd ; cat /proc/$(pidof switchd)/maps | grep libopennsl | head -1
   ```
   Runtime addr of a func = map_base + (file_offset - 0x10000)  [image base 0x10000].
2. Functions to break (stripped → break at computed address, log args, continue):
   - MMD write  FUN_0158f010(unit, ctx, devad, reg, val)   @ file 0x0158f010
   - RMW        FUN_0158fbdc(unit, ctx, devad, reg, val, mask) @ 0x0158fbdc
   - poll       FUN_0158ee1c  @ 0x0158ee1c  (optional, for handshake timing)
   PPC arg regs: r3,r4,r5,r6,r7,r8 = param_1..6.
3. GDB script (`/tmp/wc_capture.gdb`) — compute BASE on-box first:
   ```
   set pagination off
   set logging file /tmp/wc_miim_capture.log
   set logging on
   # BASE = map_base - 0x10000  (fill in)
   break *(BASE + 0x0158f010)
   commands
     silent
     printf "W devad=%d reg=0x%x val=0x%x\n", $r5, $r6, $r7
     continue
   end
   break *(BASE + 0x0158fbdc)
   commands
     silent
     printf "RMW devad=%d reg=0x%x val=0x%x mask=0x%x\n", $r5, $r6, $r7, $r8
     continue
   end
   continue
   ```
4. Trigger cold init on the QSFP ports WITHOUT restarting switchd if possible
   (port bounce): `bcmcmd "port xe linkscan=off"` then a speed re-set, OR the
   Cumulus ifdown/ifup of swp49–52. If a bounce doesn't re-run independent_lane_init,
   fall back to: detach GDB, `service switchd restart` is too heavy — instead
   attach GDB to switchd BEFORE it inits (start switchd under gdb), let it init,
   capture, detach. (Heavier; only if bounce insufficient.)
5. Filter the log to QSFP ports' phy_addr (from `cat /proc/.../maps` + the port
   map) and the cal registers above. The 0x8308/0x833c values are the prize.

## Replay in EdgeNOS
Hardcode the captured per-lane values into the v5 cal scaffold
(`bcmi_warpcore_xgxs_drv.c`, toggle-gated kick): replace the guessed seed
(0x8308=0x05/0x833c=0x80) + add the captured TX-FIR (0x8067/77/87/97) + any
extra setup regs Cumulus writes that we omitted. Keep per-lane AER addressing
(confirmed working). If Cumulus uses a specific fw_mode in 0x81f2 that lets the
seed stick, set that too (the 0x820e dance from WC_FIRMWARE_PAUSE_PROTOCOL.md).

## Pre-reflash checklist (do on EdgeNOS first, it's about to be wiped)
- [ ] Confirm working EdgeNOS image is backed up / reflashable (onie-nos-install URL).
- [ ] Note swp49–52 → logical/physical port + warpcore phy_addr map (from our
      probe: port 49 phy=0x281, 57=0x289, 61=0x28d; QSFP ports were 49–52 block).
- [ ] Save current binaries: output/edged-rxkick5 etc. already saved.
- [ ] Cumulus license ready: /home/smiley/license.txt (clock → 2013-10-01).

## After capture → reflash back to EdgeNOS
The switch IP moves on reflash (DHCP) — sweep 10.1.1.0/24 for the box (Dropbear
= EdgeNOS, see project_switch_ip_reflash). Was .208 this session.
