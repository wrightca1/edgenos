/*
 * fm6000_hw.h - FM6000 device handle + register access contract
 *
 * The FM6000 analog of asic/bcm56846/bde_interface. Big difference from the
 * Broadcom/PPC path: on x86_64 the switch CSRs are reachable by a plain
 * userspace mmap of the ASIC's own PCIe BAR0 (/sys/bus/pci/.../resource0) — no
 * eieio/sync barriers, no kernel BDE shim needed for register I/O. Only the
 * packet-DMA engine needs the kernel driver (coherent alloc + MSI); that lives
 * in fpdma.{c,h}.
 *
 * Register access primitive (phase7g §a):
 *   csr write  ==  *(volatile u32 *)(bar0 + (word_idx << 2)) = val
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __FM6000_HW_H__
#define __FM6000_HW_H__

#include <stdint.h>
#include <stddef.h>

#include "fm6000_regs.h"

struct fm6000_dev {
    int              unit;         /* logical switch index (0 on a pizza box) */
    int              resource_fd;  /* fd on resourceN sysfs node              */
    volatile uint8_t *bar0;        /* mmap'd BAR0                             */
    size_t           bar0_size;
    char             pci_slot[32]; /* e.g. "0000:02:00.0"                     */
};

/* ---- lifecycle --------------------------------------------------------- */
/* Find the FM6000 (Intel 8086:155b / Fulcrum 1823:1770), mmap BAR0. Returns 0
 * on success. On x86 this needs CAP_SYS_RAWIO (or root) to open resource0. */
int  fm6000_hw_open(struct fm6000_dev *dev);
void fm6000_hw_close(struct fm6000_dev *dev);

/* ---- switch CSR access (word-indexed; the SDK convention) -------------- */
static inline uint32_t fm6000_csr_read(struct fm6000_dev *dev, uint32_t word_idx)
{
    return *(volatile uint32_t *)(dev->bar0 + FM6000_CSR(word_idx));
}

static inline void fm6000_csr_write(struct fm6000_dev *dev,
                                    uint32_t word_idx, uint32_t val)
{
    *(volatile uint32_t *)(dev->bar0 + FM6000_CSR(word_idx)) = val;
}

/* Poll a CSR until (read & mask) == want, or timeout. Returns 0 on match,
 * -1 on timeout. Used by the BIST/SPICO handshakes. */
int fm6000_csr_poll(struct fm6000_dev *dev, uint32_t word_idx,
                    uint32_t mask, uint32_t want, unsigned timeout_us);

/* ---- raw DMA-block access (byte-offset; fpdma convention) -------------- */
static inline uint32_t fm6000_dma_read(struct fm6000_dev *dev, uint32_t byte_off)
{
    return *(volatile uint32_t *)(dev->bar0 + byte_off);
}

static inline void fm6000_dma_write(struct fm6000_dev *dev,
                                    uint32_t byte_off, uint32_t val)
{
    *(volatile uint32_t *)(dev->bar0 + byte_off) = val;
}

/* ---- timing ------------------------------------------------------------ */
void fm6000_delay_us(unsigned usec);   /* fmDelay analog                     */

#endif /* __FM6000_HW_H__ */
