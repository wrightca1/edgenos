/*
 * bde_interface.c - OpenMDK ASIC interface for switchd
 *
 * Initializes BCM56846 via OpenMDK CDK/BMD using /dev/mem mmap.
 * No kernel BDE module required for basic L2 switching.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>
#include <sys/mman.h>
#include <stdint.h>

#include "switchd.h"

/* CDK headers */
#include <cdk_config.h>
#include <cdk/cdk_device.h>
#include <cdk/cdk_error.h>
#include <cdk/chip/bcm56840_a0_defs.h>

/* BMD headers */
#include <bmd_config.h>
#include <bmd/bmd.h>
#include <bmd/bmd_phy_ctrl.h>

/* PHY headers */
#include <phy_config.h>
#include <phy/phy_drvlist.h>

/* BCM56846 on AS5610-52X */
#define BCM56846_VENDOR_ID  0x14e4
#define BCM56846_DEVICE_ID  0xb846
#define BCM56846_REVISION   0x02
#define BCM56846_BAR0_ADDR  0xa0000000
#define BCM56846_BAR0_SIZE  (256 * 1024)

/* BMD/PHY sleep function */
int
_usleep(uint32_t usecs)
{
    return usleep(usecs);
}

static int mem_fd = -1;
static uint32_t *cmic_base;

int bde_open(void)
{
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        syslog(LOG_ERR, "Cannot open /dev/mem: %s", strerror(errno));
        return -1;
    }

    cmic_base = mmap(NULL, BCM56846_BAR0_SIZE,
                     PROT_READ | PROT_WRITE, MAP_SHARED,
                     mem_fd, BCM56846_BAR0_ADDR);
    if (cmic_base == MAP_FAILED) {
        syslog(LOG_ERR, "Cannot mmap BAR0 at 0x%x: %s",
               BCM56846_BAR0_ADDR, strerror(errno));
        close(mem_fd);
        mem_fd = -1;
        return -1;
    }

    /* Read CMIC device ID to verify access */
    uint32_t cmic_devid = cmic_base[0x178 / 4];
    syslog(LOG_INFO, "BDE: mapped BAR0 at 0x%x, CMIC_DEV_REV_ID=0x%08x",
           BCM56846_BAR0_ADDR, cmic_devid);

    return 0;
}

void bde_close(void)
{
    if (cmic_base && cmic_base != MAP_FAILED)
        munmap(cmic_base, BCM56846_BAR0_SIZE);
    if (mem_fd >= 0)
        close(mem_fd);

    cmic_base = NULL;
    mem_fd = -1;
}

/*
 * Get bus endian flags for P2020 (big-endian PPC with PCI).
 */
static uint32_t
bus_flags(void)
{
    uint32_t flags = CDK_DEV_MBUS_PCI;
#if SYS_BE_PIO == 1
    flags |= CDK_DEV_BE_PIO;
#endif
#if SYS_BE_PACKET == 1
    flags |= CDK_DEV_BE_PACKET;
#endif
#if SYS_BE_OTHER == 1
    flags |= CDK_DEV_BE_OTHER;
#endif
    return flags;
}

int cdk_init(void)
{
    cdk_dev_id_t dev_id;
    cdk_dev_vectors_t dv;
    int unit;

    if (!cmic_base) {
        syslog(LOG_ERR, "CDK: CMIC not mapped");
        return -1;
    }

    memset(&dev_id, 0, sizeof(dev_id));
    dev_id.vendor_id = BCM56846_VENDOR_ID;
    dev_id.device_id = BCM56846_DEVICE_ID;
    dev_id.revision = BCM56846_REVISION;

    memset(&dv, 0, sizeof(dv));
    dv.base_addr = cmic_base;

    unit = cdk_dev_create(&dev_id, &dv, bus_flags());
    if (unit < 0) {
        syslog(LOG_ERR, "CDK: cdk_dev_create failed: %s (%d)",
               CDK_ERRMSG(unit), unit);
        return -1;
    }

    syslog(LOG_INFO, "CDK: unit %d created (BCM%04x rev %02x)",
           unit, dev_id.device_id, dev_id.revision);

    /* AS5610-52X uses 156.25 MHz LCPLL reference clock */
    CDK_CHIP_CONFIG_SET(unit, DCFG_LCPLL_156);

    /* Initialize PHY probe */
#if BMD_CONFIG_INCLUDE_PHY == 1
    bmd_phy_probe_init(bmd_phy_probe_default, bmd_phy_drv_list);
#endif

    /* Attach BMD driver */
    int rv = bmd_attach(unit);
    if (rv < 0) {
        syslog(LOG_ERR, "CDK: bmd_attach failed: %d", rv);
        return -1;
    }

    switchd.unit = unit;
    syslog(LOG_INFO, "CDK: BMD attached to unit %d", unit);
    return 0;
}

int bmd_init_all(void)
{
    int rv;

    syslog(LOG_INFO, "BMD: resetting ASIC on unit %d", switchd.unit);

    rv = bmd_reset(switchd.unit);
    if (rv < 0) {
        syslog(LOG_ERR, "BMD: bmd_reset failed: %d", rv);
        return -1;
    }
    syslog(LOG_INFO, "BMD: ASIC reset complete (CPS, SBUS ring map, PLLs)");

    syslog(LOG_INFO, "BMD: initializing ASIC on unit %d", switchd.unit);

    rv = bmd_init(switchd.unit);
    if (rv < 0) {
        syslog(LOG_ERR, "BMD: bmd_init failed: %d", rv);
        return -1;
    }

    syslog(LOG_INFO, "BMD: ASIC initialized (port blocks, SerDes)");
    return 0;
}

int bmd_switching_init_all(void)
{
    int rv;

    syslog(LOG_INFO, "BMD: initializing L2 switching on unit %d", switchd.unit);

    rv = bmd_switching_init(switchd.unit);
    if (rv < 0) {
        syslog(LOG_ERR, "BMD: bmd_switching_init failed: %d", rv);
        return -1;
    }

    syslog(LOG_INFO, "BMD: L2 switching initialized (VLAN 1, MAC learning)");
    return 0;
}
