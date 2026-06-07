# 40G QSFP (swp49–52) Bring-up — Status & Reproducible Investigation

Living record of the effort to bring up the four 40G QSFP+ ports on the
AS5610-52X (BCM56840 Trident+, internal Warpcore SerDes). Read alongside
`BUILD.md` (build process) and `DATAPATH_BRINGUP.md`.

## Hardware signal chain (per QSFP port)

```
Trident Warpcore (internal SerDes, 4 lanes)
   ↕ MDIO (internal, phy_id 0x00b5 = WC5 for swp49-52)
DS100DF410 retimer  (i2c 0x27; one TX retimer + one RX retimer per port, 4 ch each)
   ↕
QSFP optic: Avago AFBR-79EBPZ  ── 40GBASE-SR-BiDi
```

**The optic is BiDi, not SR4.** AFBR-79EBPZ is 40GBASE-SR-**BiDi**: 4 electrical
lanes (4×10.3125G) multiplexed by WDM onto a **duplex LC (2 fibers)** — two
wavelengths (~850/900 nm), **2 lanes per wavelength/fiber**. (Earlier notes that
called it "SR4/MPO" were wrong.) Consequence for loopback testing: a *single*
LC fiber carries only ONE wavelength = 2 of the 4 lanes; you need the full duplex
pair for all 4.

Port↔serdes map (from Cumulus accton.py): swp49=serdes(36-39), swp50=(28-31),
swp51=(56-59), swp52=(32-35). QSFP EEPROM/retimer i2c buses: swp49=66/60,
swp50=67/61, swp51=68/62, swp52=69/63 (rx/tx).

## What works

- **Detection** (DTB `i2c-mux-idle-disconnect` + `atmel,24c02`; platform-init
  MODSEL/RESET/LPMODE + at24 rebind): all 4 QSFP EEPROMs read id `0x0D` on a clean
  boot. Persists across reflash.
- **Optics + retimers**: modules lase; DS100DF410 init via `retimer-init.sh`
  (`/sys/class/retimer_dev/`, `set_eq2`).
- **40G port config**: bmd `bmdPortMode40000fd`, warpcore SR4 fw_mode `0x1111`,
  4-lane CL82 link detect (`cl82_link_get` in `portmap.c`).
- **On a swp49↔swp50 duplex-LC loopback: 2 of 4 lanes AM-lock** (`am_lock=0x3`,
  lanes 0,1), all 4 lanes receive + adapt. Cumulus brought this same loopback up
  at full 40G (4/4), so 4/4 IS achievable — see the open blocker.

## THE OPEN BLOCKER: lanes 2,3 never reach CL82 AM-lock

Steady state (both ends of the loopback): `am_lock=0x3 deskew=0` — lanes 0,1 lock,
lanes 2,3 do not, so CL82 deskew never completes and the 40G link stays down.
All four lanes show real RX adaptation (VGA/DFE moving), so signal is present on
all four; only the digital alignment-marker lock fails on 2,3. Deterministic and
reproducible (10/10 re-adapt rounds identical).

### Ruled out (with evidence)
| Hypothesis | Verdict |
|---|---|
| Single-fiber topology limit | NO — duplex BiDi pair; Cumulus did 4/4 |
| Retimer EQ | NO — full CDR-reset + re-adapt on all 4 channels of all 4 QSFP retimers: zero change |
| Per-lane RX polarity flip | NO — RX-flip lanes 2,3 = no change |
| External BCM84740 PHY | NO — doesn't exist on QSFP (Cumulus MIIM trace touches only internal WC5; `mdk-init.c` confirms) |
| PMD speed encoding (X4 vs KR4) | NO — forcing `FV_fdr_40G_X4` is WORSE (RX DFE stops adapting, 0/4). KR4 + SR4 fw_mode is correct. |
| Warpcore RX lane remap (`XauiRxLaneRemap`) | NO — SVK reference value `0x1032` is for Broadcom's SVK board, not our Accton; applied pre- and post-enable, no lock gain (disturbs RX). Cumulus's Accton platform uses default lane map and got 4/4. |

### LEADING HYPOTHESIS: missing Warpcore RX lane remap (found 2026-06-03 in the SDK)
The Broadcom reference board for our exact chip, `asic/openmdk/board/config/board_bcm56846_svk.c`,
has a `_phy_reset_cb` that, for **warpcore** PHYs (PHY_INST==0), applies
`PhyConfig_XauiRxLaneRemap`:
- **port 45** (= swp50): `rx_map = 0x3210` (identity)
- **all other warpcore ports** (49/57/61 = swp49/52/51): `rx_map = 0x1032`
  (**swaps RX lane pairs 0↔1 and 2↔3** to match board wiring)

**EdgeNOS applies this NOWHERE** (`grep XauiRxLaneRemap asic/edged` = 0 hits), so the
QSFP ports run with the Warpcore's power-on default lane order, not the board's
required mapping. `PhyConfig_XauiRxLaneRemap` IS in our linked PHY API (`phy.h:271`).
This is also consistent with the earlier TX-flip-2,3-breaks-lane-0 result (lane order
is wrong).

**Experiment 1 (2026-06-03) — remap applied POST-config: NEGATIVE/inconclusive.**
`portmap.c` QSFP branch now calls `PHY_CONFIG_SET(pc, PhyConfig_XauiRxLaneRemap, …)`,
file-driven via `/tmp/qsfp_rxremap` ("ref" = reference per-port 0x3210/0x1032, or a
hex applied to all). Applied the reference values (logged `XauiRxLaneRemap=0x1032 rv=0`
on swp49/51/52, `0x3210` on swp50) — but `am_lock` stayed `0x3` AND RX adaptation
STOPPED (`vga=20 dfe=0` all lanes, same signature as the X4 experiment); a retimer
re-kick didn't recover it. **Root cause of the negative: timing.** The reference
applies the remap inside `_phy_reset_cb` — *during* PHY reset, before the SerDes
adapts — but EdgeNOS applies it *after* `bmd_port_mode_set` has already reset+adapted
the PHY, which disturbs RX without re-initializing it. Any post-init RX-lane-config
change (X4 speed, remap) leaves RX in the non-adapting `vga=20 dfe=0` state.

**Experiment 2 (2026-06-03) — remap applied PRE-enable: also NEGATIVE → remap RULED OUT.**
Moved the `XauiRxLaneRemap` call to *between* the Disable and Enable `bmd_port_mode_set`
calls in `portmap.c`, so the reset+adapt runs with the swap in place (the proper
timing). Reference values still applied cleanly (`rv=0`), but `am_lock` stayed `0x3`
and RX still didn't adapt (`vga=20 dfe=0`). **Conclusion + key realization:**
`board_bcm56846_svk.c` is Broadcom's **SVK reference board, NOT our Accton AS5610** —
different PCB lane wiring, so its `0x1032` remap is wrong for our board (and just
disturbs the RX). Confirmed by Cumulus: `accton.py` (the Accton platform) leaves
`rx_lane_map`/`tx_lane_map` **default** and Cumulus got 4/4. **So our board needs NO
lane remap; the difference vs Cumulus is NOT high-level config (lane map / polarity /
PMD are all default-equivalent on both) — it's the detailed Warpcore bring-up
register sequence.** The `/tmp/qsfp_rxremap` hook stays in the code (harmless, default
off) for future per-value sweeps if ever needed.

**Remaining path (hard):** diff EdgeNOS's Warpcore 40G bring-up against Cumulus's
full sequence. The captured `gdb_miim_capture_20260327.log` is a *flap* (TX-driver
writes only, which we already match), not a cold init — so the deciding writes
aren't in it. Getting them needs a **cold-init Cumulus capture** of swp49/50
(reflash Cumulus, GDB-trace `init` + first 40G port-up), or deeper static RE of the
Cumulus switchd warpcore path. This is the next real lead but it is not a quick edged tweak.

### SDK inventory (which SDK is right for this chip)
- **`OpenBCM/sdk-6.5.27`** does **NOT** support our chip — it's Trident2/2+/3 era
  (`src/soc/esw/{trident2,trident2p,trident3}`), no BCM56840, no Warpcore (uses
  tscf/tsce SerDes). Not useful for the 40G work. Don't chase it.
- **`asic/openmdk`** (what we build) is the correct/only SDK: full BCM56840/56846
  support (`bmd/PKG/chip/bcm56840_{a0,b0}`, `cdk/PKG/chip/bcm56840`,
  `phy/PKG/chip/bcmi_warpcore_xgxs` = "WarpCore 10/40GbE SerDes"), plus the
  reference board configs `board_bcm56846_svk.c` / `board_bcm56840_*.c` — mine
  these for the board-specific PHY setup we may be missing (lane remap, polarity).

### Reference: Cumulus captured the working bring-up
`edgecore-5610-reverse-engineering/traces/gdb_miim_capture_20260327.log` — MIIM
read/writes during a swp50 40G loopback flap (decoded in
`.../WARPCORE_TX_CONFIG_CAPTURED.md`). It's a *flap*, not a cold init, so it shows
TX-driver writes (which we already replicate) but not the full lane setup.

## Static RE & dynamic captures (the material to crack the bring-up diff)

We do NOT need to reflash Cumulus first — there is substantial RE material:

**Static RE — decompiled Cumulus Warpcore functions** (`newnos/docs/`):
- `ind_lane_init_decomp.c` = `_phy_wc40_independent_lane_init` (the **full per-lane
  40G bring-up**, ~30 `FUN_0158fbdc(reg,val,mask)` register-modifies: blocks
  `0x8357 0x8345 0x8390 0x8300 0x8309 0x83c0 0x8301 0x8302 0x810e 0x8308 0x833c
  0x833d 0x833e 0x8150 0x832b …`).
- `wc40_speed_set_decomp.c` (25 KB), `fw_mode_set`/`tx_control_set`/`wc40_init` decomps.

**Our driver is a PARTIAL hand-reimplementation of these** — `bcmi_warpcore_xgxs_drv.c`
`_warpcore_init_stage_2` explicitly says "the full SDK does this FIRST in
independent_lane_init" and **removed the IEEE MII reset** ("COMBO_MIICNTLr_RESETf
incompatible"). It writes a broad register set (MISC1-6, RX66_*, CL72/73,
CONTROL1000X*, ANARXCONTROLPCI, RXLNSWAP1, …) but a careful line-by-line diff vs
`ind_lane_init_decomp.c` has NOT been done — that is the concrete next task. (Caveat:
our driver uses named register macros, the decomp uses raw CL45 hex addrs, so the
diff must map hex↔name, not grep.)

**Dynamic captures** (`edgecore-5610-reverse-engineering/traces/`):
- `qsfp_miim_capture.txt` (4506 lines, **bus=2 = the QSFP Warpcore**, phy 1/9/13/17) —
  the most relevant; appears to cover QSFP port bring-up, but the GDB-watchpoint
  format (`MIIM: bus=N phy=N reg=0xR WR=0xD`, interleaved 0x1f block / 0x1e AER /
  data) needs a careful parser to reconstruct the per-lane block/reg/data sequence.
- `cumulus_port_up_miim_capture.txt` (929), `allports_miim.txt` (5608),
  `gdb_miim_capture_20260327.log` (flap — TX-driver writes only, already matched).
- `SERDES_WC_INIT.md` — documents the CMIC_MIIM format + the WC init sequence for
  xe0/swp1 (SFP lane 0); the CMIC_MIIM_PARAM decode is the key to parsing the raw
  captures.

### DIFF RESULT (2026-06-03): our driver lacks the per-lane RX-calibration layer
Compared `ind_lane_init_decomp.c` against our `_warpcore_init_stage_2`. Both write a
broad common set (UNICOREMODE10G, CL73_BAMCTRL3, MISC1-6, RX66_SCW0-3 64/66 config,
CONTROL1000X1/3, RX66_CONTROL, TX driver). But the **structural** difference:
- Cumulus `_phy_wc40_independent_lane_init` is **per-lane** and reads a **per-lane RX
  calibration** value (`FUN_015936ac` → writes `0x8308` mask 0x1f + `0x833c` bit 0x80)
  plus RX-EQ table setup via `FUN_0158b954` / `FUN_01594d90` / `FUN_01597f00`.
- Our OpenMDK driver has **no equivalent functions** — its function list is
  `init_stage_0/1/2, rx_div_clk_set, pll_lock_wait, primary_lane, serdes_lane/stop,
  linkup_event`. **It relies entirely on the Warpcore firmware's RX auto-adaptation**
  (which is why DFE/VGA move) and never programs explicit per-lane RX cal.
- It also **removed the IEEE MII reset** that the full SDK does first in
  independent_lane_init.

**Hypothesis:** firmware auto-adapt is enough for the robust lanes (0,1) but the
marginal BiDi second-wavelength lanes (2,3) need the explicit per-lane RX-cal margin
the full SDK adds. **Closing this is a real undertaking, two options:**
1. **Reverse + port** `FUN_015936ac` + the RX-EQ table sub-functions into the OpenMDK
   warpcore driver (multi-step RE; those sub-functions are not yet decompiled).
2. **Capture + replicate**: get a clean cold-init QSFP MIIM capture (parse
   `qsfp_miim_capture.txt` correctly, or a fresh Cumulus cold-init trace), extract the
   exact per-lane register VALUES Cumulus writes (esp. `0x8308`/`0x833c` per lane),
   and hardcode them in our QSFP path — avoids reversing the cal math.

**The deciding question (still open):** the lanes-0,1-lock / 2,3-fail asymmetry is
NOT explained by a uniformly-incomplete per-lane init (that would hit all 4 lanes).
It best fits the **BiDi two-wavelength link budget** (lanes 2,3 = the marginal
wavelength) needing more RX margin — which the full `independent_lane_init` may
provide and our partial version may not. Next task: parse `qsfp_miim_capture.txt`
into a clean per-lane sequence and/or diff `ind_lane_init_decomp.c` against
`_warpcore_init_stage_2`, port the missing RX-robustness writes, rebuild SDK, test.

## Reproducible: build a diagnostic edged, deploy, read per-lane EQ

The per-lane EQ diagnostic (`qsfp_eq_dump` in `asic/edged/portmap.c`) logs, every
8th poll, one line per QSFP port:
`Port swpNN: 40G EQ am_lock=0x.. deskew=.. L0[vga= dfe=] L1[..] L2[..] L3[..]`
`vga`/`dfe` ≈ default (vga 16-20, dfe 0) = no RX adaptation; varied = adapting.
`am_lock` is a 4-bit per-PCS-lane bitmap; `0xf` + `deskew=1` = full 40G link.

```bash
# 1. (edged-only change) rebuild edged against existing SDK libs:
docker run --rm -v "$PWD:/build/src" --entrypoint /bin/bash edgenos-builder \
  -c 'cd /build/src/asic/edged && make CROSS_COMPILE=powerpc-linux-gnu- SDK_BLDDIR=/build/src/output/sdk'
#    (PHY/SDK change instead? use scripts/rebuild-edged-with-sdk.sh — see BUILD.md)

# 2. switch DHCPs a new IP each boot — find it by MAC 80:a2:35:81:ca:ae:
for i in $(seq 1 254); do ping -c1 -W1 10.1.1.$i >/dev/null 2>&1 & done; wait
IP=$(ip neigh | awk '/80:a2:35:81:ca:ae/{print $1; exit}')

# 3. deploy + restart (EdgeNOS SSH: legacy kex, password as5610):
EOPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o KexAlgorithms=+diffie-hellman-group14-sha1"
sshpass -p as5610 scp $EOPTS asic/edged/edged root@$IP:/tmp/edged
sshpass -p as5610 ssh $EOPTS root@$IP \
  'systemctl stop edged; cp /tmp/edged /usr/sbin/edged; systemctl start edged'

# 4. read steady-state per-lane EQ (edged logs to the journal, NOT daemon.log):
sshpass -p as5610 ssh $EOPTS root@$IP \
  'journalctl -u edged -b | grep -E "swp(49|50): 40G EQ" | tail -4'
```

Live retimer re-tune (no rebuild) — per-port DS100DF410 via sysfs:
`for d in qsfp_rx_eq_0 qsfp_tx_eq_0 qsfp_rx_eq_1 qsfp_tx_eq_1; do
  x=$(grep -rl "^$d\$" /sys/class/retimer_dev/*/label | xargs dirname);
  echo 12>$x/device/channels; echo 0>$x/device/pfd_prbs_dfe; echo 64>$x/device/adapt_eq_sm;
  echo 28>$x/device/cdr_rst; usleep 20000; echo 16>$x/device/cdr_rst; done`

## Safety / recovery
- Switch DHCPs a new IP per boot → track by MAC `80:a2:35:81:ca:ae`.
- Known-good KR4 binaries: `output/edged-v10`, `output/edged-v11` (18.9 MB).
  A rebuilt edged < ~15 MB is BROKEN (inconsistent SDK libs) — do not deploy.
- To revert a running edged you must stop it first (`Text file busy` otherwise):
  `systemctl stop edged; cp <good> /usr/sbin/edged; systemctl start edged`.
