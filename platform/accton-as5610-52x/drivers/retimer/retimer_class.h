/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _RETIMER_CLASS_H
#define _RETIMER_CLASS_H

#include <linux/device.h>

struct retimer_dev {
	struct device *dev;
	int id;
	char label[64];
};

struct retimer_dev *retimer_dev_register(struct device *parent,
					 const char *label);
void retimer_dev_unregister(struct retimer_dev *rdev);

#endif /* _RETIMER_CLASS_H */
