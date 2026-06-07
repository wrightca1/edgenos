# Warpcore firmware-adapt disable + host RX cal — protocol decode

**Goal:** get all 4 PCS lanes of a 40G QSFP port to AM-lock (currently 2/4 on
swp49↔swp50). Root cause (confirmed 3 ways, see
`project_qsfp_rxkick_attempt_2026_06_04`): OpenMDK leaves the Warpcore
**firmware RX auto-adapt running**, and it owns the lane state. A host-side
RX-cal "kick" cannot coexist with it — the firmware overwrites any host seed.
The fix is to do what the full Broadcom SDK does: **pause/stop the firmware uC,
program the per-lane cal, then restart**.

All decompiles below are from Cumulus `libopennsl.so.1`
(ghidra project `analysis/build-server/opennsl/ghidra-projects/libopennsl`).

## Why all three earlier kick attempts failed

`asic/openmdk/.../bcmi_warpcore_xgxs_drv.c` `_warpcore_init_stage_2` wrote the
correct cal register *values* (`0x8300/0x8302/0x8308/0x833c/0x833d/0x833e/
0x8150/0x832b/0x8309`) but **never paused the firmware first**. Attempts #1/#2
also used broadcast AER (clobbered locked lanes); #3 fixed addressing
(single-lane AER, lanes 2,3 only) → no regression but no lock. The *missing
piece in every case* is the firmware-pause handshake.

## The two firmware control points

### A. uC mailbox / UC_CTRL register `0x820e` (per-lane, via each lane's phy_addr)
From `_phy_wc40_firmware_mode_set` @0x01597254 (`docs/fw_mode_set_decomp.c`).
Per lane (lane addressed by its **own MDIO phy_addr** from a table
`uVar2 = *(iVar5 + 0xc8 + lane*4)`):

```
RMW 0x820e = 0x301 (mask 0xff0f)   ; "ready" / request command slot
poll 0x820e bit 0x80 == 1 (250ms)  ; READY_FOR_CMD ack
RMW 0x820e = 0x001 (mask 0xff0f)   ; STOP the uC (pause firmware)
poll 0x820e bit 0x80 == 1
RMW 0x81f2 (devad 1) nibble[lane] = fw_mode   ; per-lane fw mode (0xf<<lane*4 mask)
RMW 0x820e = 0x201 (mask 0xff0f)   ; LOAD / resume w/ new mode
poll 0x820e bit 0x80 == 1
RMW 0x820e = 0x301 (mask 0xff0f)   ; RESTART the uC
poll 0x820e bit 0x80 == 1
```
- `0x81f2` holds a 4-bit **fw_mode per lane** (nibble per lane). Our SR path
  sets `0x1111` (mode 1 = SFP_OPT_SR4 per lane).
- Command codes in low byte of 0x820e: `0x01`=stop, `0x201`=load/resume,
  `0x301`=ready/restart; bit `0x80` = command-done handshake.
- `fw_mode` (param_3, must be <8) is the lane behavior selector. **TODO: confirm
  which fw_mode value disables RX auto-adapt** (candidate: an "OS/SW DFE"
  mode). 0x81f2 nibble is the lever.

### B. DSC pause via `0xffe0` (used inside independent_lane_init)
From `_phy_wc40_independent_lane_init` @0x01599d10 (`docs/ind_lane_init_decomp.c`
lines 36-54):

```
MMD write 0xffe0 = 0x8000                       ; request uC/DSC pause
poll 0xffe0 bit 0x8000 == 0 (polarity 0, 10ms)  ; ACK (bit self-clears)
... (delay 25ms) ...
if dsc_state in {4,5}:  RMW 0x820e = 0x301 (mask 0xff0f)
```
Then it runs the **full per-lane RX cal** (the body of independent_lane_init):
DFE/slicer `0x8300`, FSM `0x8302`, CTLE seed `0x8308` + cal-valid `0x833c`
(seed from `FUN_015936ac`), LMS `0x833d/0x833e`, deskew `0x8150/0x832b`,
restart/release `0x8309`, plus TX-FIR via `FUN_0158b954/01594d90/01597f00`.

## Primitives (arg signatures)
- `FUN_0158f010(unit, ctx, devad, reg, val)` — MMD **write**. Lane encoded into
  the address (`reg | lane<<16` when devad==0 & a ctx flag; `0xffde` special-cased
  bare; multicast/broadcast via ctx+0x1d4). (`docs/wc_mmd_decomp_ghidra.c`)
- `FUN_0158c5b0(unit, ctx, devad, reg, *out)` — MMD **read**.
- `FUN_0158ee1c(ctx, reg, mask, polarity, timeout_us, devad)` — **poll**: read
  `reg` until `(mask & val)` is 0 (polarity 0) or nonzero (polarity 1); -9 on
  timeout. (`docs/wc_mmd_decomp_ghidra.c`)
- `FUN_0158fbdc(unit, ctx, devad, reg, val, mask)` — **RMW**, CONFIRMED:
  `new = (old & ~mask) | (val & mask)` (read FUN_0158c5b0; write FUN_0158f010).
  Our equivalent:
  ```c
  static int wc_rmw(int unit, uint32_t phy, uint32_t reg, uint32_t val, uint32_t mask){
      uint32_t v; cdk_xgs_miim_read(unit, phy, reg, &v);
      v = (v & ~mask) | (val & mask);
      return cdk_xgs_miim_write(unit, phy, reg, v);
  }
  ```
- `FUN_0156c428(unit, ctx)` — dispatches to MMD (`FUN_0156c024`) vs SBus
  (`FUN_0156bc04`) low-level write by ctx flag `*(ctx+0x100)&2`. Not needed for
  our cdk path (we go straight through cdk_xgs_miim_*).

## Mapping to OUR cdk path
Our writes go through `cdk_xgs_miim_write(unit, phy_addr, reg, val)`
(`asic/openmdk/cdk/.../xgs_miim.c`): `(1<<16)|reg` = clause-45 DEVAD=1; lane
select = AER reg `0xFFDE` (`0x01FF`=broadcast, lane#=single). Our RMW = read +
mask + write. The SDK addresses 0x820e by **per-lane phy_addr**; we must confirm
whether the AS5610 exposes per-lane MDIO addresses or whether AER-select +
single ctx phy_addr reaches the same uC mailbox. (This is the open addressing
question for 0x820e specifically — the cal regs already reached the SerDes in
attempts #1-3 via AER, so those work.)

## Risk notes (per `feedback_miim_safety`)
- The uC is **per-Warpcore (one micro for all 4 lanes)** — stopping it pauses
  adapt on lanes 0,1 too. A faithful port re-cals all 4 lanes through the
  stop/restart dance (what the SDK does). Getting the handshake/addressing wrong
  risks the currently-working lanes 0,1 and L1 stability.
- Always recoverable by `cp /usr/sbin/edged.baseline /usr/sbin/edged; restart`.
  Box currently at baseline `a3537f24` (10.1.1.208).

## Implementation plan (staged)
1. Confirm `FUN_0158fbdc`/`FUN_0156c428` semantics (pending decompile).
2. Add a `wc_uc_pause(unit, phy_addr)` / `wc_uc_restart()` pair replicating the
   `0x820e` dance (or the `0xffe0` DSC pause) via cdk_xgs_miim_{read,write}.
3. Gate behind the existing `/tmp/wc_rxkick` toggle; first test pausing →
   existing per-lane seed → restart, lanes 2,3 only, verify no regression on 0,1.
4. If seed-with-pause locks 2,3 → done. If not, port the full cal body
   (FUN_015936ac seed + TX-FIR funcs) for all 4 lanes.
