/*
 * fpdma_vfio.h - VFIO backing for the FM6000 (BAR0 + DMA pool + MSI)
 *
 * Offline-buildable DMA backing that needs no proprietary kernel module. The
 * FM6000 is bound to vfio-pci with the IOMMU on; then:
 *   - register access = mmap of the VFIO BAR0 region (fed to fm6000_hw_attach),
 *   - DMA memory = one IOMMU-mapped pool with IOVAs pinned below 4 GiB (the
 *     FM6000 is a 32-bit DMA master), handed out through struct fpdma_backing,
 *   - RX completion = an MSI eventfd.
 *
 * Host prep (once): load vfio-pci, unbind fpdma/igb, bind the device:
 *   echo 8086 155b > /sys/bus/pci/drivers/vfio-pci/new_id
 *   (or driverctl set-override 0000:02:00.0 vfio-pci)
 * Needs the IOMMU enabled (intel_iommu=on) or vfio noiommu mode.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __FPDMA_VFIO_H__
#define __FPDMA_VFIO_H__

#include <stddef.h>
#include "fpdma.h"

struct fpdma_vfio;   /* opaque */

/* Open the device via VFIO and stand up a DMA pool of dma_pool_bytes.
 * pci_slot e.g. "0000:02:00.0". Returns 0 on success, *out set. */
int  fpdma_vfio_open(struct fpdma_vfio **out, const char *pci_slot,
                     size_t dma_pool_bytes);
void fpdma_vfio_close(struct fpdma_vfio *v);

/* BAR0 mapping for fm6000_hw_attach(). */
volatile void *fpdma_vfio_bar0(struct fpdma_vfio *v, size_t *size);

/* Allocator hooks for fpdma_init(). Backed by the IOMMU-mapped pool. */
struct fpdma_backing fpdma_vfio_backing(struct fpdma_vfio *v);

/* eventfd that fires on device MSI (RX/TX completion); -1 if unset. Poll/read
 * it, then call fpdma_rx_poll(). */
int  fpdma_vfio_eventfd(struct fpdma_vfio *v);

#endif /* __FPDMA_VFIO_H__ */
