/*
 * ds100df410.c - TI DS100DF410 quad 10G retimer I2C driver
 *
 * Creates sysfs attributes for register access and registers
 * with the retimer_class framework.
 *
 * Based on ONL ds100df410 driver, ported to kernel 5.10.
 *
 * Copyright (C) 2013 Accton Technology Corporation.
 * Copyright (C) 2024 EdgeNOS Contributors.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/of.h>

#include "retimer_class.h"

struct ds100df410_data {
	struct i2c_client *client;
	struct retimer_dev *rdev;
};

/* Register addresses exposed via sysfs */
#define DS100DF410_REG_RESET        0x00
#define DS100DF410_REG_OVERRIDE     0x09
#define DS100DF410_REG_CDR_RST      0x0A
#define DS100DF410_REG_TAP_DEM      0x15
#define DS100DF410_REG_PFD_PRBS_DFE 0x1E
#define DS100DF410_REG_DRV_SEL_VOD  0x2D
#define DS100DF410_REG_ADAPT_EQ_SM  0x31
#define DS100DF410_REG_VEO_CLK      0x36
#define DS100DF410_REG_CHANNELS     0xFF

/* Macro to create show/store functions for a register */
#define DS100DF410_REG_ATTR(_name, _reg)				\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct ds100df410_data *data = dev_get_drvdata(dev);		\
	int val = i2c_smbus_read_byte_data(data->client, _reg);	\
	if (val < 0)							\
		return val;						\
	return sprintf(buf, "%d\n", val);				\
}									\
static ssize_t _name##_store(struct device *dev,			\
			     struct device_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	struct ds100df410_data *data = dev_get_drvdata(dev);		\
	unsigned long val;						\
	int err = kstrtoul(buf, 10, &val);				\
	if (err)							\
		return err;						\
	if (val > 255)							\
		return -EINVAL;						\
	err = i2c_smbus_write_byte_data(data->client, _reg, (u8)val);	\
	return err < 0 ? err : count;					\
}									\
static DEVICE_ATTR_RW(_name)

DS100DF410_REG_ATTR(reset,          DS100DF410_REG_RESET);
DS100DF410_REG_ATTR(override,       DS100DF410_REG_OVERRIDE);
DS100DF410_REG_ATTR(cdr_rst,        DS100DF410_REG_CDR_RST);
DS100DF410_REG_ATTR(tap_dem,        DS100DF410_REG_TAP_DEM);
DS100DF410_REG_ATTR(pfd_prbs_dfe,   DS100DF410_REG_PFD_PRBS_DFE);
DS100DF410_REG_ATTR(drv_sel_vod,    DS100DF410_REG_DRV_SEL_VOD);
DS100DF410_REG_ATTR(adapt_eq_sm,    DS100DF410_REG_ADAPT_EQ_SM);
DS100DF410_REG_ATTR(veo_clk_cdr_cap, DS100DF410_REG_VEO_CLK);
DS100DF410_REG_ATTR(channels,       DS100DF410_REG_CHANNELS);

static struct attribute *ds100df410_attrs[] = {
	&dev_attr_reset.attr,
	&dev_attr_override.attr,
	&dev_attr_cdr_rst.attr,
	&dev_attr_tap_dem.attr,
	&dev_attr_pfd_prbs_dfe.attr,
	&dev_attr_drv_sel_vod.attr,
	&dev_attr_adapt_eq_sm.attr,
	&dev_attr_veo_clk_cdr_cap.attr,
	&dev_attr_channels.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ds100df410);

static int ds100df410_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct ds100df410_data *data;
	const char *label = NULL;

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	i2c_set_clientdata(client, data);

	/* Get label from device tree if available */
	if (client->dev.of_node)
		of_property_read_string(client->dev.of_node, "label", &label);

	data->rdev = retimer_dev_register(&client->dev, label);
	if (IS_ERR(data->rdev)) {
		dev_warn(&client->dev, "Failed to register retimer class\n");
		data->rdev = NULL;
	}

	/* Create sysfs attribute group on the i2c device */
	return devm_device_add_groups(&client->dev, ds100df410_groups);
}

static int ds100df410_remove(struct i2c_client *client)
{
	struct ds100df410_data *data = i2c_get_clientdata(client);

	if (data->rdev)
		retimer_dev_unregister(data->rdev);

	return 0;
}

static const struct i2c_device_id ds100df410_id[] = {
	{ "ds100df410", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ds100df410_id);

static const struct of_device_id ds100df410_of_match[] = {
	{ .compatible = "ti,ds100df410" },
	{ }
};
MODULE_DEVICE_TABLE(of, ds100df410_of_match);

static struct i2c_driver ds100df410_driver = {
	.driver = {
		.name = "ds100df410",
		.of_match_table = ds100df410_of_match,
	},
	.probe    = ds100df410_probe,
	.remove   = ds100df410_remove,
	.id_table = ds100df410_id,
};
module_i2c_driver(ds100df410_driver);

MODULE_AUTHOR("EdgeNOS Contributors");
MODULE_DESCRIPTION("TI DS100DF410 quad 10G retimer driver");
MODULE_LICENSE("GPL");
