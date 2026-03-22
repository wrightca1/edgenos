# 10G SFP+ Link Bringup Checklist — AS5610-52X (BCM56846)

Per IEEE 802.3 Clause 45/49/52, SFF-8431 (SFP+), and OpenBCM wc40.c reference.
Ordered by link-up sequence. Status: our code vs Cumulus verified behavior.

## Layer 1: Physical Medium (SFP+ Module)

| # | Item | Spec | Our Status | Notes |
|---|------|------|-----------|-------|
| 1.1 | SFP+ present (MOD_ABS=LOW) | SFF-8431 | OK | PCA9506 bus 65 addr 0x20, confirmed ports 1-3 |
| 1.2 | TX_DISABLE deasserted (LOW) | SFF-8431 | OK | PCA9506 bus 65 addr 0x21 = 0x00 |
| 1.3 | TX_FAULT clear | SFF-8431 | ? | Not checked |
| 1.4 | RX_LOS status | SFF-8431 | OK | Ports 1-3 clear (receiving from far end) |
| 1.5 | SFP EEPROM readable (A0, 0x50) | SFF-8472 | OK | Verified via sysfs |

## Layer 2: SerDes / PMA (WARPcore) — **THIS IS WHERE WE'RE STUCK**

| # | Item | Spec | Our Status | Notes |
|---|------|------|-----------|-------|
| 2.1 | Correct port-to-lane mapping | Board-specific | OK | Cumulus config.bcm: ports 1-8→lanes 65-72 |
| 2.2 | LCPLL reference clock config | Chip-specific | OK | DCFG_LCPLL_156 (156.25 MHz) set |
| 2.3 | XLPORT block power-up (IDDQ, PWRDWN, PWRDWN_PLL cleared) | Chip-specific | OK | xport_reset() in bmd_reset |
| 2.4 | XLPORT RSTB_HW deasserted | Chip-specific | OK | xport_reset() |
| 2.5 | XLPORT RSTB_MDIOREGS deasserted | Chip-specific | OK | xport_reset() |
| 2.6 | XLPORT RSTB_PLL deasserted | Chip-specific | OK | xport_reset() |
| 2.7 | TX PLL lock verified | Chip-specific | OK | xport_reset() checks, all pass except port 53 |
| 2.8 | WC multi-MMD mode enabled | Chip-specific | OK | warpcore_phy_init() writes reg 0x1F=0x8000 |
| 2.9 | WC independent lane mode (mode_10g=4) | Chip-specific | OK | warpcore_phy_init() for speed<=20000 |
| 2.10 | WC PLL sequencer started | Chip-specific | OK | warpcore_phy_init() sets START_SEQUENCER=1 |
| 2.11 | WC firmware loaded | Chip-specific | OK | init_stage_0 calls bcmi_warpcore_xgxs_firmware_set |
| 2.12 | WC PLL lock after firmware | Chip-specific | OK | init_stage_2 calls _warpcore_pll_lock_wait |
| 2.13 | WC speed set to 10G XFI | 802.3 Cl.49 | OK | speed_set FV_fdr_10G_XFI, confirmed via PHY dump |
| 2.14 | **WC TX driver current (idriver)** | WC-specific | **FAIL** | OpenMDK default=0, Cumulus SDK sets 9. Register 0x8067 stuck at 0x2001 |
| 2.15 | **WC TX pre-driver current (ipredrive)** | WC-specific | **FAIL** | Same — default=0, needs 9 |
| 2.16 | **WC TX FIR taps (pre-emphasis)** | WC-specific | **FAIL** | Cumulus: MAIN=55, POSTC=8, PREC=0. Not set by OpenMDK |
| 2.17 | WC TX electrical idle cleared | WC-specific | ? | ELECIDLE bit reads 0, but idriver=0 means no output anyway |
| 2.18 | WC RX equalizer config | WC-specific | ? | Cumulus: regs 0x19-0x1d = 0x8320/0x8350 |
| 2.19 | WC lane disable cleared | WC-specific | OK | MISC3r LANEDISABLE=0 confirmed |
| 2.20 | WC autoneg disabled (forced 10G) | 802.3 Cl.73 | OK | Cumulus: port_init_autoneg_xe=0 |
| 2.21 | CL45 PMA/PMD Control 1 (1.0) — not in low power, not loopback | 802.3 Cl.45 | ? | Not explicitly checked |
| 2.22 | CL45 PMA/PMD Control 2 (1.7) — PMA type correct | 802.3 Cl.45 | ? | Not explicitly set |
| 2.23 | CL45 PMD TX Disable (1.9) — all lanes TX enabled | 802.3 Cl.45 | ? | Not checked |

## Layer 3: PCS (64B/66B encoding, Clause 49)

| # | Item | Spec | Our Status | Notes |
|---|------|------|-----------|-------|
| 3.1 | 64B/66B encoding enabled | 802.3 Cl.49 | OK | MISC3 shows CL49_ED66=1 |
| 3.2 | IND_40BITIF set for XFI | Chip-specific | OK | MISC3 bit 15 = 1 |
| 3.3 | Scrambler enabled | 802.3 Cl.49 | ? | Default should be on |
| 3.4 | Block sync acquisition | 802.3 Cl.49 | ? | Can't check without TX working |
| 3.5 | CL45 PCS Status (3.32) — block lock | 802.3 Cl.45 | ? | Not checked |

## Layer 4: MAC (XMAC)

| # | Item | Spec | Our Status | Notes |
|---|------|------|-----------|-------|
| 4.1 | XMAC out of soft reset (SOFT_RESET=0) | Chip-specific | OK | xport_init sets SOFT_RESET=0 |
| 4.2 | XMAC TX enabled (TX_EN=1) | Chip-specific | OK | xport_init sets TX_EN=1 |
| 4.3 | XMAC RX disabled initially (RX_EN=0) | Chip-specific | OK | port_mode_set disables RX |
| 4.4 | XLPORT mode = single lane (PHY_PORT_MODE=2) | Chip-specific | OK | port_mode_set configures |
| 4.5 | XLPORT MAC mode = XMAC (not GMAC) | Chip-specific | OK | For speed>=10G |
| 4.6 | XMAC TX_CTRL (IPG, CRC, pad) | Chip-specific | OK | xport_init configures |
| 4.7 | Cumulus: `setreg xmac_tx_ctrl 0xc802` | rc.soc | ? | Different from OpenMDK value |

## Layer 5: Link Detection & Forwarding

| # | Item | Spec | Our Status | Notes |
|---|------|------|-----------|-------|
| 5.1 | PHY link status polling | 802.3 | OK | bmd_port_mode_update calls bmd_link_update |
| 5.2 | MAC RX enable on link-up | Chip-specific | PARTIAL | port_enable_set enables when link detected |
| 5.3 | EPC_LINK_BMAP set on link-up | Chip-specific | PARTIAL | Same — set by port_enable_set |
| 5.4 | PHY link=1 from WARPcore | 802.3 | OK | Ports 65-66 report link=1 (RX works!) |

## Layer 6: Retimer (DS100DF410)

| # | Item | Spec | Our Status | Notes |
|---|------|------|-----------|-------|
| 6.1 | CDR enabled, not in reset | DS100DF410 | OK | sfp-enable.sh: CDR reset assert/release |
| 6.2 | DFE/CTLE equalization mode | DS100DF410 | OK | sfp-enable.sh: reg 0x31=64 (CTLE+DFE) |
| 6.3 | No 25MHz ref clock | DS100DF410 | OK | sfp-enable.sh: reg 0x36=1 |
| 6.4 | CDR lock on ASIC→SFP direction | DS100DF410 | **FAIL** | Never locks — WC not driving valid signal |
| 6.5 | CDR lock on SFP→ASIC direction | DS100DF410 | OK | WC sees link (far-end Nexus signal passes) |

## MDIO Access (Clause 22 / 45)

| # | Item | Spec | Our Status | Notes |
|---|------|------|-----------|-------|
| M.1 | CL22 block select (reg 0x1F) for WC access | Broadcom-specific | OK | Used in warpcore_phy_init |
| M.2 | CL45 DEVAD encoding for internal registers | 802.3 Cl.45 | OK | MIIM_CL45_WRITE macro works |
| M.3 | AER lane selection for per-lane access | WC-specific | PARTIAL | PHY driver handles, but our raw writes don't always work |
| M.4 | Multi-MMD mode enabled for CL45 access | WC-specific | OK | warpcore_phy_init enables |

---

## ROOT CAUSE SUMMARY

**Items 2.14-2.16 are the blockers.** The OpenMDK WARPcore driver (`bcmi_warpcore_xgxs_drv.c`)
does not configure:
- TX driver current (idriver) — defaults to 0 (no drive)
- TX pre-driver current (ipredrive) — defaults to 0
- TX FIR taps (pre-emphasis) — not set

The Memory SDK (`wc40.c` in OpenBCM) sets these via `_phy_wc40_tx_control_set()` and
`_phy_wc40_tx_fir_default_set()` during `_phy_wc40_independent_lane_init()`. Defaults:
idriver=9, ipredrive=9.

**Verified from Cumulus**: MAIN=55, POSTC=8, PREC=0, VGA=36, PF=5 (from bcmcmd phy diag).
No explicit overrides in config.bcm — all defaults from the SDK.

## WHAT CUMULUS DOES THAT WE DON'T

From GDB capture (SERDES_WC_INIT.md) during port link-up:
1. Page 0, reg 0x17 = 0x8010 (TX control)
2. Page 0, reg 0x18 = 0x8370 (TX drive amplitude) — **KEY REGISTER**
3. Page 0x0008, reg 0x1e = 0x8000 (IEEE block enable)
4. Page 0x1000, reg 0x18 = 0x8010 (clock recovery)
5. Page 0x0a00, reg 0x10 = 0xffe0 (SerDes digital: fiber mode)
6. Page 0x3800, reg 0x01 = 0x0010 (WC_CORE sequencer start)
7. Regs 0x19-0x1d = 0x8320/0x8350 (RX equalization)

**The reg 0x18 = 0x8370 write on page 0 is the TX amplitude setting.**
This is a Broadcom-extended CL22 register, not the same as WC internal TX_DRIVERr (0x8067).

## CL45 REGISTERS TO CHECK (NOT YET VERIFIED)

Read these via MIIM_CL45_READ to confirm WC state:

| Register | DEVAD | Addr | What to check |
|----------|-------|------|---------------|
| PMA Control 1 | 1 | 0x0000 | bit 11=low_power (must be 0), bit 0=loopback (must be 0) |
| PMA Status 2 | 1 | 0x0008 | bit 10=local_fault, bit 13=tx_fault (both must be 0) |
| PMD TX Disable | 1 | 0x0009 | bits 0-4=TX disable per lane (all must be 0) |
| PMD Signal Det | 1 | 0x000A | bits 1-4=signal detect per lane |
| PCS Status 1 | 3 | 0x0020 | bit 12=rx_link_status |
| 10GBASE-R Stat | 3 | 0x0020 | bit 0=block_lock (PCS locked to far end) |

## CUMULUS VERIFIED PARAMETERS

From live switch GDB capture and bcm.d config:

### SDK Config (config.bcm)
- `phy_ext_rom_boot=0` — no external ROM boot
- `xgxs_lcpll_xtal_refclk=1` — LCPLL reference
- `port_init_autoneg_xe=0` — forced mode, no autoneg
- NO emphasis/polarity/lane_map overrides — all defaults
- Port map: 1-8→65-72, 9-16→5-12, etc.

### SerDes TX Parameters (from bcmcmd phy diag)
- MAIN=55, POSTC=8, PREC=0 (TX FIR taps)
- VGA=36, PF=5 (RX equalizer)
- BIAS=100%

### GDB-Captured MDIO Init (port link-up)
```
Page 0, reg 0x17 = 0x8010    TX control
Page 0, reg 0x18 = 0x8370    TX amplitude/drive    ← KEY
Page 0x0008, reg 0x1e = 0x8000  IEEE block enable
Page 0x1000, reg 0x18 = 0x8010  Clock recovery
Page 0x0a00, reg 0x10 = 0xffe0  SerDes digital mode
Page 0, regs 0x19-0x1d = 0x8320/0x8350  RX EQ
Page 0x3800, reg 0x01 = 0x0010  WC_CORE sequencer
```

## NEXT ACTION

1. **Read CL45 PMD TX Disable (1.9)** to check if per-lane TX is disabled
2. **Replay CL22 reg 0x18 = 0x8370** (TX amplitude) for each WARPcore
3. **Read back reg 0x18** to verify write took effect
4. **Check retimer CDR lock** after TX amplitude set

The reg 0x18 write is Broadcom extended CL22, done with:
```c
cdk_xgs_miim_write(unit, phy_addr, 0x1f, 0x0000);  // page 0
cdk_xgs_miim_write(unit, phy_addr, 0x18, 0x8370);   // TX amplitude
```
This should be done for each WARPcore primary lane, using the correct PHY_ADDR.

See also: docs/10gbase-r-linkup-checklist.md (comprehensive standards-based checklist)
