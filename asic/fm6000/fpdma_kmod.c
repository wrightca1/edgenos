/*
 * fpdma_kmod.c - kmod DMA backing for the FM6000 (independent, IOMMU-less)
 *
 * Userspace side of fm6000dma.ko: opens /dev/fm6000dma, mmaps BAR0 (for the
 * register layer) and the module's coherent low-4GiB DMA pool, and hands out
 * pool sub-allocations through struct fpdma_backing. MSI completion is the
 * chardev fd (poll/read). Mirrors fpdma_vfio.c so fm6000_edged binds either.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "fpdma_kmod.h"
#include "kmod/fm6000dma_uapi.h"

#define KMOD_ALIGN   64u          /* RCB alignment for ring bases */

struct fpdma_kmod {
    int        fd;
    volatile void *bar0;
    size_t     bar0_len;
    void      *pool;              /* mmap of the coherent pool (CPU)   */
    uint64_t   pool_dma;          /* device base address of the pool   */
    size_t     pool_len;
    size_t     pool_used;
};

static size_t align_up(size_t v, size_t a) { return (v + (a - 1)) & ~(a - 1); }

/* Bump allocator over the pinned pool; free is a no-op (pool released at close,
 * matching fpdma.c's alloc-once-at-init usage). */
static void *pool_alloc(void *ctx, size_t len, uint64_t *dma_addr)
{
    struct fpdma_kmod *k = ctx;
    size_t off = align_up(k->pool_used, KMOD_ALIGN);

    if (off + len > k->pool_len) {
        fprintf(stderr, "fpdma_kmod: pool exhausted (need %zu, have %zu)\n",
                len, k->pool_len - off);
        return NULL;
    }
    k->pool_used = off + len;
    *dma_addr = k->pool_dma + off;              /* < 4 GiB (module guaranteed) */
    return (uint8_t *)k->pool + off;
}

static void pool_free(void *ctx, void *va, size_t len, uint64_t dma_addr)
{
    (void)ctx; (void)va; (void)len; (void)dma_addr;
}

int fpdma_kmod_open(struct fpdma_kmod **out)
{
    struct fpdma_kmod *k;
    struct fm6000dma_info info;

    k = calloc(1, sizeof(*k));
    if (!k)
        return -1;
    k->fd = -1;

    k->fd = open("/dev/" FM6000DMA_DEVNAME, O_RDWR | O_CLOEXEC);
    if (k->fd < 0) {
        fprintf(stderr, "fpdma_kmod: open /dev/%s: %s (module loaded?)\n",
                FM6000DMA_DEVNAME, strerror(errno));
        goto fail;
    }
    if (ioctl(k->fd, FM6000DMA_GET_INFO, &info) < 0) {
        perror("fpdma_kmod: GET_INFO");
        goto fail;
    }
    k->bar0_len = info.bar0_len;
    k->pool_len = info.pool_len;
    k->pool_dma = info.pool_dma;

    k->bar0 = mmap(NULL, k->bar0_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                   k->fd, FM6000DMA_OFF_BAR0);
    if (k->bar0 == MAP_FAILED) { perror("fpdma_kmod: mmap BAR0"); k->bar0 = NULL; goto fail; }

    k->pool = mmap(NULL, k->pool_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                   k->fd, FM6000DMA_OFF_POOL);
    if (k->pool == MAP_FAILED) { perror("fpdma_kmod: mmap pool"); k->pool = NULL; goto fail; }

    fprintf(stderr, "fpdma_kmod: up (BAR0 %zuKiB, pool %zuKiB @ dma 0x%llx)\n",
            k->bar0_len / 1024, k->pool_len / 1024,
            (unsigned long long)k->pool_dma);
    *out = k;
    return 0;

fail:
    fpdma_kmod_close(k);
    return -1;
}

void fpdma_kmod_close(struct fpdma_kmod *k)
{
    if (!k)
        return;
    if (k->pool && k->pool != MAP_FAILED)
        munmap(k->pool, k->pool_len);
    if (k->bar0 && k->bar0 != MAP_FAILED)
        munmap((void *)k->bar0, k->bar0_len);
    if (k->fd >= 0)
        close(k->fd);
    free(k);
}

volatile void *fpdma_kmod_bar0(struct fpdma_kmod *k, size_t *size)
{
    if (size)
        *size = k->bar0_len;
    return k->bar0;
}

struct fpdma_backing fpdma_kmod_backing(struct fpdma_kmod *k)
{
    struct fpdma_backing b = { .alloc = pool_alloc, .free = pool_free, .ctx = k };
    return b;
}

int fpdma_kmod_eventfd(struct fpdma_kmod *k)
{
    return k->fd;                    /* poll()able; read() -> u64 count */
}
