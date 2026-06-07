# SFP+ & QSFP optics + SerDes bring-up — technical reference

Everything we've learned bringing the front-panel optics and the Warpcore SerDes to
life on an **Edgecore AS5610-52X** (Broadcom **BCM56846 / Trident+**) under a
from-scratch NOS (EdgeNOS), with **Cumulus Linux** on the same hardware as the L1
oracle. This is the **physical-layer** companion to the datapath/forwarding write-ups.

Redact internal MACs/IPs before sharing publicly.

---

## 0. The signal chain (know your hops)

```
Trident+ Warpcore SerDes lane(s)  ──►  DS100DF410 retimer  ──►  SFP+/QSFP cage  ──►  optic ──► fiber
        (CL49/CL82 PCS, MAC)            (CDR + equalization)      (I²C EEPROM,        (BiDi/SR)
                                                                  GPIO reset/modsel)
```

Three independent things must all be right before a link passes traffic:
1. **The chip SerDes** must be configured for the right speed/mode and the **PCS must
   lock** (10G: CL49 `block_lock`; 40G: CL82 alignment-marker lock + 4-lane deskew).
2. **The retimer** (DS100DF410) must have its CDR locked and its equalization tuned, or
   it keeps the output **muted** — the chip will never see a clean signal.
3. **The optic** must be powered, taken out of reset, mod-selected on the I²C mux, and
   (for QSFP) have its EEPROM and control bytes set correctly.

Most "the port won't come up" time goes into figuring out *which* of these three is the
actual blocker — they all present as "no link."

---

## 1. SFP+ (10G) — the parts that mattered

### 1.1 SerDes / PCS
- Internal **Warpcore** SerDes (`bcmi_warpcore_xgxs`), 4 lanes per core; a 10G SFP+ port
  uses one lane. PCS is **CL49** (10GBASE-R), success indicator = **`block_lock=1`**.
- Link state machine settles to a recognizable value once locked (we used `LSM=0xc262`
  as the "good" fingerprint while debugging).

### 1.2 DS100DF410 retimers — the real 10G unlock
Between the chip SerDes and the SFP+ cages sit **DS100DF410** retimers. Two findings
were decisive:
- **CDR reset is mandatory for signal pass-through.** Without toggling the channel's CDR
  reset, the retimer never re-locks to the incoming signal and the downstream side stays
  dark. (`cdr_rst` via the retimer's sysfs/driver.)
- **The output stays muted until equalization is set.** The combination that *unmuted*
  the retimer and produced PCS lock: **`pfd_prbs_dfe = 0`** and **`adapt_eq_sm = 64`**.
  With those, the DS100DF410 output came up and the Warpcore PCS hit `block_lock=1`.
- We drive these through a sysfs model mirrored from Cumulus's retimer driver
  (`/sys/class/retimer_dev/...`: `cdr_rst`, `adapt_eq_sm`, `set_eq2`, etc.), applied at
  platform init (`retimer-init.sh`).

### 1.3 The MII_STATUS gate (a software trap, not a hardware one)
The switch daemon gated TX on `BMD_PST_LINK_UP`, which is derived from the Warpcore
**MII status register (page 0x1800)**. Our PHY init path **wasn't populating that
status register**, so `MII_STATUS` read "no link" (`0x0109`, bit-2 clear) even though
the link was physically up — and the chip silently dropped every CPU-originated frame.
The fix was to **bypass the MII-status link gate** in the TX path: the link genuinely
is up (Cumulus proved the chassis links end-to-end), our status-read path just doesn't
reflect it yet. Lesson: a stale/empty status register can masquerade as "no link."

### 1.4 EEPROM / I²C
SFP+ module presence/type come from the cage EEPROM over I²C (through the platform
muxes). Straightforward relative to QSFP, but the same mux/idle-disconnect discipline
applies (§2.2).

### 1.5 Outcome
10G SFP+ ports link reliably, and the box now forwards real L2/L3 traffic — bidirectional
ping to a Cisco Nexus at 0% loss, full standard MTU.

---

## 2. QSFP (40G) — the optics control plane + the SerDes bring-up (SOLVED)

### 2.1 The optic is a BiDi module, not SR4
The installed modules are **AFBR-79EBPZ — 40GBASE-SR-BiDi** (EEPROM id `0x0D`). This is
**not** the common SR4/MPO part:
- It's a **duplex LC** module (2 fibers, not 8), using **WDM**: each fiber carries **two
  of the four lanes** on different wavelengths.
- Practical consequence: lane-to-fiber mapping and any "is the fiber the problem?"
  reasoning is different from SR4 — a single fiber issue takes out two PCS lanes.

### 2.2 Bringing the QSFP optics up (the platform control plane)
Reading the four QSFP EEPROMs at all required getting the **platform control plane**
right — this is CPLD/GPIO + muxed I²C, and the mappings are not intuitive:
- **at24 in the device tree** + **`i2c-mux-idle-disconnect`** on the mux so a stuck
  channel doesn't wedge the bus.
- **Control byte `0x71 = 0x0F`**: this sets **RESET_L de-asserted (pins 0–3 high)** and
  **MODSEL_L asserted (pins 4–7 low)** — i.e., take the modules out of reset *and*
  select them.
- **The gpiochip→address map is REVERSE-ordered** relative to what the schematic
  implies. This burned time — the "obvious" pin order was backwards.
- Result: all four QSFP optics detected, EEPROMs read, and the 40G config **persists
  across reflashes**.

### 2.3 40G PCS = CL82, four lanes, alignment + deskew
A 40GBASE-R link is **four PCS lanes** that must each **alignment-marker-lock (AM-lock)**
and then **deskew** into one logical channel (CL82). Success requires **all 4**; partial
lock = no link.

### 2.4 ✅ SOLVED (2026-06-07): all 4 lanes lock — it was two stacked bugs
For weeks this looked like a hard "2 of 4 lanes" SerDes wall. It wasn't. Two bugs
masked each other:
1. **Frozen adaptation** — we set `fw_mode=0x1111` (SR4), which *freezes* the Warpcore
   RX auto-adaptation. Fix: **`fw_mode=0`** (let firmware adapt) → all 4 lanes converge.
2. **Link-decode bug** — our check required the alignment-lock field `am_lock==0xf`, but
   that field is a *state-machine value*, not a per-lane bitmap, and **`0x6` is the
   locked state** (matches Cumulus's working 4/4 byte-for-byte; `0xf` never occurs).
   The chip was reporting `0x6` while our code called it unlocked.

Verified by raw-frame inject on swp49 + tcpdump on swp50 (and reverse): all 4 lanes,
both directions, frames intact. Fix in `patches/openmdk/bcmi_warpcore_xgxs_drv.c` +
`asic/edged/portmap.c` (`cl82_link_get`). What we'd previously **ruled out** (lane
swap/remap, polarity, X4-vs-KR4 forced modes) were all correctly ruled out — none were
the cause.

### 2.5 The disproven theory (kept as a cautionary record)
We had *pinned* a root cause that turned out to be wrong: that OpenMDK's Warpcore driver
omits the full SDK's per-lane RX-calibration layer (`_phy_wc40_independent_lane_init`,
`FUN_015936ac`, regs `0x8308`/`0x833c`) and the two stubborn lanes therefore needed
explicit cal we never ran. **This was false** — OpenMDK's firmware auto-adapt handles all
four lanes fine once it isn't frozen by `fw_mode=0x1111`. No cal-table port or cold-init
replay was needed. Lesson: a suspiciously clean fraction (2/4) is a hint to doubt your
*measurement* before theorizing about the silicon.

Decompiled references kept (now historical): `ind_lane_init_decomp.c`,
`wc40_speed_set_decomp.c`, `SERDES_WC_INIT.md`, captured MIIM traces (`*_miim_capture*`).

### 2.6 40G status: link up + forwarding
4/4 lane lock and bidirectional forwarding achieved (§2.4). No per-lane RX cal was
needed. CL82 dual-block config (AM markers + deskew) is programmed in the warpcore
driver. Remaining polish (not blockers): tuning over a live BiDi span vs the bench
loopback, and any optic-firmware nuances specific to the BiDi part.

---

## 3. Cross-cutting: how we touch the PHY safely

- **Never blind-write MIIM/PHY registers on this board.** On the AS5610 the MIIM path is
  shared/muxed; a careless raw write can wedge things. We **capture what the working NOS
  does via GDB passive observation + register dumps**, then replicate — rather than poke
  live MIIM.
- **Minimize debugger sessions on the live daemon** — repeated GDB attach/detach can
  zombie the switch daemon on this PPC platform.
- **Repeated PHY re-init degrades lane state.** Dozens of daemon restarts re-arm the
  Warpcore lanes and accumulate bad state; a port that "won't link after many restarts"
  often just needs a **clean reboot** before you go chasing a SerDes bug.
- **Read-only on the reference.** All Cumulus captures are read-only dumps; we never
  mutate the working baseline.

---

## 4. Cheat-sheet

**SerDes/PHY:** Warpcore (`bcmi_warpcore_xgxs`), `_warpcore_init_stage_2` (our partial
`independent_lane_init`), MII status page `0x1800` (`MII_STATUS`), `block_lock` (CL49
10G), CL82 AM-lock + deskew (40G), `RXLNSWAP1` / `PhyConfig_XauiRxLaneRemap`,
`_warpcore_rx_div_clk_set`, per-lane RX cal regs `~0x8308/0x833c`, modes
`FV_fdr_40G_KR4`(0x31)/`FV_fdr_40G_X4`(0x26)/CR4.

**Retimer (DS100DF410):** `cdr_rst` (CDR reset — required), `pfd_prbs_dfe=0`,
`adapt_eq_sm=64` (unmute), `set_eq2`; sysfs model `/sys/class/retimer_dev/*`;
`retimer-init.sh`.

**QSFP control plane:** `at24` DTB + `i2c-mux-idle-disconnect`; control byte `0x71=0x0F`
(RESET_L hi pins0–3, MODSEL_L lo pins4–7); **gpiochip↔addr map is reverse-ordered**;
optic AFBR-79EBPZ (40GBASE-SR-BiDi, EEPROM id `0x0D`, duplex-LC WDM, 2 lanes/fiber).

**Software gates that masquerade as L1:** `BMD_PST_LINK_UP` from MII status (bypass if
status path is stale), TX drop on "no link," lane-state degradation from restart churn.

---

## 5. Status

- ✅ **10G SFP+:** retimer CDR + EQ unlock, PCS `block_lock`, MII-gate bypass → links up,
  forwards traffic.
- ✅ **40G QSFP:** optics detected + persisted (control-plane decoded), **all 4 PCS
  lanes lock and the port forwards** in both directions. Root cause of the long
  "2 of 4" was `fw_mode=0x1111` freezing RX adaptation + an `am_lock==0xf` decode bug
  (`0x6` is the locked state); no per-lane RX cal layer was needed.
