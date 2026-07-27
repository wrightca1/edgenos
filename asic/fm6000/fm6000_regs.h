/*
 * fm6000_regs.h - FM6000 "Alta" register map (clean-room, RE-derived)
 *
 * Intel/Fulcrum FM6000 switch ASIC on the Arista DCS-7150S-52.
 *
 * SCOPE / PROVENANCE
 * ------------------
 * These are the register block bases and the specific offsets recovered by
 * *behavioral* reverse-engineering of the FocalPoint bring-up (disassembly of
 * init/microcode paths + live BAR0 reads on the 7150). See the RE writeups:
 *   - notes/analysis/phase7g-fm6000-bringup-recovered.md  (init/ucode/BIST)
 *   - edgenos/FPDMA.md                                    (packet-DMA block)
 *   - edgenos/GAPS.md                                     (what still needs a trace)
 *
 * This header deliberately contains ONLY the handful of offsets we independently
 * recovered and can cite. It is NOT the proprietary 3508-macro Intel register
 * header (fm6000_api_regs_int) — that file is R&D-only and must not be vendored.
 * When we need the full namespace we generate our own from live register traces.
 *
 * ADDRESSING (important — two conventions live in the same BAR0)
 * -------------------------------------------------------------
 *  1. Switch CSRs are *word-addressed*: the SDK API takes a word index `a` and
 *     poke goes to byte offset (a << 2).  Use FM6000_CSR(a) below.
 *  2. The packet-DMA engine block is accessed by *raw byte offset* from BAR0
 *     (fpdma.ko does ioread32(bar0 + 0x50xx) directly).  Use FM6000_DMA_*.
 *  The DMA block's byte base 0x5000 == switch word index 0x1400; we keep the two
 *  access helpers separate so this never becomes a footgun.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __FM6000_REGS_H__
#define __FM6000_REGS_H__

#include <stdint.h>

/* ---- PCI identity (from lspci + fpdma alta_pci_table) ------------------- */
#define FM6000_VENDOR_INTEL     0x8086      /* post-acquisition vendor id     */
#define FM6000_DEVICE_ALTA      0x155b      /* == Fulcrum 1823:1770           */
#define FM6000_VENDOR_FULCRUM   0x1823
#define FM6000_DEVICE_FULCRUM   0x1770

/* ---- Register block bases (word indices; phase7g "The anchor") ---------- */
#define FM6000_BLK_JSS          0x0F000u    /* switch/scheduler subsystem     */
#define FM6000_BLK_MGMT2        0x1C000u    /* mgmt2: reset/PLL/BIST/fusebox  */
#define FM6000_BLK_SERDES_WR    0xB0500u    /* SerDes SBus controller (write) */
#define FM6000_BLK_SERDES_RD    0xC0500u    /* SerDes SBus controller (read)  */
#define FM6000_BLK_SERDES_PCIE  0xD1100u
#define FM6000_BLK_EPL          0xE0000u    /* Ethernet Port Logic / PCS/MAC  */
#define FM6000_BLK_PARSER       0x100000u   /* programmable parser (MAPPER in)*/
#define FM6000_BLK_MAPPER       0x120000u   /* MAPPER microcode target        */
#define FM6000_BLK_FFU          0x300000u   /* FFU / AlgoMatch (TCAM)         */

/* ---- MGMT1: SOFT_RESET (word 0x00009) — VALIDATED LIVE 2026-07 ---------- */
/* CORRECTION: the phase7g skeleton mislabeled 0x1C03A as SOFT_RESET. 0x1C03A is
 * actually SCAN_CONFIG_DATA_IN (normal-operating-mode scan chain, below). The
 * real reset register is MGMT1 word 0x00009. A SET bit = that block HELD in
 * reset. Releasing MSB (core fabric) requires the microcode/tables loaded next,
 * and must be done AFTER the boot-controller bank-repair/freelist commands — a
 * bare MSB release into an unconfigured fabric hangs the CPU (learned the hard way). */
#define FM6000_REG_SOFT_RESET        0x00009u
#define FM6000_SOFT_RESET_PCIE       (1u << 0)  /* PCIe controller              */
#define FM6000_SOFT_RESET_MSB        (1u << 1)  /* core fabric (parser/FFU/L2AR)*/
#define FM6000_SOFT_RESET_FIBM       (1u << 2)  /* in-band-mgmt mailbox (NOT DMA)*/
#define FM6000_SOFT_RESET_JSS        (1u << 3)  /* JTAG/scan/SBus               */
#define FM6000_SOFT_RESET_EPL        (1u << 4)  /* Ethernet Port Logic (ports)  */

/* ---- MGMT2: normal operating mode (scan chain) — VALIDATED LIVE --------- */
/* Table 4-1 step 5. Stage SCAN_CONFIG_IN then write SCAN_CHAIN_IN=0xFFFFFFFF.
 * Our board bring-up does this pre-enum over the mgmt I2C slave; kept for a
 * from-scratch/edged path. */
#define FM6000_REG_SCAN_CONFIG_IN    0x1C03Au  /* stage 0x88800000/0x88008000/0x80000040 */
#define FM6000_REG_SCAN_CHAIN_IN     0x1C03Bu  /* =0xFFFFFFFF -> normal mode    */
#define FM6000_REG_SCAN_DATA_OUT     0x1C03Cu

/* ---- MGMT2: PLL/DLL status + DLL enable — VALIDATED LIVE ---------------- */
#define FM6000_REG_DLL_CTRL_HI       0x1C045u  /* enable bits [1:0] -> write 0x3 */
#define FM6000_REG_PLL_STAT          0x1C046u  /* Locked1/2[1:0]+DllLocked1/2[3:2]; 0x0F=all locked */

/* ---- MGMT2: boot controller — VALIDATED LIVE + datasheet Table 4-1 ------ */
#define FM6000_REG_PIN_STRAP_STAT    0x1C021u  /* BOOT_MODE straps (reads 0x208)*/
#define FM6000_REG_BOOT_CTRL         0x1C022u  /* Command field + status       */
#define FM6000_BOOT_STATUS_CMD_DONE  (1u << 4) /* CommandDone (poll after cmd) */
#define FM6000_BOOT_CTRL_EEPROM_DONE (1u << 5) /* EepromLoadDone               */
/* BOOT_CTRL:Command codes (datasheet §4 line 1202): */
#define FM6000_BOOT_CMD_FFU_SLICES   1u        /* Initialize FFU slice numbers */
#define FM6000_BOOT_CMD_BANK_REPAIR  2u        /* Apply bank memory repairs    */
#define FM6000_BOOT_CMD_FREELISTS    3u        /* Initialize all sched freelists*/

/* ---- PCIe controller + SerDes bring-up (word idx) — VALIDATED LIVE ------ */
/* fmPlatformSetupPCIe: the sequence that makes the FM6000 enumerate (done by
 * the board bring-up pre-enum). Offsets kept for reference. NB word 0x1400 ==
 * byte 0x5000 == the packet-DMA block below (two conventions, one block). */
#define FM6000_REG_PCI_CFG_1         0x01002u
#define FM6000_REG_PCI_ENDIANISM     0x01400u
#define FM6000_REG_PCI_TX_FRAME_LEN  0x01416u  /* MaxLen[15:0] MinLen[22:16]   */
#define FM6000_REG_PCI_DMA_CFG       0x01418u  /* DMAEn[5:4]                    */
#define FM6000_REG_PCI_CORE_CTRL_1   0x0141Du  /* CoreEnable[16]               */
#define FM6000_REG_PCI_SERDES_CTRL_1 0x01435u  /* TxOutputEn[20]+RefSel; 0xF121F34 = lanes on */
#define FM6000_REG_SBUS_CFG          0x0F000u  /* SBus controller out of reset */
#define FM6000_REG_SBUS_COMMAND      0x0F001u
#define FM6000_REG_SBUS_REQUEST      0x0F002u
#define FM6000_REG_SBUS_RESPONSE     0x0F003u

/* ---- MGMT2: BIST / built-in memory repair (BM engine) ------------------- */
#define FM6000_REG_BM_MAX_REPAIRS   0x1D08Au
#define FM6000_REG_BM_CONTROL       0x1D08Cu
#define FM6000_REG_BM_ENGINE_STATUS 0x1D08Eu   /* write 1, then poll          */
#define FM6000_REG_BM_START_OP      0x1D091u
#define FM6000_REG_BM_RESULT        0x1D70Eu   /* BIST_DONE_PASS / _FAIL      */

/* Fusebox / repair descriptor region: base + per-entry stride (128 entries). */
#define FM6000_REG_FUSEBOX_BASE     0x1D000u
#define FM6000_REG_FUSEBOX_E0       0x1D241u   /* observed live entry offsets  */
#define FM6000_REG_FUSEBOX_E1       0x1D261u
#define FM6000_REG_FUSEBOX_E2       0x1D281u
#define FM6000_REG_FUSEBOX_E3       0x1D2A1u
#define FM6000_REG_FUSEBOX_E4       0x1D2C1u

/* Per-SRAM MARCH descriptor window (~40 offsets, triplets/memory). Loop shape
 * recovered (32 memories x 7 words); the pattern *values* are runtime-computed
 * from switch-info tables -> TODO(live-trace), see GAPS.md A. */
#define FM6000_REG_MARCH_LO         0x1D400u
#define FM6000_REG_MARCH_HI         0x1D600u

/* ---- SBus slave registers for the SerDes SPICO uc (phase7g §c/2) -------- */
/* Driven through the SBus controller CSR window (FM6000_BLK_SERDES_WR). The
 * slave-register addresses below are fully recovered; the controller *framing*
 * at 0xB0500 (cmd/addr/data layout + ready poll) is TODO(live-trace). */
#define FM6000_SBUS_SPICO_CTRL      0xFD0Cu    /* 3=reset 1=enable 8=run 0=done*/
#define FM6000_SBUS_SPICO_IMEM_CTL  0xFD06u    /* 8=we; data-hi|0xC strobe... */
#define FM6000_SBUS_SPICO_ADDR_HI   0xFD04u
#define FM6000_SBUS_SPICO_ADDR_LO   0xFD05u
#define FM6000_SBUS_SPICO_DATA_LO   0xFD07u

/* ======================================================================== */
/* L2 forwarding blocks (word indices) — the CPU-punt RX path.               */
/* Bases + field encodings are RE'd facts (fm6000_api_regs_int, cited) cross- */
/* checked against the running-EOS golden capture (eos-golden-2026-07-26-l2)  */
/* + the FocalPoint SDK/diag decode (notes/analysis/phase39-cpu-punt-l2-decode.*/
/* md). A CPU-injected special-delivery frame resolves:                       */
/*   F64 DGLORT -> GLORT_CAM -> GLORT_RAM(DMaskBaseIdx) -> MCAST_DEST_TABLE    */
/*                 (76b physical-port DestMask) -> (L2F membership AND) -> port*/
/* CPU port = physical/logical 0 (PCIe/DMA); CPU GLORT = 0xFF00. All these are */
/* unconfigured on M1; fm6000_l2.c programs the minimal subset.               */
/* ======================================================================== */
#define FM6000_BLK_GLORT        0x0E000u    /* GLORT destination resolution   */
#define FM6000_BLK_L2L          0x30000u    /* L2 MAC-table lookup            */
#define FM6000_BLK_L2AR         0x140000u   /* L2 action resolution           */
#define FM6000_BLK_MOD          0x150000u   /* egress modify                  */
#define FM6000_BLK_L2F          0x180000u   /* L2 filtering (13-stage)        */
#define FM6000_BLK_MCAST_MID    0x240000u   /* MCAST_DEST_TABLE (the DMASK)    */

/* CPU (PCIe/DMA) port + its GLORT — fmSetCpuPort(sw,0)/busType=pcie->0. */
#define FM6000_CPU_PORT             0u          /* physical/logical CPU port   */
#define FM6000_CPU_GLORT            0xFF00u     /* AltaLib GLORT_CPU           */

/* GLORT (base 0x0E000): CAM = 1024-entry TCAM, 1 word/entry: KeyInvert[0:15]
 * (per-bit don't-care), Key[16:31] (16-bit DGLORT match). RAM = 2 words/entry:
 * HashCmd[0:1], DMaskBaseIdx[2:17], DMaskRange[34:40]. */
#define FM6000_GLORT_CAM(i)         (FM6000_BLK_GLORT + 0x000 + (i))
#define FM6000_GLORT_RAM(i, w)      (FM6000_BLK_GLORT + 0x800 + 2u*(i) + (w))
#define FM6000_GLORT_CAM_ENC(key, keyinv)   (((uint32_t)(key) << 16) | ((keyinv) & 0xFFFFu))
#define FM6000_GLORT_RAM_W0(hashcmd, dmaskbaseidx) \
        (((hashcmd) & 0x3u) | (((uint32_t)(dmaskbaseidx) & 0xFFFFu) << 2))

/* MCAST_DEST_TABLE (base 0x240000): 4096 entries x 4 words. DestMask[0:75] is a
 * PHYSICAL-port bitmask (word0=ports0-31, word1=32-63, word2 bits0-11=64-75),
 * MulticastIndex[76:91]=word2 bits>=12. addr = base + 4*groupId + word. Verified
 * live: golden group 4112 = word0 0x00003fff = ports 0-13. */
#define FM6000_MCAST_DEST(gid, w)   (FM6000_BLK_MCAST_MID + 4u*(gid) + (w))
#define FM6000_DEST_WORD(phys_port) ((phys_port) >> 5)          /* which of the 3 words */
#define FM6000_DEST_BIT(phys_port)  (1u << ((phys_port) & 31))  /* bit within that word */

/* L2AR (base 0x140000): action result written when a frame resolves. */
#define FM6000_L2AR_CAM_DMASK(i, w) (FM6000_BLK_L2AR + 0x4000 + 8u*(i) + (w))
#define FM6000_L2AR_ACTION_CPU_CODE(i)  (FM6000_BLK_L2AR + 0x6200 + (i)) /* trap-to-CPU code */
#define FM6000_L2AR_ACTION_DGLORT(i)    (FM6000_BLK_L2AR + 0x6700 + (i))
#define FM6000_L2AR_ACTION_DMASK_IDX(i) (FM6000_BLK_L2AR + 0x6A00 + (i))

/* L2F (base 0x180000): 13-stage filter. TABLE_256/_4K hold 76-bit PHYSICAL-port
 * membership masks (field M[0:75]) indexed by source physical port; the DMASK is
 * ANDed against them. Must admit the CPU port (bit 0). PROFILE gates the stages. */
#define FM6000_L2F_TABLE_4K(i0, w)   (FM6000_BLK_L2F + 0x00000 + 4u*(i0) + (w))
#define FM6000_L2F_TABLE_256(i0, w)  (FM6000_BLK_L2F + 0x20000 + 4u*(i0) + (w))
#define FM6000_L2F_PROFILE(i0)       (FM6000_BLK_L2F + 0x21000 + (i0))
/* Diag "pass-through" profile: TableSelect=7, CmdLookup=1, CmdA[0]=1, CmdB[0]=1. */
#define FM6000_L2F_PROFILE_PASS      (112u | 2048u | 4096u | 65536u)

/* ======================================================================== */
/* Packet-DMA engine block - RAW BYTE offsets from BAR0 (edgenos/FPDMA.md).  */
/* ======================================================================== */
#define FM6000_DMA_BASE         0x5000u

#define FM6000_DMA_COMMAND      0x5004u    /* kick/enable; re-armed each poll  */
#define FM6000_DMA_STATUS       0x5008u    /* polled after command             */
#define FM6000_DMA_COALESCING   0x500Cu
#define FM6000_DMA_RX_BD_BASE_LO 0x5010u
#define FM6000_DMA_RX_BD_BASE_HI 0x5014u
#define FM6000_DMA_RX_BD_END_LO  0x5018u
#define FM6000_DMA_RX_BD_END_HI  0x501Cu
#define FM6000_DMA_TX_BD_BASE_LO 0x5020u
#define FM6000_DMA_TX_BD_BASE_HI 0x5024u
#define FM6000_DMA_TX_BD_END_LO  0x5028u
#define FM6000_DMA_TX_BD_END_HI  0x502Cu
#define FM6000_DMA_IP            0x5030u    /* interrupt pending (W1C)          */
#define FM6000_DMA_IM            0x5034u    /* interrupt mask                   */
/* 0x5038+ : cur_tx/rx_data_ptr, cur_bd_ptr, tx_frame_len, dma_cfg,
 * frame_timeout, stat counters, core_ctrl[2], core_debug[3] (fab_dump). */
#define FM6000_DMA_CFG2          0x505Cu    /* engine cfg; fpdma_init writes 0x30f */
#define FM6000_DMA_CFG2_INIT     0x30Fu
#define FM6000_DMA_UNK68         0x5068u    /* fpdma_init writes 0 here          */
/* PCIe DMA Command codes (datasheet Table 7-2, §7.10.2.1). The command register
 * takes a COMMAND CODE, not a bitmask — the old "0x3 = enable both" was wrong
 * (0x3 = TX_STOP!). STATUS[2:0]=TxState (0=Stopped 1=Running 2=Idle 3=Draining),
 * STATUS[4:3]=RxState. */
#define FM6000_DMA_CMD_TX        0x1u       /* PCI_TX_START — begin TX descriptors */
#define FM6000_DMA_CMD_RX        0x2u       /* PCI_RX_START — begin RX descriptors */
#define FM6000_DMA_CMD_TX_STOP   0x3u       /* PCI_TX_STOP                        */
#define FM6000_DMA_CMD_RX_STOP   0x4u       /* PCI_RX_STOP                        */
#define FM6000_DMA_CMD_TX_POST   0x5u       /* PCI_TX_POST — new TX BDs posted    */
#define FM6000_DMA_CMD_RX_POST   0x6u       /* PCI_RX_POST — new RX BDs posted    */
#define FM6000_DMA_CFG           0x5060u    /* dma_cfg (w0x1418); reset 0x35    */
#define FM6000_DMA_CFG_ENABLE    0x2u       /* bit1: engine enable — golden EOS
                                             * runs 0x37 (=reset 0x35 | 0x2);
                                             * 7150 live-captured 2026-07-26     */
#define FM6000_DMA_IM_RUN        0x3u       /* golden EOS im (tx/rx-done unmasked)*/
#define FM6000_DMA_STATUS_BUSY   0x7u       /* TxState[2:0]; fpdma_reset waits ==0 */
#define FM6000_DMA_STATUS_READY  0x10u      /* RxState bit; golden running=0x12    */

/* ---- Descriptor ring geometry (FPDMA.md "Descriptor ring model") -------- */
#define FM6000_RING_MAX         1024u      /* MAX_RING_SIZE 0x400, power-of-2  */
#define FM6000_DESC_STRIDE      32u        /* fpr_post uses (i << 5)           */
#define FM6000_DESC_HANDOFF     0x09u      /* status byte stored to hand to HW */
#define FM6000_RX_MAX_LEN       0x7FFu     /* rx_skb_reass max                 */

/* Descriptor field byte offsets within the 32-byte stride. */
#define FM6000_DESC_STATUS      0x00u      /* u8 ownership/status              */
#define FM6000_DESC_DONE        0x04u      /* status bit2 = HW completed the BD
                                            * (vendor fpr_reclaim: testb $0x4;
                                            * 0x09 handoff -> 0x0D on done)     */
#define FM6000_DESC_LEN         0x02u      /* u16 length                       */
#define FM6000_DESC_ADDR_LO     0x04u      /* u32 buffer DMA addr lo           */
#define FM6000_DESC_ADDR_HI     0x08u      /* u32 buffer DMA addr hi           */
/* F64 tag field (datasheet §7.11.1.4): follows the 64-bit buffer address, so it
 * lands at offset 0x0C of the 32-byte BD. On CPU->fabric TX the DMA reads the
 * F64 tag from THIS field of the packet's first BD and INSERTS it into the frame
 * at packet offset 12 (it does NOT read a tag from the payload). 64-bit tag = 8
 * bytes; 96-bit = 12. Location/size are selected by PCI_DMA_CFG (0x37 golden). */
#define FM6000_DESC_F64         0x0Cu      /* F64/ISL tag, 8 or 12 bytes       */
#define FM6000_DESC_F64_LEN64   8u         /* 64-bit tag (DMA_CFG size bit=0)  */

/* ---- Address helpers ---------------------------------------------------- */
/* Switch CSR word index -> BAR0 byte offset. */
#define FM6000_CSR(word_idx)    ((uint32_t)(word_idx) << 2)

#endif /* __FM6000_REGS_H__ */
