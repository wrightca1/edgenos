/*
 * fpdma.h - FM6000 packet-DMA engine (CPU punt/inject) — clean-room
 *
 * The FM6000's on-chip DMA engine at BAR0+0x5000 with dual descriptor rings
 * (TX/RX). Recovered in edgenos/FPDMA.md from Arista's proprietary fpdma.ko
 * (symbols intact). This is the transport EdgeNOS's control plane uses to send
 * and receive frames on the CPU port.
 *
 * DMA memory + interrupts need kernel or VFIO backing: coherent ring memory in
 * the low 4 GB (32-bit master), RCB-aligned, plus MSI for completion. Those are
 * provided through struct fpdma_backing so this ring logic stays portable
 * between a kernel module and a VFIO/uio userspace driver.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __FPDMA_H__
#define __FPDMA_H__

#include <stdint.h>
#include <stddef.h>

#include "fm6000_hw.h"

/* Coherent DMA allocation backing (kernel fpdma.ko / VFIO / hugepage-IOMMU).
 * alloc returns a CPU pointer and sets *dma_addr to the device-visible address
 * (must be < 4 GiB and RCB-aligned for ring bases). */
struct fpdma_backing {
    void *(*alloc)(void *ctx, size_t len, uint64_t *dma_addr);
    void  (*free)(void *ctx, void *va, size_t len, uint64_t dma_addr);
    void  *ctx;
};

/* One descriptor ring (TX or RX). */
struct fpdma_ring {
    volatile uint8_t *desc;      /* ring base (CPU virt), 32*size bytes        */
    uint64_t          desc_dma;  /* ring base (device phys), RCB-aligned       */
    uint32_t          size;      /* #descriptors, power-of-two, <= 1024        */
    uint32_t          mask;      /* size - 1                                   */
    uint32_t          head;      /* producer (SW fills here)                   */
    uint32_t          tail;      /* consumer (HW/reclaim here)                 */
    void            **buf_va;    /* per-slot buffer CPU ptr (side array)       */
    uint64_t         *buf_dma;   /* per-slot buffer device addr                */
    size_t            buf_len;   /* per-slot buffer size (RX fill)             */
};

struct fpdma {
    struct fm6000_dev    *dev;
    struct fpdma_backing  back;
    struct fpdma_ring     tx;
    struct fpdma_ring     rx;
};

/* Allocate + program both rings and enable the engine. tx_sz/rx_sz are
 * power-of-two descriptor counts (<= FM6000_RING_MAX). */
int  fpdma_init(struct fpdma *fp, struct fm6000_dev *dev,
                struct fpdma_backing *back, uint32_t tx_sz, uint32_t rx_sz);
void fpdma_shutdown(struct fpdma *fp);

/* Queue one frame for TX (copies into a ring buffer, hands off to HW). Returns
 * 0, or -1 if the ring is full.
 *
 * `frame` is the L2 payload WITHOUT the F64 tag: DMAC(6) SMAC(6) then the packet
 * body (at offset-12 config the normal VLAN tag is absent — the DMA splices the
 * tag in). The 8- or 12-byte F64 tag is passed separately in `f64`/`f64len` and
 * written into the BD's F64 field; the DMA inserts it into the frame at offset 12
 * on the way to the fabric (datasheet §7.11.1.4). Pass f64=NULL to send raw bytes
 * with a zero tag (normal FTYPE, DGLORT 0) — almost never what you want for punt. */
int  fpdma_tx_f64(struct fpdma *fp, const void *frame, uint16_t len,
                  const void *f64, uint8_t f64len);

/* Back-compat: no explicit tag (zero F64 field). Prefer fpdma_tx_f64 for punt. */
int  fpdma_tx(struct fpdma *fp, const void *frame, uint16_t len);

/* Reap completed TX descriptors (fpr_reclaim). Returns #reclaimed. */
int  fpdma_tx_reclaim(struct fpdma *fp);

/* Poll RX ring for up to `budget` frames; for each, call cb(cb_ctx, data, len).
 * Refills consumed RX descriptors. Returns #frames delivered. (NAPI analog.) */
int  fpdma_rx_poll(struct fpdma *fp, int budget,
                   void (*cb)(void *, const void *, uint16_t), void *cb_ctx);

#endif /* __FPDMA_H__ */
