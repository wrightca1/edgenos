/*
 * fpdma.c - FM6000 packet-DMA ring engine (clean-room)
 *
 * Ring setup + descriptor mechanics recovered in edgenos/FPDMA.md (fpdma_init /
 * fpr_post / fpr_reclaim / fpdma_napi_poll). Register block at BAR0+0x5000.
 *
 * Descriptor (32-byte stride, fpr_post):
 *   [0x00] u8  status   (store 0x09 to hand off to HW; RX OWN/SOP/EOP/err bits
 *                        are TODO(live-trace) — see FPDMA.md "needs a live read")
 *   [0x02] u16 length
 *   [0x04] u32 buf addr lo   [0x08] u32 buf addr hi
 *   [0x0C..0x1F] reserved/pad
 * Handoff = fill [2..12], memory barrier, then store status byte; (re)kick the
 * engine via COMMAND. No per-descriptor doorbell.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fpdma.h"

/* x86: an sfence orders the descriptor-body stores before the ownership store.
 * (The PPC Broadcom path needed eieio/sync; here a compiler+store barrier does.) */
#define FPDMA_WMB() __asm__ __volatile__("sfence" ::: "memory")

static inline volatile uint8_t *desc_ptr(struct fpdma_ring *r, uint32_t i)
{
    return r->desc + ((size_t)(i & r->mask) * FM6000_DESC_STRIDE);
}

static void desc_write(volatile uint8_t *d, uint8_t status, uint16_t len, uint64_t addr)
{
    *(volatile uint16_t *)(d + FM6000_DESC_LEN)     = len;
    *(volatile uint32_t *)(d + FM6000_DESC_ADDR_LO) = (uint32_t)(addr & 0xFFFFFFFFu);
    *(volatile uint32_t *)(d + FM6000_DESC_ADDR_HI) = (uint32_t)(addr >> 32);
    FPDMA_WMB();
    *(volatile uint8_t *)(d + FM6000_DESC_STATUS)   = status;   /* hand off last */
}

/* ---- ring alloc -------------------------------------------------------- */
static int ring_alloc(struct fpdma *fp, struct fpdma_ring *r,
                      uint32_t size, size_t buf_len)
{
    uint64_t dma = 0;

    if (size == 0 || size > FM6000_RING_MAX || (size & (size - 1)))
        return -1;                       /* must be power-of-two, <= 1024      */

    memset(r, 0, sizeof(*r));
    r->size = size;
    r->mask = size - 1;
    r->buf_len = buf_len;

    r->desc = fp->back.alloc(fp->back.ctx,
                             (size_t)size * FM6000_DESC_STRIDE, &dma);
    if (!r->desc)
        return -1;
    /* Base must be RCB-aligned and in the low 4 GiB (32-bit master). */
    if ((dma & (64 - 1)) || (dma >> 32)) {
        fprintf(stderr, "fpdma: ring base 0x%llx not RCB-aligned/<4GiB\n",
                (unsigned long long)dma);
        return -1;
    }
    r->desc_dma = dma;
    memset((void *)r->desc, 0, (size_t)size * FM6000_DESC_STRIDE);

    r->buf_va  = calloc(size, sizeof(*r->buf_va));
    r->buf_dma = calloc(size, sizeof(*r->buf_dma));
    if (!r->buf_va || !r->buf_dma)
        return -1;

    /* RX rings need a buffer per descriptor up front. */
    if (buf_len) {
        for (uint32_t i = 0; i < size; i++) {
            uint64_t bdma = 0;
            r->buf_va[i] = fp->back.alloc(fp->back.ctx, buf_len, &bdma);
            if (!r->buf_va[i] || (bdma >> 32))
                return -1;
            r->buf_dma[i] = bdma;
            desc_write(desc_ptr(r, i), FM6000_DESC_HANDOFF,
                       (uint16_t)buf_len, bdma);   /* own -> HW, ready to fill  */
        }
        r->head = size;                  /* all RX descriptors handed to HW    */
    }
    return 0;
}

static void ring_free(struct fpdma *fp, struct fpdma_ring *r)
{
    if (r->buf_len && r->buf_va) {
        for (uint32_t i = 0; i < r->size; i++)
            if (r->buf_va[i])
                fp->back.free(fp->back.ctx, r->buf_va[i], r->buf_len, r->buf_dma[i]);
    }
    if (r->desc)
        fp->back.free(fp->back.ctx, (void *)r->desc,
                      (size_t)r->size * FM6000_DESC_STRIDE, r->desc_dma);
    free(r->buf_va);
    free(r->buf_dma);
    memset(r, 0, sizeof(*r));
}

/* Brief settle after a COMMAND write (vendor fpdma_init pauses between the TX and
 * RX enables). The running STATUS legitimately keeps bit1 set (golden EOS reads
 * 0x12), so we don't wait for it to clear — just let the engine latch. */
static void dma_settle(struct fm6000_dev *dev)
{
    int i;
    for (i = 0; i < 50; i++) {
        uint32_t st = fm6000_dma_read(dev, FM6000_DMA_STATUS);
        if (st == 0xFFFFFFFFu || (st & FM6000_DMA_STATUS_READY))
            break;
        usleep(10);
    }
}

/* ---- program the 0x5000 register block --------------------------------- */
static void program_rings(struct fpdma *fp)
{
    struct fm6000_dev *dev = fp->dev;
    uint64_t rx_base = fp->rx.desc_dma;
    uint64_t rx_end  = rx_base + (uint64_t)fp->rx.size * FM6000_DESC_STRIDE;
    uint64_t tx_base = fp->tx.desc_dma;
    uint64_t tx_end  = tx_base + (uint64_t)fp->tx.size * FM6000_DESC_STRIDE;

    /* Exact fpdma_init sequence, recovered from the vendor fpdma.ko disasm
     * (2026-07-26). The earlier version wrote only rings + a single COMMAND=0x3;
     * the engine never advanced a TX BD because it was missing DMA_CFG2=0x30f and
     * the split TX-then-RX enable. Order matters: config, then TX ring + COMMAND=1
     * (enable/kick TX) + status-settle, then RX ring + COMMAND=2 (RX), then unmask. */
    fm6000_dma_write(dev, FM6000_DMA_IP,   0xFFFFFFFFu);        /* clear pending  */
    fm6000_dma_write(dev, FM6000_DMA_UNK68, 0);
    fm6000_dma_write(dev, FM6000_DMA_CFG,  0x37u);              /* dma_cfg enable */
    fm6000_dma_write(dev, FM6000_DMA_CFG2, FM6000_DMA_CFG2_INIT);/* 0x30f — was missing */

    fm6000_dma_write(dev, FM6000_DMA_TX_BD_BASE_LO, (uint32_t)tx_base);
    fm6000_dma_write(dev, FM6000_DMA_TX_BD_BASE_HI, (uint32_t)(tx_base >> 32));
    fm6000_dma_write(dev, FM6000_DMA_TX_BD_END_LO,  (uint32_t)tx_end);
    fm6000_dma_write(dev, FM6000_DMA_TX_BD_END_HI,  (uint32_t)(tx_end >> 32));
    fm6000_dma_write(dev, FM6000_DMA_COMMAND, FM6000_DMA_CMD_TX);   /* enable TX  */
    dma_settle(dev);

    fm6000_dma_write(dev, FM6000_DMA_RX_BD_BASE_LO, (uint32_t)rx_base);
    fm6000_dma_write(dev, FM6000_DMA_RX_BD_BASE_HI, (uint32_t)(rx_base >> 32));
    fm6000_dma_write(dev, FM6000_DMA_RX_BD_END_LO,  (uint32_t)rx_end);
    fm6000_dma_write(dev, FM6000_DMA_RX_BD_END_HI,  (uint32_t)(rx_end >> 32));
    fm6000_dma_write(dev, FM6000_DMA_COMMAND, FM6000_DMA_CMD_RX);   /* enable RX  */
    dma_settle(dev);

    fm6000_dma_write(dev, FM6000_DMA_IM, FM6000_DMA_IM_RUN);
}

/* ---- public API -------------------------------------------------------- */
int fpdma_init(struct fpdma *fp, struct fm6000_dev *dev,
               struct fpdma_backing *back, uint32_t tx_sz, uint32_t rx_sz)
{
    memset(fp, 0, sizeof(*fp));
    fp->dev  = dev;
    fp->back = *back;

    if (ring_alloc(fp, &fp->tx, tx_sz, 0) < 0) {
        fprintf(stderr, "fpdma: TX ring alloc failed\n");
        return -1;
    }
    if (ring_alloc(fp, &fp->rx, rx_sz, 2048) < 0) {   /* jumbo-safe RX buffers  */
        fprintf(stderr, "fpdma: RX ring alloc failed\n");
        ring_free(fp, &fp->tx);
        return -1;
    }
    program_rings(fp);
    fprintf(stderr, "fpdma: rings up (tx=%u rx=%u)\n", tx_sz, rx_sz);
    return 0;
}

void fpdma_shutdown(struct fpdma *fp)
{
    if (fp->dev) {
        fm6000_dma_write(fp->dev, FM6000_DMA_COMMAND, 0);      /* stop engine    */
        fm6000_dma_write(fp->dev, FM6000_DMA_IM, 0xFFFFFFFFu);
    }
    ring_free(fp, &fp->tx);
    ring_free(fp, &fp->rx);
}

int fpdma_tx(struct fpdma *fp, const void *frame, uint16_t len)
{
    struct fpdma_ring *r = &fp->tx;
    uint32_t slot = r->head & r->mask;
    uint64_t bdma = 0;
    void *buf;

    if (((r->head + 1) & r->mask) == (r->tail & r->mask))
        return -1;                       /* ring full                          */
    if (len > 2048)
        return -1;

    /* One bounce buffer per slot; alloc lazily, reuse thereafter. */
    if (!r->buf_va[slot]) {
        buf = fp->back.alloc(fp->back.ctx, 2048, &bdma);
        if (!buf || (bdma >> 32))
            return -1;
        r->buf_va[slot]  = buf;
        r->buf_dma[slot] = bdma;
    }
    memcpy(r->buf_va[slot], frame, len);

    /* NOTE: caller is responsible for having prepended the F64/ISL CPU tag
     * (DGLORT/SGLORT/FTYPE/SWPRI/VLAN at L2 offset 12) — see fpdma_f64 in
     * FPDMA.md. The raw ring just DMAs bytes. */
    desc_write(desc_ptr(r, slot), FM6000_DESC_HANDOFF, len, r->buf_dma[slot]);
    r->head++;

    fm6000_dma_write(fp->dev, FM6000_DMA_COMMAND, FM6000_DMA_CMD_ENABLE);        /* kick           */
    return 0;
}

int fpdma_tx_reclaim(struct fpdma *fp)
{
    struct fpdma_ring *r = &fp->tx;
    int n = 0;

    /* HW sets status bit2 (FM6000_DESC_DONE) on completion — confirmed from the
     * vendor fpr_reclaim disasm (`testb $0x4,(desc)`). Reclaim while done. */
    while (r->tail != r->head) {
        volatile uint8_t *d = desc_ptr(r, r->tail);
        if (!(*(volatile uint8_t *)(d + FM6000_DESC_STATUS) & FM6000_DESC_DONE))
            break;                       /* still owned by HW                  */
        r->tail++;
        n++;
    }
    return n;
}

int fpdma_rx_poll(struct fpdma *fp, int budget,
                  void (*cb)(void *, const void *, uint16_t), void *cb_ctx)
{
    struct fpdma_ring *r = &fp->rx;
    int n = 0;

    /* Ack interrupts at the top of the poll (W1C), matching fpdma_napi_poll. */
    fm6000_dma_write(fp->dev, FM6000_DMA_IP, fm6000_dma_read(fp->dev, FM6000_DMA_IP));

    while (n < budget) {
        volatile uint8_t *d = desc_ptr(r, r->tail);
        uint8_t  status = *(volatile uint8_t *)(d + FM6000_DESC_STATUS);
        uint16_t len;

        /* HW sets status bit2 (FM6000_DESC_DONE) when it fills a descriptor
         * (same done-bit as TX, per fpr_reclaim). TODO(live-trace): SOP/EOP/error
         * bits + multi-descriptor reassembly (rx_skb_reass, max 0x7ff). */
        if (!(status & FM6000_DESC_DONE))
            break;                       /* still owned by HW / empty          */

        len = *(volatile uint16_t *)(d + FM6000_DESC_LEN);
        if (len > FM6000_RX_MAX_LEN)
            len = FM6000_RX_MAX_LEN;
        if (cb)
            cb(cb_ctx, r->buf_va[r->tail & r->mask], len);

        /* Recycle: hand the descriptor back to HW. */
        desc_write(d, FM6000_DESC_HANDOFF, (uint16_t)r->buf_len,
                   r->buf_dma[r->tail & r->mask]);
        r->tail++;
        n++;
    }
    if (n)
        fm6000_dma_write(fp->dev, FM6000_DMA_COMMAND, FM6000_DMA_CMD_ENABLE);   /* re-arm RX      */
    return n;
}
