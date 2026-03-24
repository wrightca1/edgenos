/*
 * bde_interface.c - OpenMDK ASIC interface for switchd
 *
 * Initializes BCM56846 via OpenMDK CDK/BMD.
 *
 * Register access architecture (from AS5610-52X RE):
 *
 *   USERSPACE (switchd)
 *     |
 *     +-- CDK_DEV_READ32/WRITE32 (via dv.read32/write32 function pointers)
 *           |
 *   KERNEL (linux-kernel-bde.ko)
 *     |
 *     +-- ioread32/iowrite32 on BAR0 (includes PPC MMIO barriers)
 *     +-- DMA pool: dma_alloc_coherent (for packet I/O + S-Channel)
 *
 * Why we need the kernel BDE module (not raw /dev/mem):
 *   1. PPC requires eieio/sync barriers for MMIO - ioread32 includes these
 *   2. CMICm registers above 0x10000 need proper PIO access
 *   3. DMA coherent memory must be allocated by kernel DMA API
 *   4. IRQ handling for DMA completion
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
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>

#include "switchd.h"

/* CDK headers */
#include <cdk_config.h>
#include <cdk/cdk_device.h>
#include <cdk/cdk_error.h>
#include <cdk/chip/bcm56840_a0_defs.h>

/* BMD headers */
#include <bmd_config.h>
#include <bmd/bmd.h>
#include <bmd/bmd_dma.h>
#include <bmd/bmd_phy_ctrl.h>

/* PHY headers */
#include <phy_config.h>
#include <phy/phy_drvlist.h>

/* BCM56846 on AS5610-52X */
#define BCM56846_VENDOR_ID  0x14e4
#define BCM56846_DEVICE_ID  0xb846
#define BCM56846_REVISION   0x02
#define BCM56846_BAR0_SIZE  (256 * 1024)

/* BDE kernel module ioctl definitions (must match linux-kernel-bde.c) */
#define BDE_IOC_MAGIC  'b'

struct bde_dev_info {
    unsigned int vendor_id;
    unsigned int device_id;
    unsigned int revision;
    unsigned long base_addr;
    unsigned int base_size;
    unsigned long dma_addr;
    unsigned int dma_size;
};

struct bde_reg_io {
    unsigned int dev;
    unsigned int addr;
    unsigned int val;
};

struct bde_dma_info {
    unsigned int dev;
    unsigned int size;
    unsigned long phys_addr;
};

/*
 * IOCTL command numbers.
 * CRITICAL: PPC has _IOC_READ=1, _IOC_WRITE=2 (reversed from x86!)
 * The cross-compiler's _IOW macro may use x86 encoding (direction=1)
 * which doesn't match the kernel's PPC encoding (direction=2).
 * We hardcode the PPC values to avoid this mismatch.
 *
 * _IOC(dir, type, nr, size) = (dir<<30) | (size<<16) | (type<<8) | nr
 * PPC: _IOR = (1<<30), _IOW = (2<<30), _IOWR = (3<<30)
 */
#define _PPC_IOC(d,t,n,s) (((d)<<30)|((s)<<16)|((t)<<8)|(n))
#define BDE_IOC_DEV_INFO     _PPC_IOC(1, BDE_IOC_MAGIC, 0, sizeof(struct bde_dev_info))
#define BDE_IOC_REG_READ     _PPC_IOC(3, BDE_IOC_MAGIC, 1, sizeof(struct bde_reg_io))
/* PPC _IOW: direction=2 (NOT 1 like x86) */
#define BDE_IOC_REG_WRITE    _PPC_IOC(2, BDE_IOC_MAGIC, 2, sizeof(struct bde_reg_io))
#define BDE_IOC_DMA_ALLOC    _PPC_IOC(3, BDE_IOC_MAGIC, 3, sizeof(struct bde_dma_info))
#define BDE_IOC_GET_NUM_DEVS _PPC_IOC(1, BDE_IOC_MAGIC, 4, sizeof(int))

/* BMD/PHY sleep function */
int _usleep(uint32_t usecs) { return usleep(usecs); }

static int bde_fd = -1;
static volatile uint32_t *bar0_map;    /* mmap'd BAR0 for fast XLPORT/MIIM access */
static void *dma_map;                   /* mmap'd DMA pool */
static unsigned long dma_phys;          /* DMA pool physical address */
static unsigned int dma_size;           /* DMA pool size */
static unsigned long bar0_phys;         /* BAR0 physical address */

/*
 * Register read via BDE kernel module ioctl.
 * Uses ioread32() in kernel which has proper PPC MMIO barriers.
 * Required for CMICm registers (S-Channel, DMA, IRQ at 0x31000+).
 */
static int bde_read32(void *dvc, uint32_t addr, uint32_t *data)
{
    struct bde_reg_io rio;

    /* All register access goes through BDE ioctl.
     * Direct mmap writes are silently dropped on P2020 PPC. */
    rio.dev = 0;
    rio.addr = addr;
    rio.val = 0;

    if (ioctl(bde_fd, BDE_IOC_REG_READ, &rio) < 0) {
        syslog(LOG_ERR, "BDE read32 at 0x%x failed: %s", addr, strerror(errno));
        *data = 0;
        return -1;
    }

    *data = rio.val;
    return 0;
}

/*
 * Register write via BDE kernel module ioctl.
 * Uses iowrite32() in kernel with proper PPC MMIO barriers.
 */
static int bde_write32(void *dvc, uint32_t addr, uint32_t data)
{
    struct bde_reg_io rio;

    /* All writes via BDE ioctl (mmap writes silently dropped on PPC) */
    rio.dev = 0;
    rio.addr = addr;
    rio.val = data;

    if (ioctl(bde_fd, BDE_IOC_REG_WRITE, &rio) < 0) {
        syslog(LOG_ERR, "BDE write32 at 0x%x = 0x%x failed: %s",
               addr, data, strerror(errno));
        return -1;
    }

    return 0;
}

int bde_open(void)
{
    struct bde_dev_info info;
    struct bde_dma_info dinfo;

    /* Open BDE kernel module device */
    bde_fd = open("/dev/linux-kernel-bde", O_RDWR | O_SYNC);
    if (bde_fd < 0) {
        syslog(LOG_ERR, "Cannot open /dev/linux-kernel-bde: %s", strerror(errno));
        return -1;
    }

    /* Get device info */
    memset(&info, 0, sizeof(info));
    if (ioctl(bde_fd, BDE_IOC_DEV_INFO, &info) < 0) {
        syslog(LOG_ERR, "BDE_IOC_DEV_INFO failed: %s", strerror(errno));
        close(bde_fd);
        bde_fd = -1;
        return -1;
    }

    bar0_phys = info.base_addr;
    syslog(LOG_INFO, "BDE: BCM%04x rev %02x at phys 0x%lx, BAR0 %u bytes",
           info.device_id, info.revision, bar0_phys, info.base_size);

    /*
     * mmap BAR0 via /dev/mem for direct register access.
     * This is the proven path (same as Cumulus switchd).
     * The BDE fd mmap doesn't work reliably on PPC due to
     * endianness issues with the kernel's ioremap mapping.
     * Direct /dev/mem mmap with PPC big-endian pointer access
     * gives correct register values.
     */
    {
        int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (mem_fd >= 0) {
            bar0_map = mmap(NULL, info.base_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, mem_fd, bar0_phys);
            if (bar0_map == MAP_FAILED) {
                syslog(LOG_WARNING, "BDE: BAR0 /dev/mem mmap failed");
                bar0_map = NULL;
            } else {
                syslog(LOG_INFO, "BDE: BAR0 mapped via /dev/mem at %p", bar0_map);
            }
            close(mem_fd);  /* fd can be closed after mmap */
        } else {
            syslog(LOG_WARNING, "BDE: Cannot open /dev/mem");
            bar0_map = NULL;
        }
    }

    /* Get DMA pool info */
    memset(&dinfo, 0, sizeof(dinfo));
    dinfo.dev = 0;
    if (ioctl(bde_fd, BDE_IOC_DMA_ALLOC, &dinfo) < 0) {
        syslog(LOG_WARNING, "BDE: No DMA pool available");
        dma_map = NULL;
        dma_phys = 0;
        dma_size = 0;
    } else {
        dma_phys = dinfo.phys_addr;
        dma_size = dinfo.size;

        /*
         * mmap DMA pool for userspace access.
         * Use offset=0: dma_mmap_coherent() in the kernel requires
         * vm_pgoff=0. The kernel BDE mmap handler recognizes offset=0
         * as a DMA mapping request.
         */
        dma_map = mmap(NULL, dma_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, bde_fd, 0);
        if (dma_map == MAP_FAILED) {
            syslog(LOG_WARNING, "BDE: DMA mmap(offset=0) failed: %s",
                   strerror(errno));
            dma_map = NULL;
        } else {
            syslog(LOG_INFO, "BDE: DMA pool %u MB at phys 0x%lx mapped at %p",
                   dma_size / (1024 * 1024), dma_phys, dma_map);
        }
    }

    return 0;
}

void bde_close(void)
{
    if (bar0_map && bar0_map != MAP_FAILED)
        munmap((void *)bar0_map, BCM56846_BAR0_SIZE);
    if (dma_map && dma_map != MAP_FAILED)
        munmap(dma_map, dma_size);
    if (bde_fd >= 0)
        close(bde_fd);

    bar0_map = NULL;
    dma_map = NULL;
    bde_fd = -1;
}

/*
 * BMD DMA allocation.
 * Allocates from the DMA pool that was allocated by the BDE kernel
 * module via dma_alloc_coherent(). Returns both virtual (for CPU)
 * and physical (for ASIC DMA) addresses.
 */
static uint32_t dma_offset;  /* Simple bump allocator */
static uint32_t dma_high_water;  /* Track peak usage for debugging */

void *_bde_dma_alloc(void *dvc, size_t size, dma_addr_t *baddr)
{
    uint32_t aligned_size;

    if (!dma_map || !dma_size) {
        syslog(LOG_ERR, "DMA alloc: no DMA pool available");
        return NULL;
    }

    /* Align to 64 bytes (cache line) */
    aligned_size = (size + 63) & ~63;

    if (dma_offset + aligned_size > dma_size) {
        syslog(LOG_ERR, "DMA alloc: pool exhausted (%u + %u > %u)",
               dma_offset, aligned_size, dma_size);
        return NULL;
    }

    void *vaddr = (uint8_t *)dma_map + dma_offset;
    *baddr = dma_phys + dma_offset;
    dma_offset += aligned_size;

    if (dma_offset > dma_high_water)
        dma_high_water = dma_offset;

    memset(vaddr, 0, size);
    return vaddr;
}

void _bde_dma_free(void *dvc, size_t size, void *laddr, dma_addr_t baddr)
{
    (void)dvc;
    (void)size;
    (void)laddr;
    (void)baddr;
    /* Bump allocator - free is handled by bde_dma_pool_reset() */
}

/*
 * Reset the DMA bump allocator.
 *
 * BMD init (bmd_reset, bmd_init, bmd_switching_init) uses DMA for
 * S-Channel table writes. These are temporary allocations that BMD
 * "frees" via _bde_dma_free after each operation. With a bump
 * allocator, frees are no-ops so the pool fills up during init.
 *
 * Call this after BMD init completes but before packet I/O starts.
 * The init-time DMA buffers are no longer referenced by hardware
 * at that point, so resetting the offset is safe.
 */
void bde_dma_pool_reset(void)
{
    syslog(LOG_INFO, "DMA pool reset: was %u bytes used (high water %u), pool %u bytes",
           dma_offset, dma_high_water, dma_size);
    dma_offset = 0;
}

int cdk_init(void)
{
    cdk_dev_id_t dev_id;
    cdk_dev_vectors_t dv;
    int unit;

    if (bde_fd < 0) {
        syslog(LOG_ERR, "CDK: BDE not open");
        return -1;
    }

    memset(&dev_id, 0, sizeof(dev_id));
    dev_id.vendor_id = BCM56846_VENDOR_ID;
    dev_id.device_id = BCM56846_DEVICE_ID;
    dev_id.revision = BCM56846_REVISION;

    memset(&dv, 0, sizeof(dv));

    /*
     * Provide BOTH base_addr and read32/write32 function pointers.
     *
     * base_addr: used by CDK when CDK_CONFIG_MEMMAP_DIRECT=1
     *            (fast direct pointer access for XLPORT/MIIM)
     *
     * read32/write32: used when CDK_CONFIG_MEMMAP_DIRECT=0
     *                 (goes through BDE ioctl for proper PPC barriers)
     *
     * For safety on PPC, we always provide function pointers.
     * The functions use fast-path mmap for 0x000-0x10000 and
     * BDE ioctl for 0x10000+ (CMICm PIO indirect registers).
     */
    /*
     * CRITICAL: do NOT set dv.base_addr!
     * CDK's cdk_dev_read32() checks base_addr FIRST and uses direct
     * pointer access if non-NULL. On P2020 PPC, /dev/mem mmap reads
     * work but WRITES are silently dropped (never reach PCIe device).
     * By setting base_addr=NULL, CDK falls through to dv.read32/write32
     * which go through the BDE kernel module's ioread/iowrite.
     */
    dv.base_addr = NULL;
    dv.read32 = bde_read32;
    dv.write32 = bde_write32;

    unit = cdk_dev_create(&dev_id, &dv, CDK_DEV_MBUS_PCI);
    if (unit < 0) {
        syslog(LOG_ERR, "CDK: cdk_dev_create failed: %s (%d)",
               CDK_ERRMSG(unit), unit);
        return -1;
    }

    syslog(LOG_INFO, "CDK: unit %d created (BCM%04x rev %02x)",
           unit, dev_id.device_id, dev_id.revision);

    /* AS5610-52X uses 156.25 MHz LCPLL reference clock */
    /* AS5610-52X: 156.25 MHz LCPLL reference clock */
    CDK_CHIP_CONFIG_SET(unit, DCFG_LCPLL_156);

    /* Initialize PHY probe */
#if BMD_CONFIG_INCLUDE_PHY == 1
    bmd_phy_probe_init(bmd_phy_probe_default, bmd_phy_drv_list);
#endif

    /* Attach BMD driver */
    int rv = bmd_attach(unit);
    if (rv < 0) {
        syslog(LOG_ERR, "CDK: bmd_attach failed: %d", rv);
        return -1;
    }

    switchd.unit = unit;
    syslog(LOG_INFO, "CDK: BMD attached to unit %d", unit);
    return 0;
}

int bmd_init_all(void)
{
    int rv;

    syslog(LOG_INFO, "BMD: resetting ASIC on unit %d", switchd.unit);

    /*
     * bmd_reset() does:
     *   1. ASIC soft reset via CMIC_CONFIG
     *   2. CPS (Chip Port System) table initialization
     *   3. SBUS ring map configuration
     *   4. PLL lock verification
     *   5. Memory (TCAM, buffer) initialization
     */
    rv = bmd_reset(switchd.unit);
    if (rv < 0) {
        syslog(LOG_ERR, "BMD: bmd_reset failed: %d", rv);
        return -1;
    }
    syslog(LOG_INFO, "BMD: ASIC reset complete");

    /*
     * bmd_init() does:
     *   1. Port mapping (physical <-> logical <-> MMU)
     *   2. For each xlport: Warpcore SerDes init (3 stages):
     *      Stage 0: Stop PLL, download firmware v0x0101, start PLL
     *      Stage 1: Check firmware CRC
     *      Stage 2: Clock compensation, 64/66 encoding, CL73 BAM
     *   3. TDM calendar programming
     *   4. MMU buffer allocation (46080 cells)
     *   5. Default QoS configuration
     *   6. Enable packet DMA channels
     */
    rv = bmd_init(switchd.unit);
    if (rv < 0) {
        syslog(LOG_ERR, "BMD: bmd_init failed: %d", rv);
        return -1;
    }

    syslog(LOG_INFO, "BMD: ASIC initialized (SerDes firmware v0x0101 loaded)");
    return 0;
}

int bmd_switching_init_all(void)
{
    int rv;

    syslog(LOG_INFO, "BMD: initializing L2 switching on unit %d", switchd.unit);

    /*
     * bmd_switching_init() does:
     *   1. Create VLAN 1 with all ports as untagged members
     *   2. Set PVID=1 on all ports
     *   3. Enable hardware MAC learning
     *   4. Configure L2 aging timer
     *   5. Set MAC_RSV_MASK for reserved MAC handling
     *   6. Enable CPU port in EPC_LINK_BMAP
     */
    rv = bmd_switching_init(switchd.unit);
    if (rv < 0) {
        syslog(LOG_ERR, "BMD: bmd_switching_init failed: %d", rv);
        return -1;
    }

    syslog(LOG_INFO, "BMD: L2 switching initialized (VLAN 1, MAC learning)");
    return 0;
}
