/*
 * retimer_class.c - Retimer device class framework
 *
 * Creates /sys/class/retimer_dev/ with auto-numbered devices.
 * Each retimer device exports a 'label' attribute read from DT.
 *
 * Based on ONL retimer_class driver, ported to kernel 5.10.
 *
 * Copyright (C) 2013 Accton Technology Corporation.
 * Copyright (C) 2024 EdgeNOS Contributors.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/idr.h>

#include "retimer_class.h"

static struct class *retimer_class;
static DEFINE_IDA(retimer_ida);

struct retimer_dev *retimer_dev_register(struct device *parent,
					 const char *label)
{
	struct retimer_dev *rdev;
	int id;

	rdev = kzalloc(sizeof(*rdev), GFP_KERNEL);
	if (!rdev)
		return ERR_PTR(-ENOMEM);

	id = ida_alloc(&retimer_ida, GFP_KERNEL);
	if (id < 0) {
		kfree(rdev);
		return ERR_PTR(id);
	}

	rdev->id = id;
	if (label)
		strscpy(rdev->label, label, sizeof(rdev->label));

	rdev->dev = device_create(retimer_class, parent, MKDEV(0, 0),
				  rdev, "retimer%d", id);
	if (IS_ERR(rdev->dev)) {
		int err = PTR_ERR(rdev->dev);
		ida_free(&retimer_ida, id);
		kfree(rdev);
		return ERR_PTR(err);
	}

	return rdev;
}
EXPORT_SYMBOL_GPL(retimer_dev_register);

void retimer_dev_unregister(struct retimer_dev *rdev)
{
	if (!rdev)
		return;
	device_unregister(rdev->dev);
	ida_free(&retimer_ida, rdev->id);
	kfree(rdev);
}
EXPORT_SYMBOL_GPL(retimer_dev_unregister);

static ssize_t label_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct retimer_dev *rdev = dev_get_drvdata(dev);
	return sprintf(buf, "%s\n", rdev->label);
}
static DEVICE_ATTR_RO(label);

static struct attribute *retimer_class_attrs[] = {
	&dev_attr_label.attr,
	NULL,
};
ATTRIBUTE_GROUPS(retimer_class);

static int __init retimer_class_init(void)
{
	retimer_class = class_create(THIS_MODULE, "retimer_dev");
	if (IS_ERR(retimer_class))
		return PTR_ERR(retimer_class);

	retimer_class->dev_groups = retimer_class_groups;
	return 0;
}

static void __exit retimer_class_exit(void)
{
	class_destroy(retimer_class);
}

/* Register early so retimer drivers can use it */
subsys_initcall(retimer_class_init);
module_exit(retimer_class_exit);

MODULE_AUTHOR("EdgeNOS Contributors");
MODULE_DESCRIPTION("Retimer device class framework");
MODULE_LICENSE("GPL");
