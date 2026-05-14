#include <bmd_config.h>
#if CDK_CONFIG_INCLUDE_BCM56840_A0 == 1

/*
 * 
 *
 * This software is governed by the Broadcom Switch APIs license.
 * This license is set out in https://raw.githubusercontent.com/Broadcom-Network-Switching-Software/OpenMDK/master/Legal/LICENSE file.
 * 
 * Copyright 2007-2020 Broadcom Inc. All rights reserved.
 */

#include <bmd/bmd.h>

#include <bmdi/arch/xgs_stp_xlate.h>

#include <cdk/chip/bcm56840_a0_defs.h>

#include "bcm56840_a0_bmd.h"
#include "bcm56840_a0_internal.h"

int
bcm56840_a0_bmd_port_stp_set(int unit, int port, bmd_stp_state_t state)
{
    int ioerr = 0;
    int lport;
    int hw_state;
    STG_TABm_t stg_tab;
    EGR_VLAN_STGm_t egr_vlan_stg;

    BMD_CHECK_UNIT(unit);
    BMD_CHECK_PORT(unit, port);

    lport = P2L(unit, port);

    if (CDK_FAILURE(bmd_xgs_stp_state_to_hw(state, &hw_state))) {
        return CDK_E_PARAM;
    }

    ioerr += READ_STG_TABm(unit, 1, &stg_tab);

    switch (lport) {
    case 0: STG_TABm_SP_TREE_PORT0f_SET(stg_tab, hw_state); break;
    case 1: STG_TABm_SP_TREE_PORT1f_SET(stg_tab, hw_state); break;
    case 2: STG_TABm_SP_TREE_PORT2f_SET(stg_tab, hw_state); break;
    case 3: STG_TABm_SP_TREE_PORT3f_SET(stg_tab, hw_state); break;
    case 4: STG_TABm_SP_TREE_PORT4f_SET(stg_tab, hw_state); break;
    case 5: STG_TABm_SP_TREE_PORT5f_SET(stg_tab, hw_state); break;
    case 6: STG_TABm_SP_TREE_PORT6f_SET(stg_tab, hw_state); break;
    case 7: STG_TABm_SP_TREE_PORT7f_SET(stg_tab, hw_state); break;
    case 8: STG_TABm_SP_TREE_PORT8f_SET(stg_tab, hw_state); break;
    case 9: STG_TABm_SP_TREE_PORT9f_SET(stg_tab, hw_state); break;
    case 10: STG_TABm_SP_TREE_PORT10f_SET(stg_tab, hw_state); break;
    case 11: STG_TABm_SP_TREE_PORT11f_SET(stg_tab, hw_state); break;
    case 12: STG_TABm_SP_TREE_PORT12f_SET(stg_tab, hw_state); break;
    case 13: STG_TABm_SP_TREE_PORT13f_SET(stg_tab, hw_state); break;
    case 14: STG_TABm_SP_TREE_PORT14f_SET(stg_tab, hw_state); break;
    case 15: STG_TABm_SP_TREE_PORT15f_SET(stg_tab, hw_state); break;
    case 16: STG_TABm_SP_TREE_PORT16f_SET(stg_tab, hw_state); break;
    case 17: STG_TABm_SP_TREE_PORT17f_SET(stg_tab, hw_state); break;
    case 18: STG_TABm_SP_TREE_PORT18f_SET(stg_tab, hw_state); break;
    case 19: STG_TABm_SP_TREE_PORT19f_SET(stg_tab, hw_state); break;
    case 20: STG_TABm_SP_TREE_PORT20f_SET(stg_tab, hw_state); break;
    case 21: STG_TABm_SP_TREE_PORT21f_SET(stg_tab, hw_state); break;
    case 22: STG_TABm_SP_TREE_PORT22f_SET(stg_tab, hw_state); break;
    case 23: STG_TABm_SP_TREE_PORT23f_SET(stg_tab, hw_state); break;
    case 24: STG_TABm_SP_TREE_PORT24f_SET(stg_tab, hw_state); break;
    case 25: STG_TABm_SP_TREE_PORT25f_SET(stg_tab, hw_state); break;
    case 26: STG_TABm_SP_TREE_PORT26f_SET(stg_tab, hw_state); break;
    case 27: STG_TABm_SP_TREE_PORT27f_SET(stg_tab, hw_state); break;
    case 28: STG_TABm_SP_TREE_PORT28f_SET(stg_tab, hw_state); break;
    case 29: STG_TABm_SP_TREE_PORT29f_SET(stg_tab, hw_state); break;
    case 30: STG_TABm_SP_TREE_PORT30f_SET(stg_tab, hw_state); break;
    case 31: STG_TABm_SP_TREE_PORT31f_SET(stg_tab, hw_state); break;
    case 32: STG_TABm_SP_TREE_PORT32f_SET(stg_tab, hw_state); break;
    case 33: STG_TABm_SP_TREE_PORT33f_SET(stg_tab, hw_state); break;
    case 34: STG_TABm_SP_TREE_PORT34f_SET(stg_tab, hw_state); break;
    case 35: STG_TABm_SP_TREE_PORT35f_SET(stg_tab, hw_state); break;
    case 36: STG_TABm_SP_TREE_PORT36f_SET(stg_tab, hw_state); break;
    case 37: STG_TABm_SP_TREE_PORT37f_SET(stg_tab, hw_state); break;
    case 38: STG_TABm_SP_TREE_PORT38f_SET(stg_tab, hw_state); break;
    case 39: STG_TABm_SP_TREE_PORT39f_SET(stg_tab, hw_state); break;
    case 40: STG_TABm_SP_TREE_PORT40f_SET(stg_tab, hw_state); break;
    case 41: STG_TABm_SP_TREE_PORT41f_SET(stg_tab, hw_state); break;
    case 42: STG_TABm_SP_TREE_PORT42f_SET(stg_tab, hw_state); break;
    case 43: STG_TABm_SP_TREE_PORT43f_SET(stg_tab, hw_state); break;
    case 44: STG_TABm_SP_TREE_PORT44f_SET(stg_tab, hw_state); break;
    case 45: STG_TABm_SP_TREE_PORT45f_SET(stg_tab, hw_state); break;
    case 46: STG_TABm_SP_TREE_PORT46f_SET(stg_tab, hw_state); break;
    case 47: STG_TABm_SP_TREE_PORT47f_SET(stg_tab, hw_state); break;
    case 48: STG_TABm_SP_TREE_PORT48f_SET(stg_tab, hw_state); break;
    case 49: STG_TABm_SP_TREE_PORT49f_SET(stg_tab, hw_state); break;
    case 50: STG_TABm_SP_TREE_PORT50f_SET(stg_tab, hw_state); break;
    case 51: STG_TABm_SP_TREE_PORT51f_SET(stg_tab, hw_state); break;
    case 52: STG_TABm_SP_TREE_PORT52f_SET(stg_tab, hw_state); break;
    case 53: STG_TABm_SP_TREE_PORT53f_SET(stg_tab, hw_state); break;
    case 54: STG_TABm_SP_TREE_PORT54f_SET(stg_tab, hw_state); break;
    case 55: STG_TABm_SP_TREE_PORT55f_SET(stg_tab, hw_state); break;
    case 56: STG_TABm_SP_TREE_PORT56f_SET(stg_tab, hw_state); break;
    case 57: STG_TABm_SP_TREE_PORT57f_SET(stg_tab, hw_state); break;
    case 58: STG_TABm_SP_TREE_PORT58f_SET(stg_tab, hw_state); break;
    case 59: STG_TABm_SP_TREE_PORT59f_SET(stg_tab, hw_state); break;
    case 60: STG_TABm_SP_TREE_PORT60f_SET(stg_tab, hw_state); break;
    case 61: STG_TABm_SP_TREE_PORT61f_SET(stg_tab, hw_state); break;
    case 62: STG_TABm_SP_TREE_PORT62f_SET(stg_tab, hw_state); break;
    case 63: STG_TABm_SP_TREE_PORT63f_SET(stg_tab, hw_state); break;
    case 64: STG_TABm_SP_TREE_PORT64f_SET(stg_tab, hw_state); break;
    case 65: STG_TABm_SP_TREE_PORT65f_SET(stg_tab, hw_state); break;
    default:
        return ioerr ? CDK_E_IO : CDK_E_PORT;
    }

    ioerr += WRITE_STG_TABm(unit, 1, stg_tab);

    /* EGR_VLAN_STG has same field layout as STG_TAB */
    CDK_MEMCPY(&egr_vlan_stg, &stg_tab, sizeof(egr_vlan_stg));
    ioerr += WRITE_EGR_VLAN_STGm(unit, 1, egr_vlan_stg);

    return ioerr ? CDK_E_IO : CDK_E_NONE;
}
#endif /* CDK_CONFIG_INCLUDE_BCM56840_A0 */
