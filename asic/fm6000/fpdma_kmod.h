/*
 * fpdma_kmod.h - kmod backing for the FM6000 (BAR0 + DMA pool + MSI)
 *
 * The IOMMU-less counterpart to fpdma_vfio: talks to the fm6000dma.ko char
 * device (/dev/fm6000dma) instead of vfio-pci. Same API shape, so fm6000_edged
 * can bind either. Use this on boxes with no usable IOMMU (e.g. the 7150's AMD
 * RS780 — see phase13 live probe).
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __FPDMA_KMOD_H__
#define __FPDMA_KMOD_H__

#include <stddef.h>
#include "fpdma.h"

struct fpdma_kmod;   /* opaque */

/* Open /dev/fm6000dma, map BAR0 + the module's coherent DMA pool. Returns 0. */
int  fpdma_kmod_open(struct fpdma_kmod **out);
void fpdma_kmod_close(struct fpdma_kmod *k);

/* BAR0 mapping for fm6000_hw_attach(). */
volatile void *fpdma_kmod_bar0(struct fpdma_kmod *k, size_t *size);

/* Allocator hooks for fpdma_init() (bump over the pinned coherent pool). */
struct fpdma_backing fpdma_kmod_backing(struct fpdma_kmod *k);

/* MSI fd (the /dev/fm6000dma fd itself: poll()able, read() yields a u64 count,
 * eventfd-compatible). */
int  fpdma_kmod_eventfd(struct fpdma_kmod *k);

#endif /* __FPDMA_KMOD_H__ */
