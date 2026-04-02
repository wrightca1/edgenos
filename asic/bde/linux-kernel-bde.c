/*
 * linux-kernel-bde.c - Broadcom Device Enumerator kernel module
 *
 * Probes BCM56846 (Trident+) on PCIe, maps BAR0, allocates DMA pool,
 * and exposes device to userspace via /dev/linux-kernel-bde.
 *
 * Based on OpenMDK libbde, ported to kernel 5.10.
 *
 * Copyright (C) 2007-2020 Broadcom Inc.
 * Copyright (C) 2024 EdgeNOS Contributors.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>

#define DRIVER_NAME    "linux-kernel-bde"
#define BDE_MAX_DEVICES 4

/* Broadcom PCI vendor ID */
#define PCI_VENDOR_ID_BROADCOM  0x14e4

/* BCM56846 = Trident+ (uses BCM56840 base, variant BCM56846) */
#define PCI_DEVICE_ID_BCM56846  0xb846
#define PCI_DEVICE_ID_BCM56840  0xb840
#define PCI_DEVICE_ID_BCM56845  0xb845

/* IOCTL commands */
#define BDE_IOC_MAGIC  'b'
#define BDE_IOC_DEV_INFO     _IOR(BDE_IOC_MAGIC, 0, struct bde_dev_info)
#define BDE_IOC_REG_READ     _IOWR(BDE_IOC_MAGIC, 1, struct bde_reg_io)
#define BDE_IOC_REG_WRITE    _IOW(BDE_IOC_MAGIC, 2, struct bde_reg_io)
#define BDE_IOC_DMA_ALLOC    _IOWR(BDE_IOC_MAGIC, 3, struct bde_dma_info)
#define BDE_IOC_GET_NUM_DEVS _IOR(BDE_IOC_MAGIC, 4, int)
#define BDE_IOC_IPROC_READ   _IOWR(BDE_IOC_MAGIC, 7, struct bde_reg_io)
#define BDE_IOC_IPROC_WRITE  _IOW(BDE_IOC_MAGIC, 8, struct bde_reg_io)

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

/* iProc PAXB sub-window definitions */
#define PAXB_NUM_SUBWIN        8
#define PAXB_SUBWIN_SIZE       0x1000
#define PAXB_IMAP0_BASE        0x2c00     /* IMAP0_0 register offset in BAR0 */
#define PAXB_REMAP_SUBWIN      7          /* Sub-window used for dynamic remap */
#define PAXB_REMAP_BAR0_BASE   (PAXB_REMAP_SUBWIN * PAXB_SUBWIN_SIZE) /* 0x7000 */
#define PAXB_IMAP_VALID        0x1        /* Valid bit in IMAP register */
#define PAXB_AXI_BASE          0x18000000 /* AXI base for iProc CMIC registers */

struct bde_device {
	struct pci_dev *pdev;
	void __iomem *base;
	unsigned long base_phys;
	unsigned int base_size;
	void *dma_virt;
	dma_addr_t dma_phys;
	unsigned int dma_size;
	int valid;

	/* iProc sub-window cache: subwin_base[i] = AXI base addr (page-aligned) */
	u32 subwin_base[PAXB_NUM_SUBWIN];
	int subwin_valid;
};

static struct bde_device bde_devices[BDE_MAX_DEVICES];
static int bde_num_devices;
static int bde_major;
static struct class *bde_class;

/* Module parameters */
static int maxpayload = 256;
module_param(maxpayload, int, 0644);
MODULE_PARM_DESC(maxpayload, "PCIe max payload size (default 256)");

static int dma_size = 4;  /* MB */
module_param(dma_size, int, 0644);
MODULE_PARM_DESC(dma_size, "DMA pool size in MB (default 4)");

/*
 * iProc PAXB sub-window register access.
 *
 * BCM56846 uses iProc (PCI-AXI bridge) with 8 sub-windows of 4KB each.
 * BAR0 offsets 0x0000-0x7FFF map to AXI addresses via IMAP0_0 through
 * IMAP0_7 registers at BAR0+0x2C00..0x2C1C.
 *
 * Each IMAP register holds: (AXI_page_address & ~0xFFF) | valid_bit
 *
 * On boot, the default IMAP values map sub-window 0 to AXI 0x18000000
 * (legacy CMIC).  Sub-windows 1-7 have default values that may or may
 * not be useful.
 *
 * To access an arbitrary AXI address:
 *   1. Check if any cached sub-window already maps the 4K page
 *   2. If not, remap sub-window 7 by writing (axi_base | 1) to IMAP0_7
 *   3. Access BAR0 + (subwin * 0x1000) + (addr & 0xFFF)
 *
 * This matches the Broadcom SDK shbde_iproc_pci_read/write from
 * shbde_iproc.c.
 *
 * Register access uses ioread32/iowrite32 (PPC LE-converted I/O).
 * Verified on BCM56846: iowrite32 works for ALL register writes on
 * fresh cold-booted ASIC.  CDK with SYS_BE_PIO=1 handles value
 * byte-swap in software.
 *
 * IMPORTANT: The ASIC must be cold-booted (full power cycle) for
 * reliable operation.  Warm reboot or module reload after errors may
 * leave the ASIC in a corrupted state where S-Channel and MIIM fail.
 */

static DEFINE_SPINLOCK(iproc_lock);

/* Cache the current IMAP values from hardware */
static void subwin_cache_init(struct bde_device *bdev)
{
	int i;

	for (i = 0; i < PAXB_NUM_SUBWIN; i++) {
		u32 imap = ioread32(bdev->base + PAXB_IMAP0_BASE + (4 * i));
		bdev->subwin_base[i] = imap & ~0xfff;
		dev_dbg(&bdev->pdev->dev, "IMAP%d = 0x%08x (base 0x%08x)\n",
			i, imap, bdev->subwin_base[i]);
	}
	bdev->subwin_valid = 1;
}

/*
 * Direct BAR0 register read/write (for offsets within the 256KB window).
 * Used for legacy CMIC registers in sub-window 0 and for PAXB config
 * registers (IMAP, OARR, endianness) which are always at fixed BAR0
 * offsets regardless of sub-window mapping.
 */
static u32 iproc_read(struct bde_device *bdev, u32 addr)
{
	if (addr >= bdev->base_size)
		return 0xffffffff;

	return ioread32(bdev->base + addr);
}

static void iproc_write(struct bde_device *bdev, u32 addr, u32 val)
{
	if (addr >= bdev->base_size)
		return;

	iowrite32(val, bdev->base + addr);
}

/*
 * iProc AXI register read via sub-window translation.
 *
 * @bdev: BDE device
 * @axi_addr: Full AXI address (e.g. 0x18032000 for CMICm MIIM)
 *
 * Looks up cached sub-windows for a match; if none, remaps sub-window 7.
 * Must be called with iproc_lock held if concurrent access is possible.
 */
static u32 iproc_axi_read(struct bde_device *bdev, u32 axi_addr)
{
	u32 page = axi_addr & ~0xfff;
	u32 offset = axi_addr & 0xfff;
	int i;

	if (!bdev->subwin_valid)
		subwin_cache_init(bdev);

	/* Search existing sub-windows for a match */
	for (i = 0; i < PAXB_NUM_SUBWIN; i++) {
		if (bdev->subwin_base[i] == page)
			return ioread32(bdev->base + (i * PAXB_SUBWIN_SIZE) + offset);
	}

	/* Remap sub-window 7 via iowrite32 to IMAP register. */
	iowrite32(page | PAXB_IMAP_VALID,
		  bdev->base + PAXB_IMAP0_BASE + (4 * PAXB_REMAP_SUBWIN));
	bdev->subwin_base[PAXB_REMAP_SUBWIN] =
		ioread32(bdev->base + PAXB_IMAP0_BASE + (4 * PAXB_REMAP_SUBWIN)) & ~0xfff;

	return ioread32(bdev->base + PAXB_REMAP_BAR0_BASE + offset);
}

/*
 * iProc AXI register write via sub-window translation.
 * Same logic as iproc_axi_read but writes instead.
 */
static void iproc_axi_write(struct bde_device *bdev, u32 axi_addr, u32 val)
{
	u32 page = axi_addr & ~0xfff;
	u32 offset = axi_addr & 0xfff;
	int i;

	if (!bdev->subwin_valid)
		subwin_cache_init(bdev);

	/* Search existing sub-windows for a match */
	for (i = 0; i < PAXB_NUM_SUBWIN; i++) {
		if (bdev->subwin_base[i] == page) {
			iowrite32(val, bdev->base + (i * PAXB_SUBWIN_SIZE) + offset);
			return;
		}
	}

	/* Remap sub-window 7 via iowrite32 to IMAP register */
	iowrite32(page | PAXB_IMAP_VALID,
		  bdev->base + PAXB_IMAP0_BASE + (4 * PAXB_REMAP_SUBWIN));
	bdev->subwin_base[PAXB_REMAP_SUBWIN] =
		ioread32(bdev->base + PAXB_IMAP0_BASE + (4 * PAXB_REMAP_SUBWIN)) & ~0xfff;

	iowrite32(val, bdev->base + PAXB_REMAP_BAR0_BASE + offset);
}

/* ── Interrupt handler (forward declaration) ─────────────────── */
static irqreturn_t bde_isr(int irq, void *dev_id);

/* ── PCI operations ──────────────────────────────────────────── */

static int bde_pci_probe(struct pci_dev *pdev,
			 const struct pci_device_id *id)
{
	struct bde_device *bdev;
	int ret;

	if (bde_num_devices >= BDE_MAX_DEVICES) {
		dev_err(&pdev->dev, "Too many BDE devices\n");
		return -ENOMEM;
	}

	bdev = &bde_devices[bde_num_devices];

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable PCI device\n");
		return ret;
	}

	pci_set_master(pdev);

	/* Set DMA mask */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "Failed to set DMA mask\n");
		goto err_disable;
	}

	/* Map BAR0 (CMIC registers) */
	bdev->base_phys = pci_resource_start(pdev, 0);
	bdev->base_size = pci_resource_len(pdev, 0);

	ret = pci_request_region(pdev, 0, DRIVER_NAME);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request BAR0\n");
		goto err_disable;
	}

	bdev->base = pci_ioremap_bar(pdev, 0);
	if (!bdev->base) {
		dev_err(&pdev->dev, "Failed to map BAR0\n");
		ret = -ENOMEM;
		goto err_release;
	}

	/* Allocate DMA pool */
	bdev->dma_size = dma_size * 1024 * 1024;
	bdev->dma_virt = dma_alloc_coherent(&pdev->dev, bdev->dma_size,
					     &bdev->dma_phys, GFP_KERNEL);
	if (!bdev->dma_virt) {
		dev_warn(&pdev->dev, "Failed to allocate %u MB DMA pool\n",
			 dma_size);
		bdev->dma_size = 0;
	}

	/* Set PCIe max payload if supported */
	if (maxpayload > 0 && pci_is_pcie(pdev)) {
		pcie_set_readrq(pdev, maxpayload);
	}

	/* DEBUG: Test PAXB register writes via iowrite32 vs __raw_writel */
	{
		u32 before, after_io, after_raw;

		/* Test ENDIAN_SEL at BAR0+0x174 */
		iowrite32(0x0, bdev->base + 0x174);     /* Clear */
		before = ioread32(bdev->base + 0x174);

		iowrite32(0x06000006, bdev->base + 0x174);
		after_io = ioread32(bdev->base + 0x174);

		iowrite32(0x0, bdev->base + 0x174);     /* Clear */
		__raw_writel(0x06000006, bdev->base + 0x174);
		after_raw = __raw_readl(bdev->base + 0x174);

		dev_info(&pdev->dev,
			 "ENDIAN_SEL test: before=0x%08x iowrite32=0x%08x __raw_writel=0x%08x\n",
			 before, after_io, after_raw);

		/* Test OARR_2 at BAR0+0x2D60 */
		before = ioread32(bdev->base + 0x2D60);
		iowrite32(0x1, bdev->base + 0x2D60);
		after_io = ioread32(bdev->base + 0x2D60);
		__raw_writel(0x01000000, bdev->base + 0x2D60);
		after_raw = __raw_readl(bdev->base + 0x2D60);

		dev_info(&pdev->dev,
			 "OARR_2 test: before=0x%08x iowrite32(0x1)=0x%08x __raw_writel(0x01000000)=0x%08x\n",
			 before, after_io, after_raw);

		/* Clean up — set OARR_2 to enable */
		__raw_writel(0x01000000, bdev->base + 0x2D60);
	}

	/*
	 * PAXB endianness: leave at default (no swap).
	 */
	{
		u32 paxb_endian = ioread32(bdev->base + 0x2030);
		dev_info(&pdev->dev, "PAXB endianness: 0x%08x\n", paxb_endian);
	}

	/*
	 * Test IMAP0_7 write via iowrite32 (direct BAR0 MMIO).
	 * The Broadcom SDK (shbde_iproc.c) uses writel for IMAP.
	 * Target: remap sub-window 7 to AXI 0x18031000 (CMICm DMA).
	 */
	{
		u32 imap_before, imap_after, imap_after_raw;
		u32 test_val = 0x18031001; /* AXI page 0x18031000 + valid */

		imap_before = ioread32(bdev->base + 0x2C1C);

		/* Try iowrite32 */
		iowrite32(test_val, bdev->base + 0x2C1C);
		imap_after = ioread32(bdev->base + 0x2C1C);

		/* Try __raw_writel (no byte-swap) */
		__raw_writel(test_val, bdev->base + 0x2C1C);
		imap_after_raw = __raw_readl(bdev->base + 0x2C1C);

		dev_info(&pdev->dev,
			 "IMAP0_7: before=0x%08x iowrite32=0x%08x raw=0x%08x (target=0x%08x)\n",
			 imap_before, imap_after, imap_after_raw, test_val);

		/* If iowrite32 worked, try accessing CMICm through sub-window 7 */
		if (imap_after == test_val || imap_after_raw == test_val) {
			u32 cmicm_test = ioread32(bdev->base + 0x7140);
			dev_info(&pdev->dev,
				 "CMICm DMA_CTRL via subwin7: 0x%08x\n", cmicm_test);
		}
	}

	/*
	 * PAXB DMA configuration.
	 *
	 * The iProc PCI-AXI bridge (PAXB) needs outbound address range
	 * registers (OARR) configured to allow the ASIC DMA engine to
	 * write to host memory. Without this, DMA TX/RX operations
	 * timeout (CDK_E_TIMEOUT).
	 *
	 * From Broadcom SDK shbde_iproc_paxb_init():
	 *   BAR0+0x2104 (PCIE_EP_AXI_CONFIG) = 0x0
	 *   BAR0+0x2D60 (OARR_2) = 0x1 (enable)
	 *   BAR0+0x2D64 (OARR_2_UPPER) = dma_hi_bits
	 *
	 * dma_hi_bits = 0x1 for PCI core 0, 0x2 for PCI core 1.
	 * BCM56846 uses PCI core 0 → dma_hi_bits = 0x1.
	 */
	/*
	 * Write OARR via PCI config space — BAR0 MMIO writes to PAXB
	 * config registers (0x2xxx) don't persist after ASIC core reset.
	 * PCI config space writes go through the iProc PAXB and persist.
	 */
	{
		u32 oarr2;

		/* Disable EP AXI config via PCI config space */
		pci_write_config_dword(pdev, 0x2104, 0x0);

		/* Enable OARR_2 for outbound DMA */
		pci_write_config_dword(pdev, 0x2D60, 0x1);

		/* DMA upper address bits (0 for 32-bit P2020) */
		pci_write_config_dword(pdev, 0x2D64, 0x0);

		pci_read_config_dword(pdev, 0x2D60, &oarr2);
		dev_info(&pdev->dev, "PAXB DMA: OARR_2=0x%08x (via PCI config)\n",
			 oarr2);
	}

	/* Initialize iProc sub-window cache from current IMAP values */
	subwin_cache_init(bdev);

	/* Request interrupt */
	ret = request_irq(pdev->irq, bde_isr, IRQF_SHARED,
			  DRIVER_NAME, bdev);
	if (ret) {
		dev_warn(&pdev->dev, "Failed to request IRQ %d (polling mode)\n",
			 pdev->irq);
		/* Non-fatal: we can poll instead */
	}

	bdev->pdev = pdev;
	bdev->valid = 1;
	pci_set_drvdata(pdev, bdev);
	bde_num_devices++;

	dev_info(&pdev->dev, "BCM%04x (rev %02x) at 0x%lx, %u MB DMA at 0x%llx\n",
		 pdev->device, pdev->revision,
		 bdev->base_phys, bdev->dma_size / (1024 * 1024),
		 (unsigned long long)bdev->dma_phys);

	return 0;

err_release:
	pci_release_region(pdev, 0);
err_disable:
	pci_disable_device(pdev);
	return ret;
}

static void bde_pci_remove(struct pci_dev *pdev)
{
	struct bde_device *bdev = pci_get_drvdata(pdev);

	if (!bdev)
		return;

	free_irq(pdev->irq, bdev);

	if (bdev->dma_virt)
		dma_free_coherent(&pdev->dev, bdev->dma_size,
				  bdev->dma_virt, bdev->dma_phys);

	if (bdev->base)
		iounmap(bdev->base);

	pci_release_region(pdev, 0);
	pci_disable_device(pdev);
	bdev->valid = 0;
}

static const struct pci_device_id bde_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM, PCI_DEVICE_ID_BCM56846) },
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM, PCI_DEVICE_ID_BCM56840) },
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM, PCI_DEVICE_ID_BCM56845) },
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM, 0xb847) },  /* BCM56847 */
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, bde_pci_ids);

static struct pci_driver bde_pci_driver = {
	.name     = DRIVER_NAME,
	.id_table = bde_pci_ids,
	.probe    = bde_pci_probe,
	.remove   = bde_pci_remove,
};

/* ── Interrupt handling ──────────────────────────────────────── */

/*
 * The Cumulus BDE uses two ioctls for synchronization:
 *   WAIT_FOR_INTERRUPT (0x20004c09): blocks until ASIC interrupt fires
 *   SEM_OP (0x20004c0a): semaphore for thread coordination
 *
 * For OpenMDK BMD, the DMA completion is polled (bmd_xgs_dma_tx_poll,
 * bmd_xgs_dma_rx_poll) rather than interrupt-driven. So we don't need
 * a full interrupt handler initially.
 *
 * However, we do need to support the interrupt wait for future use.
 */
static DECLARE_WAIT_QUEUE_HEAD(bde_wait);
static volatile int bde_interrupt_pending;

static irqreturn_t bde_isr(int irq, void *dev_id)
{
	bde_interrupt_pending = 1;
	wake_up_interruptible(&bde_wait);
	return IRQ_HANDLED;
}

#define BDE_IOC_WAIT_INTR    _IO(BDE_IOC_MAGIC, 5)
#define BDE_IOC_SEM_OP       _IO(BDE_IOC_MAGIC, 6)

/* ── Character device operations ─────────────────────────────── */

static long bde_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct bde_dev_info info;
	struct bde_reg_io rio;
	struct bde_dma_info dinfo;
	struct bde_device *bdev;

	switch (cmd) {
	case BDE_IOC_GET_NUM_DEVS:
		if (copy_to_user((void __user *)arg, &bde_num_devices,
				 sizeof(int)))
			return -EFAULT;
		return 0;

	case BDE_IOC_DEV_INFO:
		if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
			return -EFAULT;
		/* Use first device by default */
		bdev = &bde_devices[0];
		if (!bdev->valid)
			return -ENODEV;
		info.vendor_id = bdev->pdev->vendor;
		info.device_id = bdev->pdev->device;
		info.revision = bdev->pdev->revision;
		info.base_addr = bdev->base_phys;
		info.base_size = bdev->base_size;
		info.dma_addr = (unsigned long)bdev->dma_phys;
		info.dma_size = bdev->dma_size;
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;

	case BDE_IOC_REG_READ:
		if (copy_from_user(&rio, (void __user *)arg, sizeof(rio)))
			return -EFAULT;
		if (rio.dev >= bde_num_devices)
			return -EINVAL;
		bdev = &bde_devices[rio.dev];
		if (!bdev->valid)
			return -ENODEV;
		/*
		 * Register access routing:
		 *
		 * Addresses < 0x40000 (256KB BAR0) are CMIC register offsets.
		 * These map to AXI 0x18000000 + offset and need sub-window
		 * remapping for offsets above 0x1000 (CMICm DMA at 0x31xxx,
		 * SCHAN at 0x33xxx, MIIM at 0x32xxx).
		 *
		 * Addresses >= 0x40000 are S-Channel encoded register
		 * addresses (block/port/type). These go through SCHAN_MSG
		 * + SCHAN_CTRL at BAR0+0x050 — the CDK handles this
		 * internally by writing to SCHAN_MSG first, then triggering
		 * via SCHAN_CTRL. They should NOT be treated as AXI offsets.
		 */
		if (rio.addr >= bdev->base_size)
			return -EINVAL;
		/*
		 * Register access routing:
		 * - Addresses < 0x8000: direct BAR0 (all 8 sub-windows).
		 *   Sub-window 0 (0x0000-0x0FFF) = CMIC/SCHAN
		 *   Sub-window 2 (0x2000-0x2FFF) = PAXB config (IMAP, OARR)
		 *   Sub-window 7 (0x7000-0x7FFF) = dynamic AXI remap target
		 * - Addresses >= 0x8000: AXI sub-window remap via IMAP0_7.
		 *   For CMICm DMA (0x31xxx), MIIM (0x32xxx), etc.
		 */
		if (rio.addr >= PAXB_SUBWIN_SIZE)
			rio.val = iproc_axi_read(bdev,
						 PAXB_AXI_BASE + rio.addr);
		else
			rio.val = iproc_read(bdev, rio.addr);
		if (copy_to_user((void __user *)arg, &rio, sizeof(rio)))
			return -EFAULT;
		return 0;

	case BDE_IOC_REG_WRITE:
		if (copy_from_user(&rio, (void __user *)arg, sizeof(rio)))
			return -EFAULT;
		if (rio.dev >= bde_num_devices)
			return -EINVAL;
		bdev = &bde_devices[rio.dev];
		if (!bdev->valid)
			return -ENODEV;
		if (rio.addr >= bdev->base_size)
			return -EINVAL;
		if (rio.addr >= PAXB_SUBWIN_SIZE)
			iproc_axi_write(bdev,
					PAXB_AXI_BASE + rio.addr, rio.val);
		else
			iproc_write(bdev, rio.addr, rio.val);
		return 0;

	case BDE_IOC_DMA_ALLOC:
		if (copy_from_user(&dinfo, (void __user *)arg, sizeof(dinfo)))
			return -EFAULT;
		if (dinfo.dev >= bde_num_devices)
			return -EINVAL;
		bdev = &bde_devices[dinfo.dev];
		if (!bdev->valid || !bdev->dma_virt)
			return -ENODEV;
		dinfo.phys_addr = (unsigned long)bdev->dma_phys;
		dinfo.size = bdev->dma_size;
		if (copy_to_user((void __user *)arg, &dinfo, sizeof(dinfo)))
			return -EFAULT;
		return 0;

	case BDE_IOC_IPROC_READ:
		if (copy_from_user(&rio, (void __user *)arg, sizeof(rio)))
			return -EFAULT;
		if (rio.dev >= bde_num_devices)
			return -EINVAL;
		bdev = &bde_devices[rio.dev];
		if (!bdev->valid)
			return -ENODEV;
		spin_lock(&iproc_lock);
		rio.val = iproc_axi_read(bdev, rio.addr);
		spin_unlock(&iproc_lock);
		if (copy_to_user((void __user *)arg, &rio, sizeof(rio)))
			return -EFAULT;
		return 0;

	case BDE_IOC_IPROC_WRITE:
		if (copy_from_user(&rio, (void __user *)arg, sizeof(rio)))
			return -EFAULT;
		if (rio.dev >= bde_num_devices)
			return -EINVAL;
		bdev = &bde_devices[rio.dev];
		if (!bdev->valid)
			return -ENODEV;
		spin_lock(&iproc_lock);
		iproc_axi_write(bdev, rio.addr, rio.val);
		spin_unlock(&iproc_lock);
		return 0;

	case BDE_IOC_WAIT_INTR:
		/* Block until ASIC interrupt fires */
		bde_interrupt_pending = 0;
		wait_event_interruptible(bde_wait, bde_interrupt_pending);
		return 0;

	case BDE_IOC_SEM_OP:
		/* Semaphore operation (wake up waiters) */
		bde_interrupt_pending = 1;
		wake_up_interruptible(&bde_wait);
		return 0;

	default:
		return -ENOTTY;
	}
}

static int bde_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct bde_device *bdev = &bde_devices[0];
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (!bdev->valid)
		return -ENODEV;

	/* Check if mapping BAR0 or DMA region */
	if (offset == bdev->base_phys) {
		/* Map BAR0 (CMIC registers) */
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		return io_remap_pfn_range(vma, vma->vm_start,
					  offset >> PAGE_SHIFT,
					  size, vma->vm_page_prot);
	} else if (bdev->dma_virt &&
		   (offset == (unsigned long)bdev->dma_phys ||
		    offset == 0)) {
		/*
		 * Map DMA region.
		 * dma_mmap_coherent() requires vm_pgoff=0 (offset into
		 * the DMA allocation). Reset it here so userspace can
		 * pass either offset=0 or offset=dma_phys.
		 */
		vma->vm_pgoff = 0;
		return dma_mmap_coherent(&bdev->pdev->dev, vma,
					 bdev->dma_virt, bdev->dma_phys,
					 bdev->dma_size);
	}

	return -EINVAL;
}

static int bde_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int bde_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static const struct file_operations bde_fops = {
	.owner          = THIS_MODULE,
	.open           = bde_open,
	.release        = bde_release,
	.unlocked_ioctl = bde_ioctl,
	.compat_ioctl   = bde_ioctl,
	.mmap           = bde_mmap,
};

/* ── Module init/exit ────────────────────────────────────────── */

static int __init bde_init(void)
{
	int ret;

	bde_major = register_chrdev(0, DRIVER_NAME, &bde_fops);
	if (bde_major < 0)
		return bde_major;

	bde_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(bde_class)) {
		unregister_chrdev(bde_major, DRIVER_NAME);
		return PTR_ERR(bde_class);
	}

	device_create(bde_class, NULL, MKDEV(bde_major, 0),
		      NULL, DRIVER_NAME);

	ret = pci_register_driver(&bde_pci_driver);
	if (ret) {
		device_destroy(bde_class, MKDEV(bde_major, 0));
		class_destroy(bde_class);
		unregister_chrdev(bde_major, DRIVER_NAME);
		return ret;
	}

	pr_info(DRIVER_NAME ": loaded, %d device(s) found\n", bde_num_devices);
	return 0;
}

static void __exit bde_exit(void)
{
	pci_unregister_driver(&bde_pci_driver);
	device_destroy(bde_class, MKDEV(bde_major, 0));
	class_destroy(bde_class);
	unregister_chrdev(bde_major, DRIVER_NAME);
}

module_init(bde_init);
module_exit(bde_exit);

MODULE_AUTHOR("EdgeNOS Contributors");
MODULE_DESCRIPTION("Broadcom Device Enumerator for BCM56846/Trident+");
MODULE_LICENSE("GPL");
