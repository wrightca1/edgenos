/*
 * fpdma_vfio.c - VFIO backing for the FM6000 (independent, no vendor kmod)
 *
 * Binds the FM6000 through vfio-pci: maps BAR0, enables bus-master, stands up an
 * IOMMU-mapped DMA pool with IOVAs pinned below 4 GiB (the FM6000 is a 32-bit DMA
 * master, edgenos/FPDMA.md), and wires an MSI eventfd. Implements the allocator
 * contract fpdma.c calls through struct fpdma_backing.
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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <linux/vfio.h>
#include <linux/pci_regs.h>

#include "fpdma_vfio.h"

/* Fixed IOVA window for the DMA pool: base + pool must stay < 4 GiB. */
#define VFIO_POOL_IOVA_BASE   0x10000000ULL   /* 256 MiB                        */
#define VFIO_POOL_ALIGN       64u             /* RCB alignment for ring bases   */

struct fpdma_vfio {
    int             container_fd;
    int             group_fd;
    int             device_fd;

    volatile void  *bar0;
    size_t          bar0_size;
    uint64_t        config_off;    /* VFIO config region file offset            */

    void           *pool_va;       /* host mapping of the DMA pool              */
    uint64_t        pool_iova;     /* device-visible base (< 4 GiB)             */
    size_t          pool_size;
    size_t          pool_used;     /* bump cursor                              */

    int             eventfd;       /* MSI                                       */
};

static size_t align_up(size_t v, size_t a) { return (v + (a - 1)) & ~(a - 1); }

/* ---- allocator (bump; free is a no-op, pool released wholesale at close) --
 * Matches fpdma.c usage: rings + buffers are allocated once and only released at
 * device shutdown, so a bump cursor over a pinned pool is correct and simplest. */
static void *pool_alloc(void *ctx, size_t len, uint64_t *dma_addr)
{
    struct fpdma_vfio *v = ctx;
    size_t off = align_up(v->pool_used, VFIO_POOL_ALIGN);

    if (off + len > v->pool_size) {
        fprintf(stderr, "fpdma_vfio: pool exhausted (need %zu, have %zu)\n",
                len, v->pool_size - off);
        return NULL;
    }
    v->pool_used = off + len;
    *dma_addr = v->pool_iova + off;               /* < 4 GiB by construction    */
    return (uint8_t *)v->pool_va + off;
}

static void pool_free(void *ctx, void *va, size_t len, uint64_t dma_addr)
{
    (void)ctx; (void)va; (void)len; (void)dma_addr;   /* freed at close         */
}

/* ---- vfio-pci config helper: enable bus mastering ---------------------- */
static int enable_bus_master(struct fpdma_vfio *v)
{
    uint16_t cmd;
    if (pread(v->device_fd, &cmd, sizeof(cmd),
              (off_t)(v->config_off + PCI_COMMAND)) != (ssize_t)sizeof(cmd))
        return -1;
    cmd |= PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY;
    if (pwrite(v->device_fd, &cmd, sizeof(cmd),
               (off_t)(v->config_off + PCI_COMMAND)) != (ssize_t)sizeof(cmd))
        return -1;
    return 0;
}

/* ---- iommu group id from sysfs ----------------------------------------- */
static int group_id_for(const char *pci_slot)
{
    char link[256], target[256];
    ssize_t n;
    const char *base;

    snprintf(link, sizeof(link),
             "/sys/bus/pci/devices/%s/iommu_group", pci_slot);
    n = readlink(link, target, sizeof(target) - 1);
    if (n < 0)
        return -1;
    target[n] = '\0';
    base = strrchr(target, '/');
    return atoi(base ? base + 1 : target);
}

/* ---- MSI eventfd ------------------------------------------------------- */
static int setup_msi(struct fpdma_vfio *v)
{
    struct vfio_irq_info info = { .argsz = sizeof(info),
                                  .index = VFIO_PCI_MSI_IRQ_INDEX };
    char buf[sizeof(struct vfio_irq_set) + sizeof(int)];
    struct vfio_irq_set *set = (void *)buf;
    int efd, *pfd;

    if (ioctl(v->device_fd, VFIO_DEVICE_GET_IRQ_INFO, &info) < 0 || info.count == 0) {
        fprintf(stderr, "fpdma_vfio: no MSI (falling back to polled RX)\n");
        return 0;                       /* not fatal: caller can poll           */
    }

    efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (efd < 0)
        return -1;

    memset(buf, 0, sizeof(buf));
    set->argsz = sizeof(buf);
    set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    set->index = VFIO_PCI_MSI_IRQ_INDEX;
    set->start = 0;
    set->count = 1;
    pfd = (int *)set->data;
    *pfd = efd;

    if (ioctl(v->device_fd, VFIO_DEVICE_SET_IRQS, set) < 0) {
        fprintf(stderr, "fpdma_vfio: SET_IRQS(MSI): %s\n", strerror(errno));
        close(efd);
        return -1;
    }
    v->eventfd = efd;
    return 0;
}

/* ---- open / close ------------------------------------------------------ */
int fpdma_vfio_open(struct fpdma_vfio **out, const char *pci_slot,
                    size_t dma_pool_bytes)
{
    struct fpdma_vfio *v;
    struct vfio_group_status gstatus = { .argsz = sizeof(gstatus) };
    struct vfio_region_info bar0 = { .argsz = sizeof(bar0),
                                     .index = VFIO_PCI_BAR0_REGION_INDEX };
    struct vfio_region_info cfg  = { .argsz = sizeof(cfg),
                                     .index = VFIO_PCI_CONFIG_REGION_INDEX };
    struct vfio_iommu_type1_dma_map dmap = { .argsz = sizeof(dmap) };
    char grouppath[64];
    int gid;

    if (VFIO_POOL_IOVA_BASE + dma_pool_bytes > 0x100000000ULL) {
        fprintf(stderr, "fpdma_vfio: pool too big for 32-bit IOVA window\n");
        return -1;
    }

    v = calloc(1, sizeof(*v));
    if (!v)
        return -1;
    v->container_fd = v->group_fd = v->device_fd = v->eventfd = -1;

    gid = group_id_for(pci_slot);
    if (gid < 0) {
        fprintf(stderr, "fpdma_vfio: no iommu_group for %s (IOMMU enabled?)\n", pci_slot);
        goto fail;
    }

    v->container_fd = open("/dev/vfio/vfio", O_RDWR);
    if (v->container_fd < 0) { perror("open /dev/vfio/vfio"); goto fail; }

    snprintf(grouppath, sizeof(grouppath), "/dev/vfio/%d", gid);
    v->group_fd = open(grouppath, O_RDWR);
    if (v->group_fd < 0) { fprintf(stderr, "open %s: %s\n", grouppath, strerror(errno)); goto fail; }

    if (ioctl(v->group_fd, VFIO_GROUP_GET_STATUS, &gstatus) < 0 ||
        !(gstatus.flags & VFIO_GROUP_FLAGS_VIABLE)) {
        fprintf(stderr, "fpdma_vfio: group %d not viable (all devices bound?)\n", gid);
        goto fail;
    }
    if (ioctl(v->group_fd, VFIO_GROUP_SET_CONTAINER, &v->container_fd) < 0) {
        perror("VFIO_GROUP_SET_CONTAINER"); goto fail;
    }
    if (ioctl(v->container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0) {
        perror("VFIO_SET_IOMMU(TYPE1)"); goto fail;
    }

    v->device_fd = ioctl(v->group_fd, VFIO_GROUP_GET_DEVICE_FD, pci_slot);
    if (v->device_fd < 0) { fprintf(stderr, "GET_DEVICE_FD %s: %s\n", pci_slot, strerror(errno)); goto fail; }

    /* BAR0 region -> mmap for register access. */
    if (ioctl(v->device_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0) < 0) {
        perror("GET_REGION_INFO(BAR0)"); goto fail;
    }
    if (!(bar0.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
        fprintf(stderr, "fpdma_vfio: BAR0 not mmap-able\n"); goto fail;
    }
    v->bar0_size = bar0.size;
    v->bar0 = mmap(NULL, bar0.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   v->device_fd, bar0.offset);
    if (v->bar0 == MAP_FAILED) { perror("mmap BAR0"); v->bar0 = NULL; goto fail; }

    /* Config region offset (for bus-master enable). */
    if (ioctl(v->device_fd, VFIO_DEVICE_GET_REGION_INFO, &cfg) < 0) {
        perror("GET_REGION_INFO(CONFIG)"); goto fail;
    }
    v->config_off = cfg.offset;
    if (enable_bus_master(v) < 0) { perror("enable bus master"); goto fail; }

    /* DMA pool: anonymous host memory, IOMMU-mapped at a fixed low IOVA. */
    v->pool_size = align_up(dma_pool_bytes, 4096);
    v->pool_va = mmap(NULL, v->pool_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (v->pool_va == MAP_FAILED) { perror("mmap DMA pool"); v->pool_va = NULL; goto fail; }

    v->pool_iova = VFIO_POOL_IOVA_BASE;
    dmap.vaddr = (uint64_t)(uintptr_t)v->pool_va;
    dmap.iova  = v->pool_iova;
    dmap.size  = v->pool_size;
    dmap.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
    if (ioctl(v->container_fd, VFIO_IOMMU_MAP_DMA, &dmap) < 0) {
        perror("VFIO_IOMMU_MAP_DMA"); goto fail;
    }

    if (setup_msi(v) < 0)
        goto fail;

    fprintf(stderr, "fpdma_vfio: %s up (BAR0 %zuKiB, pool %zuKiB @ iova 0x%llx, msi %s)\n",
            pci_slot, v->bar0_size / 1024, v->pool_size / 1024,
            (unsigned long long)v->pool_iova, v->eventfd >= 0 ? "on" : "polled");
    *out = v;
    return 0;

fail:
    fpdma_vfio_close(v);
    return -1;
}

void fpdma_vfio_close(struct fpdma_vfio *v)
{
    if (!v)
        return;
    if (v->pool_va && v->pool_va != MAP_FAILED) {
        struct vfio_iommu_type1_dma_unmap un = {
            .argsz = sizeof(un), .iova = v->pool_iova, .size = v->pool_size,
        };
        if (v->container_fd >= 0)
            ioctl(v->container_fd, VFIO_IOMMU_UNMAP_DMA, &un);
        munmap(v->pool_va, v->pool_size);
    }
    if (v->bar0 && v->bar0 != MAP_FAILED)
        munmap((void *)v->bar0, v->bar0_size);
    if (v->eventfd >= 0)     close(v->eventfd);
    if (v->device_fd >= 0)   close(v->device_fd);
    if (v->group_fd >= 0)    close(v->group_fd);
    if (v->container_fd >= 0) close(v->container_fd);
    free(v);
}

/* ---- accessors --------------------------------------------------------- */
volatile void *fpdma_vfio_bar0(struct fpdma_vfio *v, size_t *size)
{
    if (size)
        *size = v->bar0_size;
    return v->bar0;
}

struct fpdma_backing fpdma_vfio_backing(struct fpdma_vfio *v)
{
    struct fpdma_backing b = { .alloc = pool_alloc, .free = pool_free, .ctx = v };
    return b;
}

int fpdma_vfio_eventfd(struct fpdma_vfio *v)
{
    return v->eventfd;
}
