/*
 * bcm5610_bde.c — a custom OpenBCM SDK BDE (ibde_t) for the AS5610-52X
 * (PowerPC / BCM56846, Trident+).
 *
 * The 5610 has no KNET and the stock SDK linux-user-bde does direct BAR0 mmap
 * that misses the iProc PAXB sub-window translation for CMICm registers. edged
 * already solved chip access with a custom /dev/linux-kernel-bde kernel module
 * whose ioctls (a) apply the PPC MMIO barriers and (b) auto-route CMICm regs
 * through PAXB sub-window 7. This adapter presents that PROVEN path to the SDK
 * as an ibde_t, so soc_init/bcm_init (and thus the correct IFP bring-up) can run.
 *
 * Interface + ioctls copied verbatim from edged's asic/bcm56846/bde_interface.c
 * so this stays byte-for-byte compatible with the loaded kernel module.
 *
 * Wire-up: socdiag.c bde_create() calls bcm5610_bde_create(&bde) when the env
 * var BCM5610_BDE is set (see build/build-bcmd-5610.sh).
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

#include <sal/types.h>
#include <ibde.h>

/* ---- kernel-module ioctl ABI (must match newnos/asic/bde/linux-kernel-bde.c) ---- */
#define BCM56846_VENDOR_ID  0x14e4
#define BCM56846_DEVICE_ID  0xb846
#define BCM56846_REVISION   0x02

#define BDE_IOC_MAGIC  'b'
struct bde_dev_info { unsigned int vendor_id, device_id, revision;
                      unsigned long base_addr; unsigned int base_size;
                      unsigned long dma_addr; unsigned int dma_size; };
struct bde_reg_io  { unsigned int dev, addr, val; };
struct bde_dma_info { unsigned int dev, size; unsigned long phys_addr; };

/* PPC ioctl direction encoding: _IOR=1, _IOW=2, _IOWR=3 (see bde_interface.c). */
#define _PPC_IOC(d,t,n,s) (((d)<<30)|((s)<<16)|((t)<<8)|(n))
#define BDE_IOC_DEV_INFO    _PPC_IOC(1, BDE_IOC_MAGIC, 0, sizeof(struct bde_dev_info))
#define BDE_IOC_REG_READ    _PPC_IOC(3, BDE_IOC_MAGIC, 1, sizeof(struct bde_reg_io))
#define BDE_IOC_REG_WRITE   _PPC_IOC(2, BDE_IOC_MAGIC, 2, sizeof(struct bde_reg_io))
#define BDE_IOC_DMA_ALLOC   _PPC_IOC(3, BDE_IOC_MAGIC, 3, sizeof(struct bde_dma_info))
#define BDE_IOC_IPROC_READ  _PPC_IOC(3, BDE_IOC_MAGIC, 7, sizeof(struct bde_reg_io))
#define BDE_IOC_IPROC_WRITE _PPC_IOC(2, BDE_IOC_MAGIC, 8, sizeof(struct bde_reg_io))

/* ---- device state ---- */
static int             g_fd = -1;
static volatile uint32_t *g_bar0;        /* /dev/mem BAR0 (best-effort direct) */
static void           *g_dma;            /* mmap'd DMA pool */
static unsigned long   g_dma_phys;
static unsigned int    g_dma_size;
static unsigned int    g_dma_off;        /* bump allocator */
static ibde_dev_t      g_dev;            /* what get_dev() returns */

/* ---- ibde_t implementation ---- */
static const char *bde_name(void) { return "bcm5610"; }

static int bde_num_devices(int type)
{
    /* one switch device */
    if (type == BDE_ALL_DEVICES || type == BDE_SWITCH_DEVICES) return 1;
    return 0;
}

static const ibde_dev_t *bde_get_dev(int d) { (void)d; return &g_dev; }

static uint32 bde_get_dev_type(int d)
{
    (void)d;
    /* PCI-attached switch, 256K CMIC BAR (BCM56846_BAR0_SIZE = 256K). iProc chip
     * (do NOT set BDE_NO_IPROC). */
    return BDE_PCI_DEV_TYPE | BDE_SWITCH_DEV_TYPE | BDE_256K_REG_SPACE;
}

/* The 56846 is iProc: CMIC registers live at AXI 0x18000000+offset and need the PAXB
 * sub-window translation, NOT direct BAR0. Route the SDK's read/write through edged's
 * IPROC ioctl (which does the sub-window), passing the full AXI address like edged's own
 * bde_iproc_*. (PAXB config regs go via pci_conf, not here.) BCM5610_BDE_LOG=1 dumps the
 * first accesses so we can see the SCHAN register sequence. */
#define IPROC_AXI_BASE 0x18000000u
static int g_log_n = 0, g_log_on = -1;
static int bde_logging(void) { if (g_log_on < 0) g_log_on = getenv("BCM5610_BDE_LOG") ? 1 : 0; return g_log_on; }

static uint32 bde_read(int d, uint32 addr)
{
    struct bde_reg_io rio = { 0, IPROC_AXI_BASE + addr, 0 };
    (void)d;
    if (bde_logging() && g_log_n < 80) { fprintf(stderr, "BDE rd  off=0x%05x axi=0x%08x\n", addr, rio.addr); g_log_n++; }
    if (g_fd < 0 || ioctl(g_fd, BDE_IOC_IPROC_READ, &rio) < 0) return 0;
    return rio.val;
}

static int bde_write(int d, uint32 addr, uint32 data)
{
    struct bde_reg_io rio = { 0, IPROC_AXI_BASE + addr, data };
    (void)d;
    if (bde_logging() && g_log_n < 80) { fprintf(stderr, "BDE wr  off=0x%05x axi=0x%08x =0x%08x\n", addr, rio.addr, data); g_log_n++; }
    if (g_fd < 0 || ioctl(g_fd, BDE_IOC_IPROC_WRITE, &rio) < 0) return -1;
    return 0;
}

static uint32 bde_iproc_read(int d, uint32 addr)
{
    struct bde_reg_io rio = { 0, addr, 0 };
    (void)d;
    if (g_fd < 0 || ioctl(g_fd, BDE_IOC_IPROC_READ, &rio) < 0) return 0;
    return rio.val;
}

static int bde_iproc_write(int d, uint32 addr, uint32 data)
{
    struct bde_reg_io rio = { 0, addr, data };
    (void)d;
    if (g_fd < 0 || ioctl(g_fd, BDE_IOC_IPROC_WRITE, &rio) < 0) return -1;
    return 0;
}

/* PCI config: synthesize the standard header from device info (edged's module
 * doesn't expose a config-space ioctl; the chip is already enabled by the time
 * we attach, so writes are no-ops). */
static uint32 bde_pci_conf_read(int d, uint32 addr)
{
    (void)d;
    switch (addr) {
    case 0x00: return (BCM56846_DEVICE_ID << 16) | BCM56846_VENDOR_ID;
    case 0x04: return 0x00100006;                 /* cmd: mem+bus-master; status */
    case 0x08: return (0x028000u << 8) | BCM56846_REVISION; /* class net + rev */
    case 0x10: return (uint32)(uintptr_t)g_dev.base_address; /* BAR0 */
    default:   return 0;
    }
}
static int bde_pci_conf_write(int d, uint32 addr, uint32 data)
{ (void)d; (void)addr; (void)data; return 0; }

static void bde_pci_bus_features(int d, int *be_pio, int *be_packet, int *be_other)
{
    (void)d;
    if (be_pio)    *be_pio    = 1;   /* big-endian PIO (PPC) */
    if (be_packet) *be_packet = 0;
    if (be_other)  *be_other  = 1;
}

/* DMA pool (bump allocator over the kernel BDE's coherent region). */
static uint32 *bde_salloc(int d, int size, const char *name)
{
    unsigned int aligned = ((unsigned)size + 63) & ~63u;
    void *v;
    (void)d; (void)name;
    if (!g_dma || g_dma_off + aligned > g_dma_size) return NULL;
    v = (uint8_t *)g_dma + g_dma_off;
    g_dma_off += aligned;
    memset(v, 0, size);
    return (uint32 *)v;
}
static void bde_sfree(int d, void *ptr)  { (void)d; (void)ptr; }      /* bump: no free */
static int  bde_sflush(int d, void *a, int l) { (void)d; (void)a; (void)l; return 0; }
static int  bde_sinval(int d, void *a, int l) { (void)d; (void)a; (void)l; return 0; }

static sal_paddr_t bde_l2p(int d, void *laddr)
{ (void)d; return (sal_paddr_t)(g_dma_phys + ((uint8_t *)laddr - (uint8_t *)g_dma)); }
static void *bde_p2l(int d, sal_paddr_t paddr)
{ (void)d; return (void *)((uint8_t *)g_dma + ((unsigned long)paddr - g_dma_phys)); }

static int bde_intr_connect(int d, void (*h)(void *), void *data)
{ (void)d; (void)h; (void)data; return 0; }     /* polling mode for init */
static int bde_intr_disconnect(int d) { (void)d; return 0; }

static int  bde_spi_read(int d, uint32 a, uint8 *b, int l) { (void)d;(void)a;(void)b;(void)l; return -1; }
static int  bde_spi_write(int d, uint32 a, uint8 *b, int l){ (void)d;(void)a;(void)b;(void)l; return -1; }
static uint32 bde_shmem_read(int d, uint32 a, uint8 *b, uint32 l) { (void)d;(void)a;(void)b;(void)l; return 0; }
static void bde_shmem_write(int d, uint32 a, uint8 *b, uint32 l)  { (void)d;(void)a;(void)b;(void)l; }
static sal_vaddr_t bde_shmem_map(int d, uint32 a, uint32 s) { (void)d;(void)a;(void)s; return 0; }
static int bde_get_cmic_ver(int d, uint32 *ver) { (void)d; (void)ver; return -1; }
static int bde_i2c_read(int d, uint32 a, uint32 *v)  { (void)d;(void)a;(void)v; return -1; }
static int bde_i2c_write(int d, uint32 a, uint32 v)  { (void)d;(void)a;(void)v; return -1; }

static ibde_t g_ibde = {
    .name                = bde_name,
    .num_devices         = bde_num_devices,
    .get_dev             = bde_get_dev,
    .get_dev_type        = bde_get_dev_type,
    .pci_conf_read       = bde_pci_conf_read,
    .pci_conf_write      = bde_pci_conf_write,
    .pci_bus_features    = bde_pci_bus_features,
    .read                = bde_read,
    .write               = bde_write,
    .salloc              = bde_salloc,
    .sfree               = bde_sfree,
    .sflush              = bde_sflush,
    .sinval              = bde_sinval,
    .interrupt_connect   = bde_intr_connect,
    .interrupt_disconnect= bde_intr_disconnect,
    .l2p                 = bde_l2p,
    .p2l                 = bde_p2l,
    .spi_read            = bde_spi_read,
    .spi_write           = bde_spi_write,
    .iproc_read          = bde_iproc_read,
    .iproc_write         = bde_iproc_write,
    .shmem_read          = bde_shmem_read,
    .shmem_write         = bde_shmem_write,
    .shmem_map           = bde_shmem_map,
    .get_cmic_ver        = bde_get_cmic_ver,
    .i2c_device_read     = bde_i2c_read,
    .i2c_device_write    = bde_i2c_write,
};

/* Called by socdiag.c bde_create() (patched) instead of linux_bde_create(). */
int bcm5610_bde_create(ibde_t **bde)
{
    struct bde_dev_info info;
    struct bde_dma_info dinfo;
    int mem_fd;

    g_fd = open("/dev/linux-kernel-bde", O_RDWR | O_SYNC);
    if (g_fd < 0) {
        fprintf(stderr, "bcm5610_bde: open /dev/linux-kernel-bde: %s\n", strerror(errno));
        return -1;
    }
    memset(&info, 0, sizeof(info));
    if (ioctl(g_fd, BDE_IOC_DEV_INFO, &info) < 0) {
        fprintf(stderr, "bcm5610_bde: DEV_INFO: %s\n", strerror(errno));
        return -1;
    }
    fprintf(stderr, "bcm5610_bde: BCM%04x rev %02x, BAR0 phys 0x%lx (%u bytes)\n",
            info.device_id, info.revision, info.base_addr, info.base_size);

    /* BAR0 via /dev/mem (best-effort direct access for base_address). */
    g_bar0 = NULL;
    if ((mem_fd = open("/dev/mem", O_RDWR | O_SYNC)) >= 0) {
        void *m = mmap(NULL, info.base_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, mem_fd, info.base_addr);
        if (m != MAP_FAILED) g_bar0 = m;
        close(mem_fd);
    }

    /* DMA pool. */
    memset(&dinfo, 0, sizeof(dinfo));
    if (ioctl(g_fd, BDE_IOC_DMA_ALLOC, &dinfo) == 0) {
        g_dma_phys = dinfo.phys_addr;
        g_dma_size = dinfo.size;
        g_dma = mmap(NULL, g_dma_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
        if (g_dma == MAP_FAILED) g_dma = NULL;
        else fprintf(stderr, "bcm5610_bde: DMA %u MB at phys 0x%lx\n",
                     g_dma_size / (1024*1024), g_dma_phys);
    }
    g_dma_off = 0;

    g_dev.device       = info.device_id ? info.device_id : BCM56846_DEVICE_ID;
    g_dev.rev          = info.revision  ? info.revision  : BCM56846_REVISION;
    /* base_address MUST be 0: the SDK's CMREAD/CMWRITE (Makefile.unix-user = runtime check)
     * dereferences base_address directly when set, which bypasses this adapter and misses
     * the iProc PAXB sub-window the AS5610's PCIe path needs. With base_address==0 it falls
     * through to CMVEC.read/write -> our bde_read/bde_write (which do the sub-window). */
    g_dev.base_address = 0;
    g_dev.base_address1= 0;
    g_dev.base_address2= 0;

    *bde = &g_ibde;
    return 0;
}
