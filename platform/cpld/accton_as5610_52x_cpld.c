/*
 * accton_as5610_52x_cpld.c - CPLD driver for Accton AS5610-52X
 *
 * CPLD is memory-mapped via eLBC CS1 at 0xEA000000.
 * Provides sysfs access to PSU status, fan control, LEDs, etc.
 *
 * Based on ONL/Cumulus CPLD driver, ported to kernel 5.10 platform_driver.
 *
 * Copyright (C) 2014 Accton Technology Corporation.
 * Copyright (C) 2024 EdgeNOS Contributors.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/sysfs.h>
#include <linux/hwmon.h>

#define DRIVER_NAME     "as5610_52x_cpld"
#define CPLD_PHYS_BASE  0xEA000000
#define CPLD_SIZE       0x100

/* CPLD Register Map */
#define CPLD_REG_VERSION    0x00
#define CPLD_REG_PSU_STATUS 0x01
#define CPLD_REG_PSU_CTRL   0x02
#define CPLD_REG_FAN_STATUS 0x03
#define CPLD_REG_FAN_PWM    0x0D
#define CPLD_REG_LED_SYS    0x13
#define CPLD_REG_LED_LOC    0x15
#define CPLD_REG_RESET      0x10
#define CPLD_REG_INT_STATUS 0x11

struct as5610_cpld {
	void __iomem *base;
	struct platform_device *pdev;
	struct mutex lock;
};

static struct as5610_cpld *cpld_data;

static u8 cpld_read(struct as5610_cpld *cpld, u8 reg)
{
	return ioread8(cpld->base + reg);
}

static void cpld_write(struct as5610_cpld *cpld, u8 reg, u8 val)
{
	iowrite8(val, cpld->base + reg);
}

/* ── Sysfs attributes ─────────────────────────────────────────── */

static ssize_t version_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	u8 val;

	mutex_lock(&cpld->lock);
	val = cpld_read(cpld, CPLD_REG_VERSION);
	mutex_unlock(&cpld->lock);

	return sprintf(buf, "%u\n", val);
}
static DEVICE_ATTR_RO(version);

static ssize_t psu_status_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	u8 val;

	mutex_lock(&cpld->lock);
	val = cpld_read(cpld, CPLD_REG_PSU_STATUS);
	mutex_unlock(&cpld->lock);

	/* Bits: [0]=PSU1 present, [1]=PSU1 good, [2]=PSU2 present, [3]=PSU2 good */
	return sprintf(buf, "0x%02x\n", val);
}
static DEVICE_ATTR_RO(psu_status);

static ssize_t psu1_present_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	u8 val;

	mutex_lock(&cpld->lock);
	val = cpld_read(cpld, CPLD_REG_PSU_STATUS);
	mutex_unlock(&cpld->lock);

	return sprintf(buf, "%u\n", (val & 0x01) ? 1 : 0);
}
static DEVICE_ATTR_RO(psu1_present);

static ssize_t psu1_good_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	u8 val;

	mutex_lock(&cpld->lock);
	val = cpld_read(cpld, CPLD_REG_PSU_STATUS);
	mutex_unlock(&cpld->lock);

	return sprintf(buf, "%u\n", (val & 0x02) ? 1 : 0);
}
static DEVICE_ATTR_RO(psu1_good);

static ssize_t psu2_present_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	u8 val;

	mutex_lock(&cpld->lock);
	val = cpld_read(cpld, CPLD_REG_PSU_STATUS);
	mutex_unlock(&cpld->lock);

	return sprintf(buf, "%u\n", (val & 0x04) ? 1 : 0);
}
static DEVICE_ATTR_RO(psu2_present);

static ssize_t psu2_good_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	u8 val;

	mutex_lock(&cpld->lock);
	val = cpld_read(cpld, CPLD_REG_PSU_STATUS);
	mutex_unlock(&cpld->lock);

	return sprintf(buf, "%u\n", (val & 0x08) ? 1 : 0);
}
static DEVICE_ATTR_RO(psu2_good);

static ssize_t fan_status_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	u8 val;

	mutex_lock(&cpld->lock);
	val = cpld_read(cpld, CPLD_REG_FAN_STATUS);
	mutex_unlock(&cpld->lock);

	return sprintf(buf, "0x%02x\n", val);
}
static DEVICE_ATTR_RO(fan_status);

static ssize_t fan_pwm_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	u8 val;

	mutex_lock(&cpld->lock);
	val = cpld_read(cpld, CPLD_REG_FAN_PWM);
	mutex_unlock(&cpld->lock);

	return sprintf(buf, "%u\n", val);
}

static ssize_t fan_pwm_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	unsigned long val;
	int err;

	err = kstrtoul(buf, 10, &val);
	if (err)
		return err;
	if (val > 15)
		return -EINVAL;

	mutex_lock(&cpld->lock);
	cpld_write(cpld, CPLD_REG_FAN_PWM, (u8)val);
	mutex_unlock(&cpld->lock);

	return count;
}
static DEVICE_ATTR_RW(fan_pwm);

static ssize_t led_sys_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	u8 val;

	mutex_lock(&cpld->lock);
	val = cpld_read(cpld, CPLD_REG_LED_SYS);
	mutex_unlock(&cpld->lock);

	return sprintf(buf, "0x%02x\n", val);
}

static ssize_t led_sys_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	unsigned long val;
	int err;

	err = kstrtoul(buf, 0, &val);
	if (err)
		return err;

	mutex_lock(&cpld->lock);
	cpld_write(cpld, CPLD_REG_LED_SYS, (u8)val);
	mutex_unlock(&cpld->lock);

	return count;
}
static DEVICE_ATTR_RW(led_sys);

static ssize_t led_loc_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	u8 val;

	mutex_lock(&cpld->lock);
	val = cpld_read(cpld, CPLD_REG_LED_LOC);
	mutex_unlock(&cpld->lock);

	return sprintf(buf, "0x%02x\n", val);
}

static ssize_t led_loc_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	unsigned long val;
	int err;

	err = kstrtoul(buf, 0, &val);
	if (err)
		return err;

	mutex_lock(&cpld->lock);
	cpld_write(cpld, CPLD_REG_LED_LOC, (u8)val);
	mutex_unlock(&cpld->lock);

	return count;
}
static DEVICE_ATTR_RW(led_loc);

/* Generic register access for debugging */
static ssize_t reg_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	int i, len = 0;

	mutex_lock(&cpld->lock);
	for (i = 0; i < 0x20; i++) {
		if (i % 16 == 0)
			len += sprintf(buf + len, "%02x: ", i);
		len += sprintf(buf + len, "%02x ", cpld_read(cpld, i));
		if (i % 16 == 15)
			len += sprintf(buf + len, "\n");
	}
	mutex_unlock(&cpld->lock);

	return len;
}
static DEVICE_ATTR_RO(reg);

static struct attribute *cpld_attrs[] = {
	&dev_attr_version.attr,
	&dev_attr_psu_status.attr,
	&dev_attr_psu1_present.attr,
	&dev_attr_psu1_good.attr,
	&dev_attr_psu2_present.attr,
	&dev_attr_psu2_good.attr,
	&dev_attr_fan_status.attr,
	&dev_attr_fan_pwm.attr,
	&dev_attr_led_sys.attr,
	&dev_attr_led_loc.attr,
	&dev_attr_reg.attr,
	NULL,
};
ATTRIBUTE_GROUPS(cpld);

/* ── Platform driver ──────────────────────────────────────────── */

static int as5610_cpld_probe(struct platform_device *pdev)
{
	struct as5610_cpld *cpld;

	cpld = devm_kzalloc(&pdev->dev, sizeof(*cpld), GFP_KERNEL);
	if (!cpld)
		return -ENOMEM;

	mutex_init(&cpld->lock);
	cpld->pdev = pdev;

	cpld->base = devm_ioremap(&pdev->dev, CPLD_PHYS_BASE, CPLD_SIZE);
	if (!cpld->base) {
		dev_err(&pdev->dev, "Failed to map CPLD at 0x%x\n",
			CPLD_PHYS_BASE);
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, cpld);
	cpld_data = cpld;

	dev_info(&pdev->dev, "CPLD version %u at 0x%x\n",
		 cpld_read(cpld, CPLD_REG_VERSION), CPLD_PHYS_BASE);

	return 0;
}

static int as5610_cpld_remove(struct platform_device *pdev)
{
	cpld_data = NULL;
	return 0;
}

static struct platform_driver as5610_cpld_driver = {
	.probe  = as5610_cpld_probe,
	.remove = as5610_cpld_remove,
	.driver = {
		.name  = DRIVER_NAME,
		.groups = cpld_groups,
	},
};

/* Self-registering platform device (no DT match needed for eLBC) */
static struct platform_device *cpld_pdev;

static int __init as5610_cpld_init(void)
{
	int ret;

	ret = platform_driver_register(&as5610_cpld_driver);
	if (ret)
		return ret;

	cpld_pdev = platform_device_register_simple(DRIVER_NAME, -1, NULL, 0);
	if (IS_ERR(cpld_pdev)) {
		platform_driver_unregister(&as5610_cpld_driver);
		return PTR_ERR(cpld_pdev);
	}

	return 0;
}

static void __exit as5610_cpld_exit(void)
{
	platform_device_unregister(cpld_pdev);
	platform_driver_unregister(&as5610_cpld_driver);
}

module_init(as5610_cpld_init);
module_exit(as5610_cpld_exit);

MODULE_AUTHOR("EdgeNOS Contributors");
MODULE_DESCRIPTION("CPLD driver for Accton AS5610-52X");
MODULE_LICENSE("GPL");
