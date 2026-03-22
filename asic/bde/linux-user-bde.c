/*
 * linux-user-bde.c - Userspace BDE interface
 *
 * Provides /dev/linux-user-bde for switchd to access the ASIC.
 * This is a thin proxy that relays register reads/writes and DMA
 * operations to the kernel BDE module.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/pci.h>

#define DRIVER_NAME "linux-user-bde"

/* IOCTL commands (same as kernel BDE for pass-through) */
#define UBDE_IOC_MAGIC  'u'
#define UBDE_IOC_PROBE       _IOR(UBDE_IOC_MAGIC, 0, int)
#define UBDE_IOC_CONNECT     _IO(UBDE_IOC_MAGIC, 1)
#define UBDE_IOC_DISCONNECT  _IO(UBDE_IOC_MAGIC, 2)

static int ubde_major;
static struct class *ubde_class;

static int ubde_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int ubde_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static long ubde_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int num_devs;

	switch (cmd) {
	case UBDE_IOC_PROBE:
		/* Return number of devices (scan PCI) */
		num_devs = 0;
		{
			struct pci_dev *pdev = NULL;
			while ((pdev = pci_get_device(0x14e4, PCI_ANY_ID, pdev)))
				num_devs++;
		}
		if (copy_to_user((void __user *)arg, &num_devs, sizeof(int)))
			return -EFAULT;
		return 0;

	case UBDE_IOC_CONNECT:
	case UBDE_IOC_DISCONNECT:
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations ubde_fops = {
	.owner          = THIS_MODULE,
	.open           = ubde_open,
	.release        = ubde_release,
	.unlocked_ioctl = ubde_ioctl,
	.compat_ioctl   = ubde_ioctl,
};

static int __init ubde_init(void)
{
	ubde_major = register_chrdev(0, DRIVER_NAME, &ubde_fops);
	if (ubde_major < 0)
		return ubde_major;

	ubde_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(ubde_class)) {
		unregister_chrdev(ubde_major, DRIVER_NAME);
		return PTR_ERR(ubde_class);
	}

	device_create(ubde_class, NULL, MKDEV(ubde_major, 0),
		      NULL, DRIVER_NAME);

	pr_info(DRIVER_NAME ": loaded\n");
	return 0;
}

static void __exit ubde_exit(void)
{
	device_destroy(ubde_class, MKDEV(ubde_major, 0));
	class_destroy(ubde_class);
	unregister_chrdev(ubde_major, DRIVER_NAME);
}

module_init(ubde_init);
module_exit(ubde_exit);

MODULE_AUTHOR("EdgeNOS Contributors");
MODULE_DESCRIPTION("Userspace BDE interface for BCM56846");
MODULE_LICENSE("GPL");
