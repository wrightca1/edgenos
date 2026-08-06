// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fm6000dma.c - independent DMA/MSI backing for the FM6000 packet engine.
 *
 * The Arista 7150 (AMD RS780) has NO usable IOMMU (0 iommu_groups, GART fallback;
 * see notes/analysis/phase13-live-probe-2026-07-21.md), so the VFIO path can't
 * work here. This tiny module gives userspace exactly what the FM6000 packet
 * engine needs and nothing more:
 *   - the FM6000 BAR0 (register access), mmap'd noncached
 *   - a physically-contiguous, coherent, low-4GiB DMA pool (32-bit master),
 *     mmap'd into userspace, with its device address reported via ioctl
 *   - MSI completion delivered as an eventfd-compatible read()/poll()
 *
 * It binds the FM6000 (8086:155b / 1823:1770). All packet-ring logic stays in
 * userspace (asic/fm6000/fpdma.c); this module is just BAR0 + pool + IRQ. It is
 * the drop-in replacement for fpdma_vfio on IOMMU-less boxes.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/mm.h>

#include "fm6000dma_uapi.h"

#define DRV "fm6000dma"

static unsigned int pool_mb = 4;
module_param(pool_mb, uint, 0444);
MODULE_PARM_DESC(pool_mb, "coherent DMA pool size in MiB (default 4)");

struct fm6000dma {
	struct pci_dev     *pdev;
	void __iomem       *bar0;
	resource_size_t     bar0_phys;
	resource_size_t     bar0_len;

	void               *pool_cpu;    /* coherent pool (kernel virt) */
	dma_addr_t          pool_dma;    /* device address of the pool  */
	size_t              pool_len;

	int                 irq;
	atomic_t            events;      /* MSI count, drained by read() */
	wait_queue_head_t   wq;

	struct miscdevice   misc;
};

/* Single instance (one FM6000 per pizza box). */
static struct fm6000dma *g_dev;

/* ---- MSI: just notify userspace; userspace acks the FM6000 IP register ---- */
static irqreturn_t fm6000dma_isr(int irq, void *data)
{
	struct fm6000dma *d = data;

	atomic_inc(&d->events);
	wake_up_interruptible(&d->wq);
	return IRQ_HANDLED;
}

/* ---- char device ---- */
static int fm6000dma_open(struct inode *ino, struct file *f)
{
	f->private_data = g_dev;
	return g_dev ? 0 : -ENODEV;
}

static long fm6000dma_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct fm6000dma *d = f->private_data;
	struct fm6000dma_info info;

	switch (cmd) {
	case FM6000DMA_GET_INFO:
		info.bar0_len = d->bar0_len;
		info.pool_len = d->pool_len;
		info.pool_dma = (__u64)d->pool_dma;
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	default:
		return -ENOTTY;
	}
}

/* eventfd-compatible: return a u64 count and clear it. */
static ssize_t fm6000dma_read(struct file *f, char __user *buf,
			      size_t len, loff_t *ppos)
{
	struct fm6000dma *d = f->private_data;
	u64 cnt;

	if (len < sizeof(u64))
		return -EINVAL;
	if (!(f->f_flags & O_NONBLOCK)) {
		int rc = wait_event_interruptible(d->wq, atomic_read(&d->events) > 0);
		if (rc)
			return rc;
	}
	cnt = (u64)atomic_xchg(&d->events, 0);
	if (cnt == 0)
		return -EAGAIN;
	if (copy_to_user(buf, &cnt, sizeof(cnt)))
		return -EFAULT;
	return sizeof(cnt);
}

static __poll_t fm6000dma_poll(struct file *f, struct poll_table_struct *pt)
{
	struct fm6000dma *d = f->private_data;

	poll_wait(f, &d->wq, pt);
	return atomic_read(&d->events) > 0 ? (EPOLLIN | EPOLLRDNORM) : 0;
}

static int fm6000dma_mmap(struct file *f, struct vm_area_struct *vma)
{
	struct fm6000dma *d = f->private_data;
	size_t size = vma->vm_end - vma->vm_start;
	unsigned long off = (unsigned long)vma->vm_pgoff << PAGE_SHIFT;

	if (off == FM6000DMA_OFF_BAR0) {
		if (size > d->bar0_len)
			return -EINVAL;
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		return io_remap_pfn_range(vma, vma->vm_start,
					  d->bar0_phys >> PAGE_SHIFT,
					  size, vma->vm_page_prot);
	}
	if (off == FM6000DMA_OFF_POOL) {
		if (size > d->pool_len)
			return -EINVAL;
		/* Reset pgoff: dma_mmap_coherent maps from the pool base. */
		vma->vm_pgoff = 0;
		return dma_mmap_coherent(&d->pdev->dev, vma, d->pool_cpu,
					 d->pool_dma, size);
	}
	return -EINVAL;
}

static const struct file_operations fm6000dma_fops = {
	.owner          = THIS_MODULE,
	.open           = fm6000dma_open,
	.unlocked_ioctl = fm6000dma_ioctl,
	.read           = fm6000dma_read,
	.poll           = fm6000dma_poll,
	.mmap           = fm6000dma_mmap,
	.llseek         = noop_llseek,
};

/* ---- PCI ---- */
static int fm6000dma_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct fm6000dma *d;
	int rc;

	if (g_dev)
		return -EBUSY;                 /* single instance */

	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->pdev = pdev;
	init_waitqueue_head(&d->wq);
	atomic_set(&d->events, 0);

	rc = pcim_enable_device(pdev);
	if (rc)
		return rc;

	d->bar0 = pcim_iomap_region(pdev, 0, DRV);
	if (IS_ERR(d->bar0))
		return PTR_ERR(d->bar0);
	d->bar0_phys = pci_resource_start(pdev, 0);
	d->bar0_len  = pci_resource_len(pdev, 0);

	pci_set_master(pdev);

	/* FM6000 is a 32-bit DMA master -> coherent pool lands in low 4 GiB. */
	rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (rc) {
		dev_err(&pdev->dev, "no 32-bit DMA mask\n");
		return rc;
	}

	d->pool_len = (size_t)pool_mb << 20;
	d->pool_cpu = dma_alloc_coherent(&pdev->dev, d->pool_len,
					 &d->pool_dma, GFP_KERNEL);
	if (!d->pool_cpu) {
		dev_err(&pdev->dev, "dma_alloc_coherent(%zu) failed\n", d->pool_len);
		return -ENOMEM;
	}
	if (upper_32_bits(d->pool_dma)) {
		dev_err(&pdev->dev, "pool dma 0x%llx not < 4 GiB\n",
			(unsigned long long)d->pool_dma);
		rc = -ENOMEM;
		goto free_pool;
	}

	rc = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (rc < 0) {
		dev_err(&pdev->dev, "MSI alloc failed\n");
		goto free_pool;
	}
	d->irq = pci_irq_vector(pdev, 0);
	rc = request_irq(d->irq, fm6000dma_isr, 0, DRV, d);
	if (rc)
		goto free_msi;

	d->misc.minor = MISC_DYNAMIC_MINOR;
	d->misc.name  = FM6000DMA_DEVNAME;
	d->misc.fops  = &fm6000dma_fops;
	rc = misc_register(&d->misc);
	if (rc)
		goto free_irq;

	pci_set_drvdata(pdev, d);
	g_dev = d;
	dev_info(&pdev->dev,
		 "fm6000dma up: BAR0 %llu KiB, pool %u MiB @ dma 0x%llx, MSI irq %d\n",
		 (unsigned long long)d->bar0_len / 1024, pool_mb,
		 (unsigned long long)d->pool_dma, d->irq);
	return 0;

free_irq:
	free_irq(d->irq, d);
free_msi:
	pci_free_irq_vectors(pdev);
free_pool:
	dma_free_coherent(&pdev->dev, d->pool_len, d->pool_cpu, d->pool_dma);
	return rc;
}

static void fm6000dma_remove(struct pci_dev *pdev)
{
	struct fm6000dma *d = pci_get_drvdata(pdev);

	misc_deregister(&d->misc);
	free_irq(d->irq, d);
	pci_free_irq_vectors(pdev);
	dma_free_coherent(&pdev->dev, d->pool_len, d->pool_cpu, d->pool_dma);
	g_dev = NULL;
}

static const struct pci_device_id fm6000dma_ids[] = {
	{ PCI_DEVICE(0x8086, 0x155b) },   /* FM6000 (Intel vendor id)  */
	{ PCI_DEVICE(0x1823, 0x1770) },   /* FM6000 (Fulcrum vendor id) */
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, fm6000dma_ids);

static struct pci_driver fm6000dma_driver = {
	.name     = DRV,
	.id_table = fm6000dma_ids,
	.probe    = fm6000dma_probe,
	.remove   = fm6000dma_remove,
};
module_pci_driver(fm6000dma_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("EdgeNOS independent FM6000 DMA/MSI backing (IOMMU-less boxes)");
MODULE_AUTHOR("EdgeNOS Contributors");
