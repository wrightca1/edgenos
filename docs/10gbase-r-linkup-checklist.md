# 10GBASE-R (10G SFP+) Link-Up Checklist

**Target**: AS5610-52X with BCM56846 (WARPcore SerDes), DS100DF410 retimers, SFP+ cages
**Standards**: IEEE 802.3 Clause 45/46/49, SFF-8431, SFF-8472

Legend:
- **[MUST]** = Required for basic link-up
- **[OPT]** = Optional / optimization / diagnostics only

---

## 1. Physical Medium Dependent (PMD) Layer -- SFP+ Module

Reference: SFF-8431 (SFP+ electrical spec), SFF-8472 (management/DDM)

### 1.1 Module Detection and Power-Up

| # | Step | Priority | Details |
|---|------|----------|---------|
| 1.1.1 | Read MOD_ABS (module present) | **[MUST]** | PCA9506 @ bus 65 addr 0x20. Active LOW = present. Must be 0 before proceeding. |
| 1.1.2 | Wait t_init after module insertion | **[MUST]** | SFF-8431 specifies max 300 ms from power-on to module ready (data ready on I2C). |
| 1.1.3 | Verify module identity via I2C A0h | **[MUST]** | Read A0h byte 0 (identifier): 0x03 = SFP/SFP+. Byte 6 bit 3: 10GBASE-SR, bit 5: 10GBASE-LR, etc. Confirms module is 10G-capable. |
| 1.1.4 | Check A0h byte 12 (bit rate nominal) | **[OPT]** | Should read 103 (= 10300 Mbps in units of 100 Mbps). Sanity check. |

### 1.2 TX Enable

| # | Step | Priority | Details |
|---|------|----------|---------|
| 1.2.1 | De-assert TX_DISABLE (drive LOW) | **[MUST]** | PCA9506 @ bus 65 addr 0x21. Write 0 to the port's bit. Active HIGH = disabled, so clear the bit. The module laser will NOT transmit until this is done. |
| 1.2.2 | Wait t_on (TX enable to valid output) | **[MUST]** | SFF-8431: max 2 ms from TX_DISABLE de-assertion to stable optical output. |
| 1.2.3 | Verify TX_FAULT is de-asserted | **[MUST]** | PCA9506 @ bus 65 addr 0x23 (ports 41-48) or readback via A2h byte 110 bit 2. TX_FAULT = 1 means laser failure. If asserted after t_init, module is faulty. |
| 1.2.4 | Verify RX_LOS is de-asserted | **[MUST]** | PCA9506 @ bus 65 addr 0x22. Active HIGH = loss of signal. Must be 0 to confirm far-end light is being received. If 1, no optical signal from remote -- check fiber/remote TX. |

### 1.3 SFP+ I2C Diagnostics (A0h / A2h)

| # | Step | Priority | Details |
|---|------|----------|---------|
| 1.3.1 | Read A0h bytes 0-95 (base ID fields) | **[OPT]** | Vendor name (bytes 20-35), part number (40-55), serial (68-83), wavelength (60-61), compliance codes (bytes 3-10). |
| 1.3.2 | Check A0h byte 92 (DDM type) | **[OPT]** | Bit 6: DDM implemented. Bit 5: internally calibrated. Bit 4: externally calibrated. Needed to interpret A2h data. |
| 1.3.3 | Read A2h bytes 96-105 (real-time DDM) | **[OPT]** | Bytes 96-97: Temperature (signed, 1/256 C LSB). Bytes 98-99: Vcc (unsigned, 100 uV LSB). Bytes 100-101: TX bias (unsigned, 2 uA LSB). Bytes 102-103: TX power (unsigned, 0.1 uW LSB). Bytes 104-105: RX power (unsigned, 0.1 uW LSB). |
| 1.3.4 | Verify TX power is non-zero | **[OPT]** | Confirms the module laser is actually emitting. TX power should match module spec (e.g. -6 to +0.5 dBm for LR). |
| 1.3.5 | Verify RX power is non-zero | **[OPT]** | Confirms far-end signal is arriving. If RX power is non-zero but RX_LOS is still asserted, the signal may be below the module's threshold. |
| 1.3.6 | Read A2h byte 110 (status/control) | **[OPT]** | Bit 7: TX disable state. Bit 2: TX fault. Bit 1: RX LOS. Bit 0: data ready. Alternate way to read status without GPIO. |

### 1.4 Host-Side Electrical (SFF-8431)

| # | Step | Priority | Details |
|---|------|----------|---------|
| 1.4.1 | Verify host TX signal at SFP+ pins | **[MUST]** | SFF-8431 requires 100-ohm differential, AC-coupled. The BCM56846 SerDes through DS100DF410 retimer must provide a valid 10.3125 GBd NRZ signal. |
| 1.4.2 | Host TX swing (Vdiff,pp) | **[MUST]** | SFF-8431 Table 2: 270-800 mVpp differential. Controlled by retimer output driver (DS100DF410 VOD register 0x2D). |
| 1.4.3 | Host TX de-emphasis | **[OPT]** | SFF-8431 allows/recommends pre-emphasis to compensate for PCB trace loss. DS100DF410 provides programmable de-emphasis. |

---

## 2. Retimer -- DS100DF410 (in signal path between ASIC and SFP+ cage)

Reference: TI DS100DF410 datasheet, ONL driver patch

The retimer sits between the BCM56846 WARPcore SerDes output and the SFP+ module input. Each retimer handles 4 channels. Address 0x27 on the port's I2C bus.

### 2.1 Retimer Initialization

| # | Step | Priority | Details |
|---|------|----------|---------|
| 2.1.1 | Verify I2C access to retimer | **[MUST]** | i2c_smbus_read_byte_data(bus, 0x27, reg). Retimer at 0x27 on each port group's bus. |
| 2.1.2 | Select active channel(s) | **[MUST]** | Register 0xFF (CHANNELS): write to select/enable specific channels (4 channels per retimer, each channel = one SFP+ port). |
| 2.1.3 | Reset CDR | **[MUST]** | Register 0x0A (CDR_RST): toggle CDR reset. Required after any configuration change or to recover from loss-of-lock. Write reset bit, then clear it. |
| 2.1.4 | Wait for CDR lock | **[MUST]** | After reset, CDR must acquire phase lock on incoming data. Typical lock time: 50-100 ms. No link is possible without CDR lock. |
| 2.1.5 | Verify signal detect | **[MUST]** | Retimer must detect a valid input signal from the ASIC SerDes. If no signal detect, the ASIC TX path is not driving. |

### 2.2 Retimer Equalization

| # | Step | Priority | Details |
|---|------|----------|---------|
| 2.2.1 | CTLE (adaptive input EQ) | **[OPT]** | Register 0x31 (ADAPT_EQ_SM): adaptive equalization state machine. By default, DS100DF410 auto-adapts CTLE to input channel. Usually no manual config needed. |
| 2.2.2 | DFE (decision feedback EQ) | **[OPT]** | Register 0x1E (PFD_PRBS_DFE): 5-tap DFE is self-calibrating. Adapts automatically when signal is first detected after CDR lock. |
| 2.2.3 | TX de-emphasis / VOD | **[OPT]** | Register 0x2D (DRV_SEL_VOD): output driver voltage and de-emphasis toward SFP+ module. Register 0x15 (TAP_DEM): FIR tap settings. Tune if SFP+ reports marginal RX power. |
| 2.2.4 | VEO / CDR bandwidth | **[OPT]** | Register 0x36 (VEO_CLK_CDR_CAP): CDR loop bandwidth tuning. Default is usually fine for 10.3125 Gbps. |

### 2.3 Retimer Direction

| # | Step | Priority | Details |
|---|------|----------|---------|
| 2.3.1 | Configure both directions | **[MUST]** | The retimer is bidirectional: ASIC-to-SFP (TX path) and SFP-to-ASIC (RX path). Both directions need CDR lock. Ensure the retimer is configured for the correct channel mapping. |
| 2.3.2 | SFP eq1 vs QSFP eq2 mode | **[MUST]** | Per project notes: SFP ports use eq1 equalization profile, QSFP ports use eq2. Must match the physical channel characteristics. |

---

## 3. Physical Medium Attachment (PMA) Layer -- SerDes Configuration

Reference: IEEE 802.3 Clause 45 (DEVAD=1), BCM56846 WARPcore

### 3.1 SerDes TX Path Enable

| # | Step | Priority | Details |
|---|------|----------|---------|
| 3.1.1 | Ensure PLL lock (LC PLL) | **[MUST]** | BCM56846 uses 156.25 MHz reference clock (CDK_CHIP_CONFIG: DCFG_LCPLL_156). All 17/18 TX PLLs must report locked. PLL lock is set during bmd_init(). Without PLL lock, no valid TX clock exists. |
| 3.1.2 | Enable SerDes TX driver | **[MUST]** | The WARPcore SerDes TX must be enabled. This should happen via bmd_port_mode_set(bmdPortMode10000fd). Verify by checking that far-end (or retimer) sees signal detect. |
| 3.1.3 | De-assert TX electrical idle | **[MUST]** | SerDes TX must exit electrical idle state to begin transmitting. TX idle = no signal on the differential pair. Controlled by the WARPcore firmware/registers. |
| 3.1.4 | Verify via CL45 reg 1.9 (TX Disable) | **[MUST]** | Register 1.9 bit 0: global TX disable. Must be 0. Per-lane bits (if applicable): bits 1-3. All must be 0 for TX to be active. |

### 3.2 SerDes Lane Configuration

| # | Step | Priority | Details |
|---|------|----------|---------|
| 3.2.1 | Lane polarity (TX and RX) | **[MUST]** | If board traces swap P/N on any lane, polarity inversion must be configured in the SerDes. Wrong polarity = no link. Per-lane TX_PN_SWAP and RX_PN_SWAP bits. |
| 3.2.2 | Lane mapping / remapping | **[MUST]** | BCM56846 SVK uses lane swap value 0x1032 (swaps lanes 0<->1, 2<->3). This is board-specific. Wrong lane mapping = data on wrong physical lane = no link even if signal is present. |
| 3.2.3 | Verify port-to-physical mapping | **[MUST]** | Front-panel port 1 = BCM physical port 9 (NOT port 1). Mapping: front port N -> BCM port (N+8). Using the wrong physical port means configuring the wrong SerDes. |

### 3.3 SerDes TX Signal Quality

| # | Step | Priority | Details |
|---|------|----------|---------|
| 3.3.1 | TX pre-emphasis (FIR taps) | **[OPT]** | WARPcore provides a 3-tap TX FIR filter (pre-cursor, main, post-cursor). Default taps may work for short traces. Tune if eye diagram is poor or BER is high. Accessed via WARPcore MDIO registers. |
| 3.3.2 | TX amplitude / driver current | **[OPT]** | TX output swing. Default is usually adequate. Increase for longer PCB traces between ASIC and retimer. |
| 3.3.3 | Reference clock verification | **[MUST]** | 156.25 MHz reference clock must be present and stable. Set via CDK_CHIP_CONFIG_SET(unit, DCFG_LCPLL_156). If clock is wrong, PLL will not lock and SerDes will not function. |

### 3.4 PMA Clause 45 Registers (DEVAD=1)

| # | Register | Name | Priority | Key Bits |
|---|----------|------|----------|----------|
| 3.4.1 | 1.0 | PMA/PMD Control 1 | **[MUST]** | Bit 15: Reset (self-clearing, write 1 to reset PMA). Bit 14: Loopback enable. Bit 13:6: Speed selection (1001 = 10G). Bit 11: Low power mode (must be 0 for operation). Bit 2: PMA local loopback. |
| 3.4.2 | 1.1 | PMA/PMD Status 1 | **[MUST]** | Bit 7: Fault (1 = fault detected). Bit 2: Link status (1 = PMA link up, latching low -- read twice). Bit 1: Low power ability. |
| 3.4.3 | 1.7 | PMA/PMD Control 2 | **[MUST]** | Bits 3:0: PMA type selection. For 10GBASE-R: value depends on specific PMD (SR=0x8, LR=0x9, ER=0xA, LRM=0x4). Must match the actual PHY/optics type. |
| 3.4.4 | 1.8 | PMA/PMD Status 2 | **[MUST]** | Bit 14: TX fault. Bit 13: RX fault. Bit 11: Local loopback ability. Bits 7:0: PMA type abilities (which types are supported). |
| 3.4.5 | 1.9 | PMD TX Disable | **[MUST]** | Bit 0: Global TX disable. Bits 3:1: Per-lane TX disable (for multi-lane PHYs). All bits must be 0 for TX to be active. Optional feature -- check bit 1.8.8 for ability. |
| 3.4.6 | 1.10 | PMD Signal Detect | **[MUST]** | Bit 0: Global signal detect (1 = signal present on all RX lanes). Per-lane signal detect in bits 3:1. If 0, no optical/electrical signal is reaching the PMD from the remote end. |
| 3.4.7 | 1.2/1.3 | Device Identifier | **[OPT]** | 32-bit PHY OUI. Read to verify correct PHY is being accessed. |
| 3.4.8 | 1.4 | Speed Ability | **[OPT]** | Bit 0: 10G capable. Sanity check. |

---

## 4. Physical Coding Sublayer (PCS) -- 10GBASE-R (Clause 49)

Reference: IEEE 802.3 Clause 49, Clause 45 (DEVAD=3)

### 4.1 PCS Configuration

| # | Step | Priority | Details |
|---|------|----------|---------|
| 4.1.1 | 64B/66B encoding enable | **[MUST]** | 10GBASE-R uses 64B/66B line coding (NOT 8B/10B). This is the defining characteristic of 10GBASE-R. Enabled implicitly when PCS type is set to 10GBASE-R. Line rate: 10.3125 GBd (10G payload + 3.125% coding overhead). |
| 4.1.2 | Scrambler enable | **[MUST]** | Clause 49 requires a self-synchronizing scrambler using polynomial x^58 + x^39 + 1. The scrambler operates on the 64-bit payload of each 66-bit block (NOT on the 2-bit sync header). Must be enabled -- unscrambled data will not achieve block lock at the receiver. |
| 4.1.3 | Descrambler enable | **[MUST]** | Receiver-side descrambler using the same polynomial. Self-synchronizing: no seed exchange needed. Automatically syncs after receiving valid scrambled data. |

### 4.2 Block Synchronization (Clause 49 Section 49.2.13)

| # | Step | Priority | Details |
|---|------|----------|---------|
| 4.2.1 | Block lock acquisition | **[MUST]** | Receiver scans incoming bitstream for valid sync headers (01 or 10). Lock acquired after **64 consecutive valid** sync headers with zero invalid headers. This is the fundamental PCS synchronization mechanism. |
| 4.2.2 | Block lock loss detection | **[MUST]** | Lock is lost after receiving **65 invalid sync headers** (neither 01 nor 10) within a monitoring window. On lock loss, PCS resets to searching state and must re-acquire. |
| 4.2.3 | Verify block_lock via reg 3.32 | **[MUST]** | Register 3.32 bit 0: block_lock (1 = locked). This is the primary PCS link-up indicator. If block_lock = 0, the PCS cannot decode data. |
| 4.2.4 | Check hi_ber | **[MUST]** | Register 3.32 bit 1: hi_ber (1 = high bit error rate detected). hi_ber is set when too many invalid sync headers are seen within a window. PCS link is considered DOWN if hi_ber = 1, even if block_lock = 1. |
| 4.2.5 | PCS link status (composite) | **[MUST]** | Register 3.32 bit 12: PCS receive link status = (block_lock AND NOT hi_ber). This single bit summarizes: is the PCS receiving valid 10GBASE-R data? |

### 4.3 PCS Error Monitoring

| # | Step | Priority | Details |
|---|------|----------|---------|
| 4.3.1 | Read latched status in reg 3.33 | **[OPT]** | Bit 15: block_lock latched low (1 = block lock was lost since last read). Bit 14: hi_ber latched high (1 = hi_ber was asserted since last read). Useful for detecting intermittent errors. |
| 4.3.2 | Errored blocks counter (reg 3.33) | **[OPT]** | Bits 7:0: count of errored blocks (saturates at 0xFF). Read and clear to monitor BER. Non-zero during normal operation indicates signal quality issues. |
| 4.3.3 | BER counter (reg 3.44) | **[OPT]** | 10GBASE-R BER high-order counter. Together with other counters, gives the actual bit error rate. Useful for margin testing. |

### 4.4 PCS Clause 45 Registers (DEVAD=3)

| # | Register | Name | Priority | Key Bits |
|---|----------|------|----------|----------|
| 4.4.1 | 3.0 | PCS Control 1 | **[MUST]** | Bit 15: Reset (self-clearing). Bit 14: Loopback. Bit 13:6: Speed selection. Bit 11: Low power. |
| 4.4.2 | 3.1 | PCS Status 1 | **[MUST]** | Bit 7: Fault. Bit 2: PCS receive link status (latching low). |
| 4.4.3 | 3.32 | 10GBASE-R Status 1 | **[MUST]** | Bit 12: 10GBASE-R PCS link (block_lock AND NOT hi_ber). Bit 1: hi_ber. Bit 0: block_lock. |
| 4.4.4 | 3.33 | 10GBASE-R Status 2 | **[OPT]** | Bit 15: block_lock latched low. Bit 14: hi_ber latched high. Bits 7:0: errored blocks counter. |
| 4.4.5 | 3.34-3.35 | Test Pattern Seed | **[OPT]** | Used for PRBS test patterns. Not needed for normal operation. |
| 4.4.6 | 3.36 | Test Pattern Control | **[OPT]** | Enable/select test patterns (PRBS31, etc.) for diagnostics. |

---

## 5. XGMII / MAC Layer

Reference: IEEE 802.3 Clause 46 (RS and XGMII), BCM56846 XLMAC

### 5.1 MAC Reset and Enable

| # | Step | Priority | Details |
|---|------|----------|---------|
| 5.1.1 | MAC reset | **[MUST]** | Reset XLMAC for the port. Clears internal state, FIFOs, counters. Must be done before enabling. In BCM56846, this is part of bmd_init() or explicit register write to XLMAC_CTRL. |
| 5.1.2 | XLMAC TX enable | **[MUST]** | Enable the MAC transmit path. Without this, no Ethernet frames (or even idle/fault ordered sets) are sent toward the PCS/SerDes. In XLMAC_CTRL register: TX_EN bit. |
| 5.1.3 | XLMAC RX enable | **[MUST]** | Enable the MAC receive path. Without this, received frames are discarded at the MAC. In XLMAC_CTRL register: RX_EN bit. |
| 5.1.4 | Set port speed to 10G | **[MUST]** | XLMAC must be configured for 10G speed mode. Done via bmd_port_mode_set(bmdPortMode10000fd). Mismatch = garbled frames or no link. |
| 5.1.5 | Verify both TX and RX are active | **[MUST]** | Read back XLMAC_CTRL to confirm TX_EN and RX_EN are both set. bmd_port_mode_set() should do this, but verify. |

### 5.2 MAC Configuration

| # | Step | Priority | Details |
|---|------|----------|---------|
| 5.2.1 | Inter-packet gap (IPG) | **[OPT]** | Default 12 bytes per IEEE 802.3. Configurable in XLMAC_TX_CTRL. Reducing can increase throughput but may cause interop issues. Leave at default. |
| 5.2.2 | CRC mode | **[OPT]** | XLMAC can append CRC (normal), pass-through, or replace. Default: append. Set in XLMAC_TX_CTRL and XLMAC_RX_CTRL. |
| 5.2.3 | Max frame size | **[OPT]** | XLMAC_RX_MAX_SIZE: default 1518 (or 1522 for VLAN). Set to 9216+ for jumbo frame support. |
| 5.2.4 | Strict preamble | **[OPT]** | Some MACs can check preamble strictly. Usually disabled for interop. |
| 5.2.5 | Pause frame handling | **[OPT]** | XLMAC_PAUSE_CTRL: enable/disable 802.3x flow control. Not needed for basic link-up. |

### 5.3 Forwarding Enable

| # | Step | Priority | Details |
|---|------|----------|---------|
| 5.3.1 | EPC_LINK_BMAP update | **[MUST]** | BCM56846 uses EPC_LINK_BMAP to control which ports participate in forwarding. The port's bit must be set for the switch to forward packets to/from this port. Without this, MAC is up but traffic is blackholed. |
| 5.3.2 | Port enable in ING/EGR | **[MUST]** | Ingress and egress port enable bits. Part of bmd_switching_init() for default config, but may need per-port enable if ports come up individually. |
| 5.3.3 | VLAN membership | **[OPT]** | Port must be in at least one VLAN to forward L2 traffic. Default VLAN 1 is configured by bmd_switching_init(). |

### 5.4 Link Fault Signaling (IEEE 802.3 Clause 46)

| # | Step | Priority | Details |
|---|------|----------|---------|
| 5.4.1 | Understand fault ordered sets | **[MUST]** | Local fault: sequence ordered set with type=0x01 (|Seq||0x00||0x00||0x01| repeated across 4 XGMII columns). Remote fault: type=0x02 (|Seq||0x00||0x00||0x02|). These are sent at the XGMII level. |
| 5.4.2 | Fault detection threshold | **[MUST]** | A fault is declared when **4 fault ordered sets of the same type** are received within **128 XGMII columns**, with no intervening fault of a different type. |
| 5.4.3 | Fault clear threshold | **[MUST]** | Fault is cleared after **128 columns** without any local or remote fault ordered set. |
| 5.4.4 | RS behavior on local fault RX | **[MUST]** | When RS receives local_fault from PCS (meaning our receiver lost signal): RS stops sending MAC data on TX path and continuously sends remote_fault toward the remote end. |
| 5.4.5 | RS behavior on remote fault RX | **[MUST]** | When RS receives remote_fault (meaning remote end lost OUR signal): RS stops sending MAC data on TX path and continuously sends IDLE. This is the normal "waiting for link" state. |
| 5.4.6 | Normal link-up sequence | **[MUST]** | Both ends: (1) SerDes TX active -> (2) remote PCS achieves block_lock -> (3) remote stops sending local_fault -> (4) local stops receiving remote_fault -> (5) RS returns to normal -> (6) link is UP. Both sides must complete this simultaneously. |

---

## 6. Clause 22 MDIO -- WARPcore Access

Reference: IEEE 802.3 Clause 22, Broadcom WARPcore programming model

The BCM56846 WARPcore SerDes is accessed via Clause 22 MDIO with extensions for block selection and per-lane addressing. This is how you read/write SerDes registers from the CPU.

### 6.1 Basic Access

| # | Step | Priority | Details |
|---|------|----------|---------|
| 6.1.1 | MDIO bus initialization | **[MUST]** | CL22 MDIO uses MDC (clock) and MDIO (data). Frame format: preamble (32x 1-bits) + ST=01 + OP (01=write, 10=read) + PHYAD (5 bits) + REGAD (5 bits) + TA + 16-bit data. |
| 6.1.2 | PHY address determination | **[MUST]** | Each WARPcore block has a base MDIO address. BCM56846 maps physical ports to MDIO addresses. The PHYAD in the CL22 frame selects which WARPcore block. |

### 6.2 Block Select and AER

| # | Step | Priority | Details |
|---|------|----------|---------|
| 6.2.1 | Block select (register 0x1F) | **[MUST]** | WARPcore uses CL22 register 0x1F as a "block address" register. Write the upper block address to 0x1F, then access registers 0x00-0x1E within that block. This extends the 5-bit CL22 register space to the full WARPcore register map. |
| 6.2.2 | AER (Address Extension Register) | **[MUST]** | Block 0xFFD0, register 0x1E. Write lane index to select which lane within the WARPcore quad is being accessed. Values: 0=lane0, 1=lane1, 2=lane2, 3=lane3, 0xFFFF=broadcast to all lanes. Must be set before any per-lane register access. |
| 6.2.3 | Multi-MMD mode enable | **[OPT]** | WARPcore can emulate CL45 device types (MMDs) via CL22 access. Writing to the appropriate configuration register enables mapping CL22 block/register addresses to CL45 DEVAD/register addresses. Needed if firmware expects CL45-style access but hardware only provides CL22 MDIO. |

### 6.3 Common WARPcore Register Blocks

| # | Block | Purpose | Priority |
|---|-------|---------|----------|
| 6.3.1 | 0x8000 | Digital control (speed, mode) | **[MUST]** |
| 6.3.2 | 0x8010 | TX driver (amplitude, pre-emphasis FIR taps) | **[OPT]** |
| 6.3.3 | 0x8050 | RX control (DFE, CDR) | **[OPT]** |
| 6.3.4 | 0x8060 | PLL configuration | **[MUST]** |
| 6.3.5 | 0x80A0 | Lane swap / polarity | **[MUST]** |
| 6.3.6 | 0xFFD0 | AER (lane select) | **[MUST]** |

---

## 7. Clause 45 MDIO -- 10G PHY Management

Reference: IEEE 802.3 Clause 45

### 7.1 Frame Format

| # | Field | Details | Priority |
|---|-------|---------|----------|
| 7.1.1 | ST (Start) | 00 for Clause 45 (vs 01 for Clause 22). | **[MUST]** |
| 7.1.2 | OP (Operation) | 00 = Address frame. 01 = Write frame. 11 = Read frame. 10 = Post-read-increment-address. | **[MUST]** |
| 7.1.3 | PRTAD (Port Address) | 5 bits, selects which PHY (0-31). | **[MUST]** |
| 7.1.4 | DEVAD (Device Address) | 5 bits, selects MMD device type within the PHY. | **[MUST]** |
| 7.1.5 | Two-frame access | First frame (OP=00) sets the register address within the DEVAD. Second frame (OP=01 or 11) performs the actual read or write. This is fundamentally different from CL22 which is single-frame. | **[MUST]** |

### 7.2 Device Types (DEVAD)

| # | DEVAD | Device | Key Registers | Priority |
|---|-------|--------|---------------|----------|
| 7.2.1 | 1 | PMA/PMD | 1.0 (ctrl), 1.1 (status), 1.7 (type), 1.8 (status2), 1.9 (TX disable), 1.10 (signal detect) | **[MUST]** |
| 7.2.2 | 2 | WIS | WAN interface sublayer. Not used for LAN 10GBASE-R. | N/A |
| 7.2.3 | 3 | PCS | 3.0 (ctrl), 3.1 (status), 3.32 (10GR status1), 3.33 (10GR status2) | **[MUST]** |
| 7.2.4 | 4 | PHY XS | 4.0 (ctrl), 4.1 (status), 4.24 (lane status). PHY-side XGMII extender. Reports per-lane alignment status. | **[OPT]** |
| 7.2.5 | 7 | Auto-Negotiation | 7.0 (ctrl), 7.1 (status), 7.16-7.19 (advertised ability). **NOT used for 10GBASE-R over SFP+** (no AN defined for optical SFP+). Only relevant for 10GBASE-KR (backplane). | N/A for SFP+ |
| 7.2.6 | 30 | Vendor Specific 1 | Implementation-dependent. WARPcore uses this for proprietary registers. | **[OPT]** |
| 7.2.7 | 31 | Vendor Specific 2 | Implementation-dependent. | **[OPT]** |

### 7.3 Key CL45 Register Summary

| Register | Name | Read at link-up | Expected value for link UP |
|----------|------|-----------------|---------------------------|
| 1.1 bit 2 | PMA link status | Yes (read twice, latching low) | 1 |
| 1.8 bit 14 | TX fault | Yes | 0 |
| 1.8 bit 13 | RX fault | Yes | 0 |
| 1.10 bit 0 | Signal detect | Yes | 1 |
| 3.1 bit 2 | PCS link status | Yes (read twice, latching low) | 1 |
| 3.32 bit 0 | block_lock | Yes | 1 |
| 3.32 bit 1 | hi_ber | Yes | 0 |
| 3.32 bit 12 | 10GBASE-R receive link | Yes | 1 |

---

## 8. Link Detection and Maintenance

### 8.1 Link-Up Detection Sequence

| # | Step | Priority | Details |
|---|------|----------|---------|
| 8.1.1 | Poll PMD signal detect (1.10) | **[MUST]** | First indicator: is there any signal? If 0, problem is physical (fiber, SFP, retimer, or SerDes TX on remote end). |
| 8.1.2 | Poll PCS block_lock (3.32 bit 0) | **[MUST]** | Second indicator: can we decode the signal? If signal_detect=1 but block_lock=0, the signal is present but corrupted (wrong speed, encoding, scrambler, lane swap, or severe signal quality issue). |
| 8.1.3 | Check hi_ber (3.32 bit 1) | **[MUST]** | Third indicator: is error rate acceptable? If block_lock=1 but hi_ber=1, signal quality is too poor for reliable operation. Check equalization, retimer, and optics. |
| 8.1.4 | Check PMA/PCS link status (1.1, 3.1) | **[MUST]** | Composite link status bits. Latching low: if ever 0 since last read, returns 0 on first read, then returns current state on second read. Always read twice to get current status. |
| 8.1.5 | Check for TX/RX faults (1.8) | **[MUST]** | Bit 14 (TX fault) and bit 13 (RX fault) indicate MAC/PCS-level fault conditions. Should both be 0 for a healthy link. |
| 8.1.6 | Verify no fault ordered sets | **[MUST]** | Even with PCS link up, the MAC may be sending/receiving fault ordered sets (local_fault or remote_fault). The link is only truly UP when fault signaling has cleared on both ends. |
| 8.1.7 | Enable MAC TX/RX | **[MUST]** | Once PCS link is confirmed, enable XLMAC TX and RX if not already enabled. Some implementations enable MAC first and let fault signaling handle the sequencing. |
| 8.1.8 | Set EPC_LINK_BMAP | **[MUST]** | Enable the port in the forwarding bitmap. This is the final step: the port is now part of the switching fabric. |

### 8.2 Auto-Negotiation (or lack thereof)

| # | Step | Priority | Details |
|---|------|----------|---------|
| 8.2.1 | No AN for 10GBASE-R over SFP+ | **[MUST]** | IEEE 802.3 does NOT define auto-negotiation for 10GBASE-R over optical SFP+. There is no Clause 73 (backplane AN) or Clause 37 (1G fiber AN) equivalent for 10G SFP+. Link is always forced mode at 10G. |
| 8.2.2 | Disable AN in SerDes | **[MUST]** | Ensure WARPcore auto-negotiation is disabled for SFP+ ports. AN enable bit in CL45 register 7.0 bit 12 must be 0 (or AN DEVAD not configured). If AN is accidentally enabled, the port will attempt to negotiate and fail. |
| 8.2.3 | Force speed = 10G | **[MUST]** | bmd_port_mode_set(bmdPortMode10000fd) forces 10G full-duplex. No negotiation. Both ends must agree on 10G. |

### 8.3 Link Monitoring (Steady State)

| # | Step | Priority | Details |
|---|------|----------|---------|
| 8.3.1 | Periodic status polling | **[OPT]** | Poll CL45 registers (1.1, 3.1, 3.32) at regular intervals (e.g., 1 second) to detect link drops. |
| 8.3.2 | RX_LOS GPIO monitoring | **[OPT]** | Monitor PCA9506 for RX_LOS assertion (fiber pulled, remote TX off). Faster than MDIO polling. |
| 8.3.3 | DDM monitoring | **[OPT]** | Periodically read A2h DDM to detect optics degradation (TX power dropping, temperature rising). |
| 8.3.4 | Error counter monitoring | **[OPT]** | Read PCS error counters (reg 3.33 bits 7:0) to detect increasing BER before link drops. |
| 8.3.5 | Link-down recovery | **[MUST]** | On link loss: (1) check RX_LOS, (2) check signal_detect, (3) check block_lock, (4) if all fail, consider retimer CDR reset, (5) re-run SerDes init if needed. |

---

## Appendix A: Complete Link-Up Sequence (Summary)

Ordered steps from power-on to forwarding traffic:

```
1.  ASIC init: bmd_reset() + bmd_init()          -- PLL lock, SBUS ring, port init
2.  SFP module detect: read MOD_ABS GPIO          -- confirm module present
3.  SFP identity: read A0h I2C                    -- confirm 10G module
4.  SFP TX enable: clear TX_DISABLE GPIO          -- laser on
5.  Retimer init: channel select + CDR reset      -- retimer in path
6.  Retimer CDR lock: wait + verify               -- retimer locked
7.  SerDes config: lane swap, polarity             -- match board layout
8.  Port mode: bmd_port_mode_set(10000fd)          -- SerDes + PCS + MAC for 10G
9.  Verify PLL lock                                -- SerDes clock OK
10. Wait for signal detect (1.10)                  -- far end signal arriving
11. Wait for block_lock (3.32 bit 0)               -- PCS synchronized
12. Verify hi_ber = 0 (3.32 bit 1)                 -- BER acceptable
13. Verify no TX/RX faults (1.8)                   -- clean signal path
14. Confirm fault signaling cleared                -- no local/remote fault
15. XLMAC TX + RX enable                           -- MAC active
16. EPC_LINK_BMAP set                              -- forwarding enabled
17. Link is UP -- traffic can flow
```

## Appendix B: Diagnostic Troubleshooting Tree

```
No link?
  |
  +-- RX_LOS = 1? (no light from remote)
  |     +-- Check fiber connection
  |     +-- Check remote TX_DISABLE / TX_FAULT
  |     +-- Check remote SFP module
  |
  +-- RX_LOS = 0 but signal_detect (1.10) = 0?
  |     +-- Retimer not forwarding RX signal to ASIC
  |     +-- Check retimer CDR lock (RX direction)
  |     +-- Check retimer channel enable
  |
  +-- signal_detect = 1 but block_lock (3.32) = 0?
  |     +-- Signal present but PCS cannot sync
  |     +-- Wrong lane mapping / polarity (most likely!)
  |     +-- Wrong speed/encoding on one end
  |     +-- Severe signal integrity issue
  |
  +-- block_lock = 1 but hi_ber = 1?
  |     +-- Signal synced but too many errors
  |     +-- Tune SerDes / retimer equalization
  |     +-- Check optics DDM (TX/RX power levels)
  |     +-- Possible marginal fiber or dirty connector
  |
  +-- PCS link up but MAC reports fault?
  |     +-- Check XLMAC TX/RX enable
  |     +-- Check for link fault signaling (local/remote fault)
  |     +-- Both ends must stop sending fault ordered sets
  |
  +-- MAC up but no traffic forwarding?
        +-- Check EPC_LINK_BMAP
        +-- Check VLAN membership
        +-- Check ingress/egress port enable
```

---

Sources:
- [SFF-8431 SFP+ Electrical Specification](https://www.gigalight.com/downloads/standards/sff-8431.pdf)
- [SFF-8472 Management Interface for SFP+ (SNIA)](https://members.snia.org/document/dl/25916)
- [SFF-8472 DDM Application Note (Coherent/Finisar AN-2030)](https://cdn.hackaday.io/files/21599924091616/AN_2030_DDMI_for_SFP_Rev_E2.pdf)
- [IEEE 802.3 Clause 45 MDIO Draft (Working Paper)](https://www.ieee802.org/3/ak/public/jan03/WP2p0_cls45.pdf)
- [IEEE 802.3 Clause 49 PCS 64B/66B Introduction (UNH-IOL)](https://www.iol.unh.edu/sites/default/files/knowledgebase/10gec/10GbE_Cl49.pdf)
- [IEEE 802.3 Clause 49 PCS Test Suite (UNH-IOL)](https://www.iol.unh.edu/sites/default/files/testsuites/10gec/Clause_49_PCS_Test_Suite_v1.0.pdf)
- [10GbE Link Fault Signaling (UNH-IOL)](https://www.iol.unh.edu/sites/default/files/knowledgebase/10gec/10GbE_fault_signaling.pdf)
- [Local Fault and Remote Fault in Ethernet (Christopher Hart)](https://chrisjhart.com/Ethernet-Local-Fault-and-Remote-Fault/)
- [DS100DF410 Retimer (Texas Instruments)](https://www.ti.com/product/DS100DF410)
- [DS100DF410 ONL Driver Patch (GitHub)](https://github.com/opennetworklinux/linux/blob/master/3.2.65-1+deb7u2/patches/driver-ds100df410-retimer.patch)
- [MDIO Background (Total Phase)](https://www.totalphase.com/support/articles/200349206-mdio-background/)
- [Broadcom SerDes Configuration Guide](https://docs.broadcom.com/doc/DBG16S-AN1-PUB)
- [Intel 10GBASE-R PCS Features](https://www.intel.com/content/www/us/en/docs/programmable/683573/current/10gbase-r-supported-features.html)
- [Intel XGMII Link Fault Handling](https://www.intel.com/content/www/us/en/docs/programmable/683426/18-1-18-1/xgmii-error-handling-link-fault.html)
- [IEEE 802.3 Section Four (2005)](http://magrawal.myweb.usf.edu/dcom/Ch3_802.3-2005_section4.pdf)
- [SFP+ Design Guide](https://www.optixcom.com/product_pdf/1.duplex/3-SFPplus/SFP+%20Design%20Guide.pdf)
