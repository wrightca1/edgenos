/*
 * fm6000dma_uapi.h - userspace<->kernel contract for the FM6000 DMA kmod.
 *
 * Shared by the kernel module (fm6000dma.c) and the userspace backing shim
 * (asic/fm6000/fpdma_kmod.c). Kept minimal and stable.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __FM6000DMA_UAPI_H__
#define __FM6000DMA_UAPI_H__

#include <linux/types.h>
#include <linux/ioctl.h>

#define FM6000DMA_DEVNAME   "fm6000dma"        /* /dev/fm6000dma */
#define FM6000DMA_IOC_MAGIC 0xF6

/* Layout the module exposes: BAR0 length + the coherent DMA pool it allocated.
 * pool_dma is the *device* address of the pool base (< 4 GiB, 32-bit master);
 * userspace derives each buffer's dma_addr as pool_dma + (vaddr - pool_base). */
struct fm6000dma_info {
	__u64 bar0_len;
	__u64 pool_len;
	__u64 pool_dma;
};

#define FM6000DMA_GET_INFO  _IOR(FM6000DMA_IOC_MAGIC, 1, struct fm6000dma_info)

/* mmap() byte-offset selectors (mmap offset chooses what gets mapped):
 *   0                  -> BAR0 registers (noncached)
 *   FM6000DMA_OFF_POOL -> the coherent DMA pool (cacheable-coherent) */
#define FM6000DMA_OFF_BAR0  0x0UL
#define FM6000DMA_OFF_POOL  0x10000000UL       /* 256 MiB — beyond any BAR0 */

/* read() returns a __u64 event count and clears it (eventfd-compatible, so the
 * daemon's `read(fd,&u64,8)` works unchanged); poll() reports POLLIN when >0. */

#endif /* __FM6000DMA_UAPI_H__ */
