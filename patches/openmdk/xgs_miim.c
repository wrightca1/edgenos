/*
 *
 * This software is governed by the Broadcom Switch APIs license.
 * This license is set out in https://raw.githubusercontent.com/Broadcom-Network-Switching-Software/OpenMDK/master/Legal/LICENSE file.
 * 
 * Copyright 2007-2020 Broadcom Inc. All rights reserved.
 *
 * XGS memory access functions.
 */

#include <cdk/cdk_device.h>
#include <cdk/cdk_assert.h>
#include <cdk/cdk_debug.h>

#include <cdk/arch/xgs_chip.h>
#include <cdk/arch/xgs_mem.h>
#include <cdk/arch/xgs_schan.h>
#include <cdk/arch/xgs_cmic.h>
#include <cdk/arch/xgs_miim.h>

/* MIIM trace logging removed — diagnostic done, file was filling tmpfs */

#define MIIM_PARAM_ID_OFFSET 		16
#define MIIM_PARAM_REG_ADDR_OFFSET	24
#define CMIC_MIIM_ADDRESS_5673r		0x1a4
#define CMIC_MIIM_ADDRESSr		0x4a0

int 
cdk_xgs_miim_write(int unit, uint32_t phy_addr, uint32_t reg, uint32_t val)
{
    int rv = CDK_E_NONE; 
    uint32_t polls;
    uint32_t phy_param, schan_ctrl;

    CDK_ASSERT(CDK_DEV_EXISTS(unit)); 
    
    /* Use clause 45 access if DEVAD specified */
    if (reg & 0x001f0000) {
        phy_addr |= CDK_XGS_MIIM_CLAUSE45;
        reg &= 0x001fffff;
    }

    phy_param = (phy_addr << MIIM_PARAM_ID_OFFSET) | val;

    CDK_DEBUG_MIIM
        (("cdk_xgs_miim_write[%d]: phy_addr=0x%08"PRIx32" reg_addr=%08"PRIx32" data: 0x%08"PRIx32"\n",
          unit, phy_addr, reg, val));

    /* Write parameter register and tell CMIC to start */

    /* Clause 45 support changes Clause 22 access method */
    if (CDK_XGS_FLAGS(unit) & CDK_XGS_CHIP_FLAG_CLAUSE45) {
        if (CDK_XGS_FLAGS(unit) & CDK_XGS_CHIP_FLAG_C45_5673) {
            /* Use non-standard register address */
            CDK_DEV_WRITE32(unit, CMIC_MIIM_ADDRESS_5673r, reg); 
        } else {
            CDK_DEV_WRITE32(unit, CMIC_MIIM_ADDRESSr, reg); 
        }
    } else {
        phy_param |= (reg << MIIM_PARAM_REG_ADDR_OFFSET);
    }

    CDK_DEV_WRITE32(unit, CMIC_MIIM_PARAMr, phy_param);
    CDK_DEV_WRITE32(unit, CMIC_SCHAN_CTRLr, SC_MIIM_WR_START_SET);

    /* Poll for completion */
    for (polls = 0; polls < CDK_CONFIG_MIIM_MAX_POLLS; polls++) {
        CDK_DEV_READ32(unit, CMIC_SCHAN_CTRLr, &schan_ctrl);
        if (schan_ctrl & SC_MIIM_OP_DONE_TST) {
            break;
	}
    }

    /* Check for timeout and error conditions */
    if (polls == CDK_CONFIG_MIIM_MAX_POLLS) {
	rv = -1;
        CDK_DEBUG_MIIM
            (("cdk_xgs_miim_write[%d]: Timeout at phy_addr=0x%08"PRIx32" reg_addr=%08"PRIx32"\n",
              unit, phy_addr, reg));
    }

    CDK_DEV_WRITE32(unit, CMIC_SCHAN_CTRLr, SC_MIIM_OP_DONE_CLR);

    return rv;
}

int
cdk_xgs_miim_read(int unit, uint32_t phy_addr, uint32_t reg, uint32_t *val)
{
    int rv = CDK_E_NONE; 
    uint32_t polls; 
    uint32_t phy_param, schan_ctrl, miim_data;

    CDK_ASSERT(CDK_DEV_EXISTS(unit)); 
    
    /* Use clause 45 access if DEVAD specified */
    if (reg & 0x001f0000) {
        phy_addr |= CDK_XGS_MIIM_CLAUSE45;
        reg &= 0x001fffff;
    }

    phy_param = (phy_addr << MIIM_PARAM_ID_OFFSET);

    /* Write parameter register and tell CMIC to start */

    /* Clause 45 support changes Clause 22 access method */
    if (CDK_XGS_FLAGS(unit) & CDK_XGS_CHIP_FLAG_CLAUSE45) {
        if (CDK_XGS_FLAGS(unit) & CDK_XGS_CHIP_FLAG_C45_5673) {
            /* Use non-standard register address */
            CDK_DEV_WRITE32(unit, CMIC_MIIM_ADDRESS_5673r, reg); 
        } else {
            CDK_DEV_WRITE32(unit, CMIC_MIIM_ADDRESSr, reg); 
        }
    } else {
        phy_param |= (reg << MIIM_PARAM_REG_ADDR_OFFSET);
    }

    CDK_DEV_WRITE32(unit, CMIC_MIIM_PARAMr, phy_param);
    CDK_DEV_WRITE32(unit, CMIC_SCHAN_CTRLr, SC_MIIM_RD_START_SET);

    /* Wait for completion using either the interrupt or polling method */
    for (polls = 0; polls < CDK_CONFIG_MIIM_MAX_POLLS; polls++) {
        CDK_DEV_READ32(unit, CMIC_SCHAN_CTRLr, &schan_ctrl); 
        if (schan_ctrl & SC_MIIM_OP_DONE_TST) {
            break; 
	}
    }
    
    /* Check for timeout and error conditions */
    if (polls == CDK_CONFIG_MIIM_MAX_POLLS) {
	rv = -1; 
        CDK_DEBUG_MIIM
            (("cdk_xgs_miim_read[%d]: Timeout at phy_addr=0x%08"PRIx32" reg_addr=%08"PRIx32"\n",
              unit, phy_addr, reg));
    }

    CDK_DEV_WRITE32(unit, CMIC_SCHAN_CTRLr, SC_MIIM_OP_DONE_CLR);

    if (rv >= 0) {
        CDK_DEV_READ32(unit, CMIC_MIIM_READ_DATAr, &miim_data);
        *val = miim_data;
        CDK_DEBUG_MIIM
            (("cdk_xgs_miim_read[%d]: phy_addr=0x%08"PRIx32" reg_addr=%08"PRIx32" data: 0x%08"PRIx32"\n",
              unit, phy_addr, reg, miim_data));
    }

    return rv;
}
