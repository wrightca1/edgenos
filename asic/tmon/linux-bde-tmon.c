/*
 * linux-bde-tmon.c - Broadcom BCM5684x on-die temperature monitor
 *
 * Exposes the chip's internal temperature sensor(s) as a Linux hwmon
 * device. Trident/Trident+ chips have 8 thermal sensors scattered
 * across the die — the driver reads all 8 each query and returns the
 * MAX as the chip temperature.
 *
 * MMIO register map (offsets from chip BAR0):
 *   +0x88..0x8B  TMON_CTRL
 *                  bit 0  : primary enable
 *                  bit 16 : secondary enable
 *                  bits 1-2,17 cleared to 0
 *                  bits 16-17 readback: 01 = enabled & ready
 *   +0x174..0x177 ENDIAN_CTRL (per-byte 0x01 pattern → byte-swap mode)
 *   +0x90, +0x94                  TMON_SENSOR[0..1]
 *   +0x95C, +0x960, +0x964, +0x968 TMON_SENSOR[2..5]
 *   +0xE40, +0xE44                TMON_SENSOR[6..7]
 *
 * Temperature formula (per-sensor):
 *   raw  = readl(sensor_offset) & 0x3FF       (10-bit ADC)
 *   if (raw * 542 < 410001)
 *       temp_mC = 410000 - raw * 542
 *   else
 *       temp_mC = 150000 (capped)
 *
 * Reverse-engineered from Cumulus linux-bde-tmon.ko (Ghidra decomp,
 * cumulus_baseline_2013/ghidra-analysis/linux-bde-tmon.ko_decompile.c),
 * authored by JR Rivers <jrrivers@cumulusnetworks.com>, GPL.
 *
 * Copyright (C) 2014 Cumulus Networks (original).
 * Copyright (C) 2024 EdgeNOS Contributors.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>

/*
 * hwmon names must match [A-Za-z0-9_]+ — no hyphens allowed.
 * The module is still installed as linux-bde-tmon.ko for backward
 * naming consistency with Cumulus, but the hwmon `name` attribute
 * uses the underscore form.
 */
#define DRIVER_NAME "bde_tmon"

#define PCI_VENDOR_ID_BROADCOM_NETXTREME 0x14e4

/* Trident family device IDs that have an on-die TMON */
static const struct pci_device_id trident_devid[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM_NETXTREME, 0xb846) }, /* BCM56846 */
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM_NETXTREME, 0xb845) }, /* BCM56845 */
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM_NETXTREME, 0xb840) }, /* BCM56840 */
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM_NETXTREME, 0xb842) }, /* BCM56842 */
	{ 0, }
};

#define TMON_REG_CTRL      0x088
#define TMON_REG_ENDIAN    0x174

#define TMON_CTRL_ENABLE   0x00010001	/* bit 0 + bit 16 */
#define TMON_CTRL_CLEAR    0x00020007	/* bits 0-2, 17 cleared first */
#define TMON_CTRL_READY    0x00010000	/* readback: bit 16 set, bit 17 clear */
#define TMON_CTRL_READY_M  0x00030000

#define TMON_TEMP_MAX_MC   150000	/* 150°C — sensor cap */
#define TMON_DEFAULT_MAX   105000	/* 105°C — default alarm */
#define TMON_DEFAULT_HYST   50000	/* 50°C  — default hysteresis */

/* Eight sensor offsets, scattered across the die */
static const u32 trident_sensor_offsets[] = {
	0x090, 0x094, 0x95c, 0x960, 0x964, 0x968, 0xe40, 0xe44,
};
#define TMON_NUM_SENSORS ARRAY_SIZE(trident_sensor_offsets)

struct bde_tmon {
	struct pci_dev *pdev;
	void __iomem *base;
	struct device *hdev;
	struct mutex lock;
	bool byteswap;
	u32 alarm_max_mc;
	u32 alarm_hyst_mc;
};

/*
 * Read the chip's endian-control register. On BE host with BE chip,
 * each byte of this 32-bit register has bit 0 set (0x01010101). In
 * that case the chip's data path is byte-swapping for us, so we need
 * to swap back. On LE host (or if chip is in LE mode), no swap.
 */
static bool tmon_detect_byteswap(void __iomem *base)
{
	u32 val = ioread32(base + TMON_REG_ENDIAN);

	return (val & 0x01010101) != 0;
}

static u32 tmon_reg_read(struct bde_tmon *t, u32 off)
{
	u32 v = ioread32(t->base + off);
	return t->byteswap ? swab32(v) : v;
}

static void tmon_reg_write(struct bde_tmon *t, u32 off, u32 val)
{
	iowrite32(t->byteswap ? swab32(val) : val, t->base + off);
}

static void tmon_enable_trident(struct bde_tmon *t)
{
	u32 val;

	val = tmon_reg_read(t, TMON_REG_CTRL);
	val = (val & ~TMON_CTRL_CLEAR) | TMON_CTRL_ENABLE;
	tmon_reg_write(t, TMON_REG_CTRL, val);
}

static bool tmon_ready(struct bde_tmon *t)
{
	u32 val = tmon_reg_read(t, TMON_REG_CTRL);

	return (val & TMON_CTRL_READY_M) == TMON_CTRL_READY;
}

/*
 * Convert a 10-bit raw ADC reading to milli-Celsius using the chip's
 * calibration formula. Returns TMON_TEMP_MAX_MC on out-of-range.
 */
static u32 tmon_raw_to_mc(u32 raw)
{
	u32 r = raw & 0x3ff;
	u32 mc;

	if (r * 542u >= 410001u)
		return TMON_TEMP_MAX_MC;

	mc = 410000u - r * 542u;
	if (mc > TMON_TEMP_MAX_MC)
		mc = TMON_TEMP_MAX_MC;
	return mc;
}

static u32 tmon_read_max_mc(struct bde_tmon *t)
{
	u32 hottest = 0;
	int i;

	if (!tmon_ready(t)) {
		tmon_enable_trident(t);
		return TMON_TEMP_MAX_MC;
	}

	for (i = 0; i < TMON_NUM_SENSORS; i++) {
		u32 raw = tmon_reg_read(t, trident_sensor_offsets[i]);
		u32 mc  = tmon_raw_to_mc(raw);

		if (mc > hottest)
			hottest = mc;
	}
	return hottest;
}

/* ---- hwmon sysfs ---- */

static ssize_t temp1_input_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct bde_tmon *t = dev_get_drvdata(dev);
	u32 mc;

	mutex_lock(&t->lock);
	mc = tmon_read_max_mc(t);
	mutex_unlock(&t->lock);
	return sprintf(buf, "%u\n", mc);
}

static ssize_t temp1_max_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct bde_tmon *t = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", t->alarm_max_mc);
}

static ssize_t temp1_max_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct bde_tmon *t = dev_get_drvdata(dev);
	u32 v;

	if (kstrtou32(buf, 10, &v))
		return -EINVAL;
	t->alarm_max_mc = v;
	return count;
}

static ssize_t temp1_max_hyst_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct bde_tmon *t = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", t->alarm_hyst_mc);
}

static ssize_t temp1_max_hyst_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct bde_tmon *t = dev_get_drvdata(dev);
	u32 v;

	if (kstrtou32(buf, 10, &v))
		return -EINVAL;
	t->alarm_hyst_mc = v;
	return count;
}

/*
 * Do NOT define our own `name` attribute. hwmon_device_register_with_groups()
 * auto-creates `name` from the name argument (DRIVER_NAME); on Linux 6.x the
 * hwmon core does this unconditionally, so a driver-provided `name` collides
 * -> "cannot create duplicate filename .../hwmon/hwmonN/name" / -EEXIST and the
 * whole device fails to register. Let the core own `name`.
 */
static DEVICE_ATTR_RO(temp1_input);
static DEVICE_ATTR_RW(temp1_max);
static DEVICE_ATTR_RW(temp1_max_hyst);

static struct attribute *tmon_attrs[] = {
	&dev_attr_temp1_input.attr,
	&dev_attr_temp1_max.attr,
	&dev_attr_temp1_max_hyst.attr,
	NULL,
};
ATTRIBUTE_GROUPS(tmon);

/* ---- PCI probe ---- */

static struct bde_tmon *the_tmon;

static int tmon_attach(struct pci_dev *pdev)
{
	struct bde_tmon *t;
	resource_size_t phys, len;

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;

	t->pdev = pdev;
	mutex_init(&t->lock);
	t->alarm_max_mc = TMON_DEFAULT_MAX;
	t->alarm_hyst_mc = TMON_DEFAULT_HYST;

	/*
	 * BAR0 may already be claimed by linux-kernel-bde. We don't try
	 * to claim it — we just remap the same physical region for our
	 * read-only sensor accesses. This is safe because the TMON
	 * register block at +0x88 and the sensor regs at +0x90..+0xe44
	 * are not touched by BDE during normal operation.
	 */
	phys = pci_resource_start(pdev, 0);
	len  = pci_resource_len(pdev, 0);
	if (!phys || !len) {
		dev_err(&pdev->dev, "BAR0 not assigned\n");
		kfree(t);
		return -ENODEV;
	}

	t->base = ioremap(phys, len);
	if (!t->base) {
		dev_err(&pdev->dev, "ioremap BAR0 failed\n");
		kfree(t);
		return -ENOMEM;
	}

	t->byteswap = tmon_detect_byteswap(t->base);
	tmon_enable_trident(t);

	t->hdev = hwmon_device_register_with_groups(&pdev->dev,
						    DRIVER_NAME, t,
						    tmon_groups);
	if (IS_ERR(t->hdev)) {
		int err = PTR_ERR(t->hdev);
		dev_err(&pdev->dev, "hwmon register failed: %d\n", err);
		iounmap(t->base);
		kfree(t);
		return err;
	}

	dev_info(&pdev->dev,
		 "BCM56%03x TMON enabled (byteswap=%d, 8 sensors, hwmon=%s)\n",
		 pdev->device & 0xfff, t->byteswap,
		 dev_name(t->hdev));

	the_tmon = t;
	return 0;
}

static void tmon_detach(struct bde_tmon *t)
{
	if (!t)
		return;
	hwmon_device_unregister(t->hdev);
	iounmap(t->base);
	kfree(t);
}

static int __init bde_tmon_init(void)
{
	struct pci_dev *pdev = NULL;
	const struct pci_device_id *id;
	int found = 0;
	int rc = 0;

	for (id = trident_devid; id->vendor; id++) {
		pdev = pci_get_device(id->vendor, id->device, NULL);
		if (pdev) {
			rc = tmon_attach(pdev);
			pci_dev_put(pdev);
			if (rc == 0) {
				found = 1;
				break;
			}
		}
	}

	if (!found) {
		pr_info(DRIVER_NAME ": no Trident-class device found\n");
		return -ENODEV;
	}
	return 0;
}

static void __exit bde_tmon_exit(void)
{
	tmon_detach(the_tmon);
	the_tmon = NULL;
}

module_init(bde_tmon_init);
module_exit(bde_tmon_exit);

MODULE_DESCRIPTION("Thermal monitoring driver for Broadcom Trident SoCs");
MODULE_AUTHOR("EdgeNOS Contributors (RE'd from Cumulus original by JR Rivers)");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, trident_devid);
