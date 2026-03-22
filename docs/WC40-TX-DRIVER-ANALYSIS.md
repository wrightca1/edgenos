# WARPcore TX Driver Analysis — Ghidra Decompilation of libopennsl.so.1

**Date**: 2026-03-19
**Source**: Ghidra headless analysis of libopennsl.so.1 (49MB, PPC32, from Cumulus 2.5)
**Target**: Understand how the Cumulus Memory SDK sets the WARPcore TX driver current

## Summary

The Cumulus SDK does NOT write TX_DRIVERr (0x8067) directly. It uses a multi-layer
abstraction with a firmware mailbox dispatch system. Direct register writes from
OpenMDK are ignored because the firmware manages the register through this mailbox.

## Complete Call Chain

```
phy_wc40_speed_set (0x0175e26c, 736 lines)
  ├── Sets sw_init_drive (offset 0x4f0) = 1 (software controls TX)
  ├── _phy_wc40_tx_control_get (0x0174f4a0, index=0xd=TXDRV_DFT_INX)
  │     └── Returns idriver=9, ipredrive=9, post2=0 (defaults)
  ├── _phy_wc40_tx_control_set (0x0175308c)
  │     ├── Remaps data: val = data & 0xf | 0x7000000 | (data & 0x3f0)<<4 | (data & 0x7c00)<<6
  │     ├── For each mask bit, writes val|prefix to PHY struct offset 0x4f0
  │     └── Calls FUN_0177b524 (SOC PHY register table dispatcher)
  │           ├── FUN_0177b524 (3620 bytes) — SOC register table manager
  │           │     ├── Sets up lane context (offset 0x18)
  │           │     └── Calls FUN_0177b418 (dispatch by string name)
  │           │           ├── Looks up register name in dispatch table
  │           │           └── Calls handler function pointer
  │           │                 └── Handler does actual MIIM write to 0x8067
  │           └── Dispatches for each field separately
  └── Sets sw_init_drive (offset 0x4f0) = 6 (firmware resumes control)
```

## Key Data Structures

### PHY Control Structure Offsets (from iVar10/iVar23)
| Offset | Field | Purpose |
|--------|-------|---------|
| 0x17 | lane_num | Current lane number (byte) |
| 0x18 | lane_mode | Lane access mode |
| 0x1a0 | phy_addr | PHY MIIM address |
| 0x294 | something | Lane state |
| 0x2a4 | phy_flags | PHY configuration flags |
| 0x4d0 | per-WC data | Per-WARPcore context (0xe4 bytes each) |
| **0x4e0** | **mode** | Current operating mode |
| **0x4f0** | **sw_init_drive** | TX driver command word / firmware mailbox |
| 0x4fc | link_state | Link status |
| 0x520 | something | Flag |
| 0x544 | features | Feature bits |
| 0x55c | something | Saved state |

### TX Driver Data Remapping (line 38 of WC40_REG_MODIFY)
```
Input (param_5) = (post2 << 12) | (idriver << 8) | (ipredrive << 4) | preemph
                = 0x0990 for idriver=9, ipredrive=9

Output (uVar4) = input[3:0]          |  // preemph → bits [3:0]
                 0x7000000            |  // command prefix
                 (input[9:4] << 4)    |  // shifted idriver+ipredrive fields
                 (input[14:10] << 6)     // shifted post2 field

For 0x0990: output = 0x07001900
```

### Mask-to-Prefix Mapping (TX_DRIVERr field writes)
| Mask bit | Prefix | Field |
|----------|--------|-------|
| 0x20 | 0x00/0x08 | Main driver + ELECIDLE |
| 0x01 | 0x10 | Field 1 (preemph?) |
| 0x02 | 0x20 | Field 2 |
| 0x04 | 0x30 | Field 3 |
| 0x08 | 0x40 | Field 4 |
| 0x10 | 0x50 | Field 5 |

### Dispatch Table (at DAT + 0x1773438)
Array of {char* name, function_ptr handler} pairs (up to 100 entries).
The string-based lookup matches register names to handler functions.
Each handler knows how to write one specific WC register.

## Why OpenMDK Writes Fail

1. OpenMDK writes directly to WC register 0x8067 via MIIM
2. The WC firmware controls this register and ignores direct writes
3. The Cumulus SDK communicates with firmware via the dispatch/mailbox system
4. The firmware reads the command from its mailbox and applies settings
5. Without the SDK's abstraction layer, the firmware never sees our write

## How to Fix

### Option A: Replicate the Firmware Mailbox
Write the remapped command value (0x07001900 for idriver=9) to the correct
firmware mailbox register, then trigger the firmware to process it.
Need to identify the actual hardware mailbox register (not struct offset 0x4f0).

### Option B: Find the Handler Function
Use Ghidra to find the dispatch table entries and identify the handler for
TX_DRIVERr. Decompile the handler to find the exact MIIM write sequence.

### Option C: Find the SOC PHY Control Handler
The tx_control_set calls FUN_01752d64 with control IDs:
  - ID 2 = SOC_PHY_CONTROL_DRIVER_CURRENT (idriver)
  - ID 3 = SOC_PHY_CONTROL_PRE_DRIVER_CURRENT (ipredrive)
  - ID 0x62 = SOC_PHY_CONTROL_DRIVER_POST2_CURRENT (post2)
These store values to the PHY struct, then FUN_01752890 triggers the dispatch
to actually apply them via MIIM. Decompile the dispatch handler to find the
actual MIIM write.

### Option D: Capture INITIAL BOOT MIIM via GDB
The existing SERDES_WC_INIT.md capture is from a port flap, not initial boot.
The port flap may re-apply cached values. A fresh GDB capture during the
initial Cumulus boot would show the complete TX driver init MIIM sequence,
including the firmware mailbox writes.

### Option E: Try writing directly to UC_CTRLr mailbox
The firmware reads commands from UC_CTRLr (0x820E). The remapped value
0x07001900 (for idriver=9) might need to be written to a related register
that the firmware polls. Try writing the remapped value to UC_CTRLr
SUPPLEMENT_INFO field.

## DEFINITIVE FINDING: All WC analog registers are firmware-controlled

Every analog TX register we tried to write is immediately overridden by the firmware:
- TX_DRIVERr (0x8067): always reads 0x2001 (idrv=0, pdrv=0)
- TXB_TX_DRIVERr (0x80A7): same
- FIRMWARE_MODEr (0x81F2): always reads 0x0707
- ANATXACONTROL0 (0x8061): always reads 0x0109 (PRBS_EN=1)

Attempted write methods that ALL failed:
- phy_aer_iblk_write (OpenMDK PHY driver)
- PHY_BUS_WRITE with CL22 page-select
- CL45 MIIM write via cdk_xgs_miim_write
- PhyConfig_TxIDrv / PhyConfig_TxPreIDrv API
- UC_CTRLr firmware STOP/RESUME/RESTART sequence
- PLL sequencer stop/write/restart
- All with broadcast AER, per-lane AER, and no AER

The WC firmware v0x242F microcontroller:
- Loaded OK (version reads back correctly)
- Manages ALL analog registers autonomously
- Does NOT respond to UC_CTRLr commands (READY_FOR_CMD=0)
- Requires the Memory SDK's SOC PHY register dispatch protocol
- The dispatch protocol uses a string-based lookup table and function pointers

## Additional Finding: tx_control_set Field Calls
```
FUN_01752d64(unit, port, lane, 2, idriver)     ; SOC_PHY_CONTROL_DRIVER_CURRENT
FUN_01752d64(unit, port, lane, 3, ipredrive)   ; SOC_PHY_CONTROL_PRE_DRIVER_CURRENT
FUN_01752d64(unit, port, lane, 0x62, post2)    ; SOC_PHY_CONTROL_DRIVER_POST2_CURRENT
FUN_01752890(unit, port, lane, 1, combined, 0x20) ; main driver write
FUN_01752890(unit, port, lane, 1, preemph, 0x01)  ; preemph write
```
The combined value = (idriver << 8) | (ipredrive << 4) with mask 0x20.

## GDB Capture Reinterpretation
The page 0 / reg 0x18 = 0x8370 in SERDES_WC_INIT.md maps to WC register
0x0008 (AUTONEGLPABIL2r), NOT TX_DRIVERr. The actual TX driver MIIM write
may be a UC_CTRLr command or a different register entirely, captured as a
separate MIIM transaction in the GDB log that wasn't identified as TX-related.

## Decompiled Files
| File | Function | Size |
|------|----------|------|
| wc40_speed_set_decomp.c | phy_wc40_speed_set | 736 lines |
| wc40_init_decomp.c | phy_wc40_init | 55 lines |
| ind_lane_init_decomp.c | independent_lane_init | 426 lines |
| tx_control_set_decomp.c | tx_control_set | 107 lines |
| wc40_reg_modify_decomp.c | WC40_REG_MODIFY | 86 lines |
| wc40_tx_fir_default_set.c | tx_fir_default_set | 76 lines |
| wc40_firmware_mode_set.c | firmware_mode_set | 112 lines |
| wc40_fw_mailbox_decomp.c | SOC register table mgr | 517 lines |
| wc40_dispatch_table_decomp.c | Dispatch by name | 42 lines |

## Binary Analysis: MIIM Write Functions (2026-03-20)

Found 32 CMIC_MIIM_ADDRESS (0x4a0) writes. Real MIIM (via BAR0 register):
- 0x00876284: stw r0, 0x4a0(r31) — likely soc_miim_write
- 0x00877694: stw r11, 0x4a0(r29) — likely soc_miim_read
- 0x00ef22b4: stw r29, 0x4a0(r31) — PHY layer MIIM?
- 0x00f8518c: stw r11, 0x4a0(r27) — another MIIM function
- 0x01927be0: stw r0, 0x4a0(r31) — late in binary, could be WC PHY

Next step: Decompile functions at 0x00876284 and 0x01927be0 to find the
actual MIIM write sequence for TX_DRIVERr. These are called by the dispatch
handler chain and will show exact register/data values.

Cumulus switchd (from CumulusLinux-2.5.0-powerpc.bin) cannot be used for
GDB capture because it exits immediately on sfs_config_get failure (license check).

GDB is now installed on the switch (apt-get install gdb).

## Firmware Analysis (2026-03-20/21)

### OpenMDK vs Cumulus firmware are DIFFERENT binaries
- OpenMDK wc40_ucode_b0_bin: 30,842 bytes, starts with 01 a8 02 00 03 (AJMP 0x0a8)
- Cumulus switchd firmware candidates: ~30,115 bytes each, starts with 75 a3 02 00 00 (MOV 0xa3,#0x02)
- Multiple copies in switchd at 0x01a5xxxx range (different versions for different modes?)

### Firmware extraction attempts
1. Extracted from CumulusLinux-2.5.0-powerpc.bin squashfs → switchd at /tmp/cumulus-root/usr/sbin/switchd
2. Found 8051-dense code blocks at 0x01d3xxxx (first attempt) — wrong, was data tables
3. Found header-matching candidates at 0x01a5564b (75 a3 02 00 00 pattern) — correct format
4. Loaded Cumulus firmware but FW_VER still shows 0x242F — firmware may not be loading correctly

### FW_VER = 0x242F may be from WC internal ROM
- FW_VER reads 0x242F on first WC block regardless of what firmware we download
- FW_VER reads 0x0000 on other WC blocks (no firmware)
- The WC may have an internal ROM firmware that always runs as fallback
- Downloaded firmware may only supplement/override specific behaviors

### Cumulus images available
- CumulusLinux-2.5.0-powerpc.bin (extracted, firmware candidates found)
- CumulusLinux-2.5.1-powerpc.bin (not yet extracted)
- CumulusLinux-2.5.0-amd64.bin (x86, might have firmware in different format)

### Next steps for firmware
1. Extract firmware from CumulusLinux-2.5.1-powerpc.bin (the actual version on the live switch)
2. Use Ghidra on the Cumulus switchd to find _phy_wc40_ucode_get and trace to the actual firmware array
3. Check if the firmware download is actually succeeding by reading DOWNLOAD_STATUS register
4. Try disabling CRC check (WARPCORE_CRC_DISABLE=1) in case CRC mismatch causes fallback to ROM
