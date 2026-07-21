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

/* ---- MGMT2: soft-reset / PLL (phase7g §b BIST skeleton) ----------------- */
#define FM6000_REG_SOFT_RESET   0x1C03Au    /* +fmDelay 640us                 */
#define FM6000_REG_PLL_CTRL     0x1C03Cu    /* +fmDelay 1640us                */

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

/* ---- Descriptor ring geometry (FPDMA.md "Descriptor ring model") -------- */
#define FM6000_RING_MAX         1024u      /* MAX_RING_SIZE 0x400, power-of-2  */
#define FM6000_DESC_STRIDE      32u        /* fpr_post uses (i << 5)           */
#define FM6000_DESC_HANDOFF     0x09u      /* status byte stored to hand to HW */
#define FM6000_RX_MAX_LEN       0x7FFu     /* rx_skb_reass max                 */

/* Descriptor field byte offsets within the 32-byte stride. */
#define FM6000_DESC_STATUS      0x00u      /* u8 ownership/status              */
#define FM6000_DESC_LEN         0x02u      /* u16 length                       */
#define FM6000_DESC_ADDR_LO     0x04u      /* u32 buffer DMA addr lo           */
#define FM6000_DESC_ADDR_HI     0x08u      /* u32 buffer DMA addr hi           */

/* ---- Address helpers ---------------------------------------------------- */
/* Switch CSR word index -> BAR0 byte offset. */
#define FM6000_CSR(word_idx)    ((uint32_t)(word_idx) << 2)

#endif /* __FM6000_REGS_H__ */
