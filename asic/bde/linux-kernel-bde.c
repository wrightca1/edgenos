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

struct bde_device {
	struct pci_dev *pdev;
	void __iomem *base;
	unsigned long base_phys;
	unsigned int base_size;
	void *dma_virt;
	dma_addr_t dma_phys;
	unsigned int dma_size;
	int valid;
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
		if (!bdev->valid || rio.addr >= bdev->base_size)
			return -EINVAL;
		/*
		 * Use __raw_readl instead of ioread32.
		 * On P2020 PPC, the PCIe outbound window does NOT byte-swap,
		 * so ASIC registers appear in big-endian format to the CPU.
		 * ioread32() adds an unwanted LE→BE conversion.
		 * __raw_readl() reads the raw 32-bit value as-is.
		 */
		rio.val = __raw_readl(bdev->base + rio.addr);
		if (copy_to_user((void __user *)arg, &rio, sizeof(rio)))
			return -EFAULT;
		return 0;

	case BDE_IOC_REG_WRITE:
		if (copy_from_user(&rio, (void __user *)arg, sizeof(rio)))
			return -EFAULT;
		if (rio.dev >= bde_num_devices)
			return -EINVAL;
		bdev = &bde_devices[rio.dev];
		if (!bdev->valid || rio.addr >= bdev->base_size)
			return -EINVAL;
		/* Raw write - no endian conversion (see REG_READ comment) */
		__raw_writel(rio.val, bdev->base + rio.addr);
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
	} else if (bdev->dma_virt && offset == (unsigned long)bdev->dma_phys) {
		/* Map DMA region */
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
