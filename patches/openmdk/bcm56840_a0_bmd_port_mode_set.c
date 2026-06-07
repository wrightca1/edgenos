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
#include <bmd/bmd_device.h>

#include <bmdi/bmd_port_mode.h>

#include <cdk/chip/bcm56840_a0_defs.h>
#include <cdk/arch/xgs_chip.h>
#include <cdk/cdk_debug.h>

#include "bcm56840_a0_bmd.h"
#include "bcm56840_a0_internal.h"

#define DRAIN_WAIT_MSEC                 500

/* Supported HiGig encapsulations */
#define HG_FLAGS        (BMD_PORT_MODE_F_HIGIG | BMD_PORT_MODE_F_HIGIG2)

int
bcm56840_a0_bmd_port_mode_set(int unit, int port, 
                              bmd_port_mode_t mode, uint32_t flags)
{
    int ioerr = 0;
    int rv = CDK_E_NONE;
    int mac_lb = (flags & BMD_PORT_MODE_F_MAC_LOOPBACK) ? 1 : 0;
    int phy_lb = (flags & BMD_PORT_MODE_F_PHY_LOOPBACK) ? 1 : 0;
    int duplex = 1;
    uint32_t speed = 1000;
    int sp_sel = COMMAND_CONFIG_SPEED_1000;
    int pref_intf = 0;
    int cnt, num_lanes;
    int lport;
    uint32_t pbm, clr_mask;
    uint32_t speed_max;
    EPC_LINK_BMAPm_t epc_link;
    FLUSH_CONTROLr_t flush_ctrl;
    XLP_TXFIFO_CELL_CNTr_t cell_cnt;
    COMMAND_CONFIGr_t command_cfg;

    BMD_CHECK_UNIT(unit);
    BMD_CHECK_PORT(unit, port);

    speed_max = bcm56840_a0_port_speed_max(unit, port);
    num_lanes = bcm56840_a0_port_num_lanes(unit, port);

    if (flags & HG_FLAGS) {
        pref_intf = BMD_PHY_IF_HIGIG;
    }

    if (BMD_PORT_PROPERTIES(unit, port) & (BMD_PORT_HG | BMD_PORT_XE)) {
        switch (mode) {
        case bmdPortModeAuto:
        case bmdPortModeDisabled:
            speed = speed_max;
            break;
        case bmdPortMode10000fd:
        case bmdPortMode10000XFI:
        case bmdPortMode10000KR:
            speed = 10000;
            break;
        case bmdPortMode10000SFI:
            speed = 10000;
            pref_intf = BMD_PHY_IF_SFI;
            break;
        case bmdPortMode20000fd:
            speed = 20000;
            break;
        case bmdPortMode40000fd:
            speed = 40000;
            /* On the AS5610 the 40G ports are QSFP optical (40GBASE-SR4),
             * so default the generic 40G mode to the SR line interface, which
             * makes the Warpcore program the SFP_OPT_SR4 firmware mode for the
             * optical link. HiGig 40G ports (if any) keep BMD_PHY_IF_HIGIG
             * set above; copper DAC/KR use bmdPortMode40000CR/KR explicitly. */
            if (!(flags & HG_FLAGS)) {
                pref_intf = BMD_PHY_IF_SR;
            }
            break;
        case bmdPortMode40000KR:
            speed = 40000;
            pref_intf = BMD_PHY_IF_KR;
            break;
        case bmdPortMode40000CR:
            speed = 40000;
            pref_intf = BMD_PHY_IF_CR;
            break;
#if BMD_CONFIG_INCLUDE_HIGIG == 1
        case bmdPortMode12000fd:
            if (flags & HG_FLAGS) {
                speed = 12000;
            }
            break;
        case bmdPortMode13000fd:
            if (flags & HG_FLAGS) {
                speed = 13000;
            }
            break;
        case bmdPortMode15000fd:
            if (flags & HG_FLAGS) {
                speed = 15000;
            }
            break;
        case bmdPortMode16000fd:
            if (flags & HG_FLAGS) {
                speed = 16000;
            }
            break;
        case bmdPortMode30000fd:
            if (flags & HG_FLAGS) {
                speed = 30000;
            }
            break;
#endif
        default:
            break;
        }
    }

    /* If no XAUI mode was selected, check SerDes modes */
    if (speed == 1000) {
        if (num_lanes > 1) {
            return CDK_E_PARAM;
        }
        switch (mode) {
        case bmdPortMode10hd:
        case bmdPortMode100hd:
        case bmdPortMode1000hd:
            duplex = 0;
            break;
        default:
            break;
        }
        switch (mode) {
        case bmdPortMode10fd:
        case bmdPortMode10hd:
            speed = 10;
            sp_sel = COMMAND_CONFIG_SPEED_10;
            break;
        case bmdPortMode100fd:
        case bmdPortMode100hd:
            speed = 100;
            sp_sel = COMMAND_CONFIG_SPEED_100;
            break;
        case bmdPortMode2500fd:
            speed = 2500;
            sp_sel = COMMAND_CONFIG_SPEED_2500;
        case bmdPortMode1000fd:
        case bmdPortMode1000hd:
        case bmdPortModeAuto:
            break;
        case bmdPortModeDisabled:
            break;
        default:
            return CDK_E_PARAM;
        }
    }

    if (speed > speed_max) {
        return CDK_E_PARAM;
    }

    if (BMD_PORT_PROPERTIES(unit, port) & BMD_PORT_GE) {
        if (speed > 1000) {
            return CDK_E_PARAM;
        }
    } else {
        if (num_lanes > 1 && speed < 1000) {
            return CDK_E_PARAM;
        }
    }

    lport = P2L(unit, port);

    if ((flags & BMD_PORT_MODE_F_INTERNAL) == 0) {

        /* Set preferred line interface */
        bmd_phy_line_interface_set(unit, port, pref_intf);

        /* Stop CPU and MMU from scheduling packets to the port */
        BMD_PORT_STATUS_CLR(unit, port, BMD_PST_LINK_UP);
        ioerr += READ_EPC_LINK_BMAPm(unit, 0, &epc_link);
        clr_mask = LSHIFT32(1, lport & 0x1f);
        if (lport >= 64) {
            pbm = EPC_LINK_BMAPm_PORT_BITMAP_W2f_GET(epc_link);
            pbm &= ~clr_mask;
            EPC_LINK_BMAPm_PORT_BITMAP_W2f_SET(epc_link, pbm);
        } else if (lport >= 32) {
            pbm = EPC_LINK_BMAPm_PORT_BITMAP_W1f_GET(epc_link);
            pbm &= ~clr_mask;
            EPC_LINK_BMAPm_PORT_BITMAP_W1f_SET(epc_link, pbm);
        } else {
            pbm = EPC_LINK_BMAPm_PORT_BITMAP_W0f_GET(epc_link);
            pbm &= ~clr_mask;
            EPC_LINK_BMAPm_PORT_BITMAP_W0f_SET(epc_link, pbm);
        }
        ioerr += WRITE_EPC_LINK_BMAPm(unit, 0, epc_link);

        /* Drain all packets from the Tx pipeline */
        ioerr += READ_FLUSH_CONTROLr(unit, port, &flush_ctrl);
        FLUSH_CONTROLr_FLUSHf_SET(flush_ctrl, 1);
        ioerr += WRITE_FLUSH_CONTROLr(unit, port, flush_ctrl);
        cnt = DRAIN_WAIT_MSEC / 10;
        while (--cnt >= 0) {
            ioerr += READ_XLP_TXFIFO_CELL_CNTr(unit, port, &cell_cnt);
            if (XLP_TXFIFO_CELL_CNTr_GET(cell_cnt) == 0) {
                break;
            }
            BMD_SYS_USLEEP(10000);
        }
        if (cnt < 0) {
            CDK_WARN(("bcm56840_a0_bmd_port_mode_set[%d]: "
                      "drain failed on port %d\n", unit, port));
        }
        FLUSH_CONTROLr_FLUSHf_SET(flush_ctrl, 0);
        ioerr += WRITE_FLUSH_CONTROLr(unit, port, flush_ctrl);

#if BMD_CONFIG_INCLUDE_HIGIG == 1
        if (BMD_PORT_PROPERTIES(unit, port) & (BMD_PORT_HG | BMD_PORT_XE)) {
            bmd_port_mode_t cur_mode;
            uint32_t cur_flags;

            /*
             * If HiGig/Ethernet encapsulation changes, we need 
             * to reinitialize the warpcore.
             */
            rv = bcm56840_a0_bmd_port_mode_get(unit, port, 
                                               &cur_mode, &cur_flags);
            if (CDK_SUCCESS(rv) && 
                ((flags ^ cur_flags) & HG_FLAGS)) {
                if (flags & HG_FLAGS) {
                    BMD_PORT_PROPERTIES(unit, port) = BMD_PORT_HG;
                } else {
                    BMD_PORT_PROPERTIES(unit, port) = BMD_PORT_XE;
                }
                rv = bcm56840_a0_warpcore_phy_init(unit, port);
            }
        }
#endif
    }

    /* Update PHYs before MAC */
    if (CDK_SUCCESS(rv)) {
        rv = bmd_port_mode_to_phy(unit, port, mode, flags, speed, duplex);
    }

    /* Let PHYs know that we disable the MAC */
    if (CDK_SUCCESS(rv)) {
        rv = bmd_phy_notify_mac_enable(unit, port, 0);
    }

    /* Reset the MAC */
    ioerr += READ_COMMAND_CONFIGr(unit, port, &command_cfg);
    COMMAND_CONFIGr_SW_RESETf_SET(command_cfg, 1);
    ioerr += WRITE_COMMAND_CONFIGr(unit, port, command_cfg);

    /* Disable MACs (Rx only) */
    if (BMD_PORT_PROPERTIES(unit, port) & (BMD_PORT_HG | BMD_PORT_XE)) {
        XMAC_CTRLr_t mac_ctrl;

        ioerr += READ_XMAC_CTRLr(unit, port, &mac_ctrl);
        XMAC_CTRLr_RX_ENf_SET(mac_ctrl, 0);
        ioerr += WRITE_XMAC_CTRLr(unit, port, mac_ctrl);
    }
    ioerr += READ_COMMAND_CONFIGr(unit, port, &command_cfg);
    COMMAND_CONFIGr_RX_ENAf_SET(command_cfg, 0);
    ioerr += WRITE_COMMAND_CONFIGr(unit, port, command_cfg);

    if (mode == bmdPortModeDisabled) {
        BMD_PORT_STATUS_SET(unit, port, BMD_PST_FORCE_LINK | BMD_PST_DISABLED);
    } else {
        BMD_PORT_STATUS_CLR(unit, port, BMD_PST_DISABLED);
        if (BMD_PORT_PROPERTIES(unit, port) & (BMD_PORT_HG | BMD_PORT_XE)) {
            int hg, hg2;
            int sub_port, mac_mode;
            int old_phy_mode, phy_mode;
            int xlgmii_align;
            XMAC_CTRLr_t xmac_ctrl;
            XMAC_MODEr_t xmac_mode;
            XLPORT_MODE_REGr_t xlport_mode;
            XLPORT_XMAC_CONTROLr_t xlport_ctrl;
            PORT_TABm_t port_tab;
            EGR_PORTm_t egr_port;
            EGR_ING_PORTm_t egr_ing_port;
            XLPORT_CONFIGr_t xlport_cfg;
            ICONTROL_OPCODE_BITMAPm_t opcode_bmap;
            EGR_VLAN_CONTROL_3r_t vctrl3;
            XMAC_EEE_CTRLr_t xmac_eee_ctrl;

            /* Update XLPORT mode according to speed */
            ioerr += READ_XLPORT_MODE_REGr(unit, &xlport_mode, port);
            old_phy_mode = XLPORT_MODE_REGr_PHY_PORT_MODEf_GET(xlport_mode);
            phy_mode = 2;
            if (speed > 20000) {
                phy_mode = 0;
            } else if (speed > 10000) {
                phy_mode = 1;
            }
            XLPORT_MODE_REGr_PHY_PORT_MODEf_SET(xlport_mode, phy_mode);
            mac_mode = 0;
            if (speed < 10000) {
                mac_mode = 1;
            }
            sub_port = XLPORT_SUBPORT(port);
            if (sub_port == 0) {
                XLPORT_MODE_REGr_PORT0_MAC_MODEf_SET(xlport_mode, mac_mode);
            } else if (sub_port == 1) {
                XLPORT_MODE_REGr_PORT1_MAC_MODEf_SET(xlport_mode, mac_mode);
            } else if (sub_port == 2) {
                XLPORT_MODE_REGr_PORT2_MAC_MODEf_SET(xlport_mode, mac_mode);
            } else if (sub_port == 3) {
                XLPORT_MODE_REGr_PORT3_MAC_MODEf_SET(xlport_mode, mac_mode);
            }
            ioerr += WRITE_XLPORT_MODE_REGr(unit, xlport_mode, port);

            /* Reset and initialize MAC if core mode changed */
            if (phy_mode != old_phy_mode) {
                ioerr += READ_XLPORT_XMAC_CONTROLr(unit, &xlport_ctrl, port);
                XLPORT_XMAC_CONTROLr_XMAC_RESETf_SET(xlport_ctrl, 1);
                ioerr += WRITE_XLPORT_XMAC_CONTROLr(unit, xlport_ctrl, port);
                BMD_SYS_USLEEP(1000);
                XLPORT_XMAC_CONTROLr_XMAC_RESETf_SET(xlport_ctrl, 0);
                ioerr += WRITE_XLPORT_XMAC_CONTROLr(unit, xlport_ctrl, port);
                BMD_SYS_USLEEP(1000);
                ioerr += bcm56840_a0_xport_init(unit, port);
                /* Reinitialize XLPORT neighbor if 2-lane mode */
                if (num_lanes == 2) {
                    int np = port + 2;
                    if (XLPORT_SUBPORT(port) == 2) {
                        np = port - 2;
                    }
                    /* Align PHY speed to new core mode */
                    if (CDK_SUCCESS(rv)) {
                        rv = bcm56840_a0_xport_init(unit, np);
                    }
                    if (CDK_SUCCESS(rv)) {
                        rv = bmd_phy_speed_set(unit, np, speed);
                    }
                }
            }

            /* Set encapsulation and alignment */
            hg = hg2 = 0;
            xlgmii_align = 0;
            if (speed >= 30000) {
                xlgmii_align = 1;
            }
#if BMD_CONFIG_INCLUDE_HIGIG == 1
            if (flags & HG_FLAGS) {
                hg = 1;
                xlgmii_align = 0;
                if (flags & BMD_PORT_MODE_F_HIGIG2) {
                    hg2 = 1;
                }
            }
#endif
            ioerr += READ_XMAC_MODEr(unit, port, &xmac_mode);
            ioerr += READ_PORT_TABm(unit, lport, &port_tab);
            ioerr += READ_EGR_PORTm(unit, lport, &egr_port);
            ioerr += READ_EGR_ING_PORTm(unit, lport, &egr_ing_port);
            ioerr += READ_XLPORT_CONFIGr(unit, port, &xlport_cfg);
            /* MAC header mode */
            XMAC_MODEr_HDR_MODEf_SET(xmac_mode, hg2 ? 2 : hg);
            /* Set IEEE vs HiGig */        
            PORT_TABm_PORT_TYPEf_SET(port_tab, hg);
            EGR_PORTm_PORT_TYPEf_SET(egr_port, hg);
            EGR_ING_PORTm_PORT_TYPEf_SET(egr_ing_port, hg);
            XLPORT_CONFIGr_HIGIG_MODEf_SET(xlport_cfg, hg);
            ICONTROL_OPCODE_BITMAPm_SET(opcode_bmap, 0, hg ? 0x1 : 0x0);
            /* Set HiGig vs. HiGig2 */
            PORT_TABm_HIGIG2f_SET(port_tab, hg2);
            EGR_PORTm_HIGIG2f_SET(egr_port, hg2);
            EGR_ING_PORTm_HIGIG2f_SET(egr_ing_port, hg2);
            XLPORT_CONFIGr_HIGIG2_MODEf_SET(xlport_cfg, hg2);
            ioerr += WRITE_XMAC_MODEr(unit, port, xmac_mode);
            ioerr += WRITE_PORT_TABm(unit, lport, port_tab);
            ioerr += WRITE_EGR_PORTm(unit, lport, egr_port);
            ioerr += WRITE_EGR_ING_PORTm(unit, lport, egr_ing_port);
            ioerr += WRITE_XLPORT_CONFIGr(unit, port, xlport_cfg);
            ioerr += WRITE_ICONTROL_OPCODE_BITMAPm(unit, lport, opcode_bmap);

            /* HiGig ports require special egress tag action */
            ioerr += READ_EGR_VLAN_CONTROL_3r(unit, lport, &vctrl3);
            EGR_VLAN_CONTROL_3r_TAG_ACTION_PROFILE_PTRf_SET(vctrl3, hg ? 1 : 0);
            ioerr += WRITE_EGR_VLAN_CONTROL_3r(unit, lport, vctrl3);

            /* Configure 10G MAC */
            ioerr += READ_XMAC_CTRLr(unit, port, &xmac_ctrl);
            XMAC_CTRLr_LINE_LOCAL_LPBKf_SET(xmac_ctrl, mac_lb);
            XMAC_CTRLr_XLGMII_ALIGN_ENBf_SET(xmac_ctrl, xlgmii_align);
            ioerr += WRITE_XMAC_CTRLr(unit, port, xmac_ctrl);
            
            /* Configure EEE */
            ioerr += READ_XMAC_EEE_CTRLr(unit, port, &xmac_eee_ctrl);            
            XMAC_EEE_CTRLr_EEE_ENf_SET(xmac_eee_ctrl, 0);
            ioerr += WRITE_XMAC_EEE_CTRLr(unit, port, xmac_eee_ctrl);                            
            if (flags & BMD_PORT_MODE_F_EEE) {
                /* Enable IEEE 802.3az EEE */
                XMAC_EEE_CTRLr_EEE_ENf_SET(xmac_eee_ctrl, 1);
                ioerr += WRITE_XMAC_EEE_CTRLr(unit, port, xmac_eee_ctrl);                            
            }
        }
        if (speed < 10000) {
            /* Set speed and duplex */
            ioerr += READ_COMMAND_CONFIGr(unit, port, &command_cfg);
            COMMAND_CONFIGr_ETH_SPEEDf_SET(command_cfg, sp_sel);
            COMMAND_CONFIGr_HD_ENAf_SET(command_cfg, !duplex);
            ioerr += WRITE_COMMAND_CONFIGr(unit, port, command_cfg);

            /* Set MAC loopback mode */
            ioerr += READ_COMMAND_CONFIGr(unit, port, &command_cfg);
            COMMAND_CONFIGr_LOOP_ENAf_SET(command_cfg, mac_lb);
            ioerr += WRITE_COMMAND_CONFIGr(unit, port, command_cfg);

            /* Bring the MAC out of reset */
            ioerr += READ_COMMAND_CONFIGr(unit, port, &command_cfg);
            COMMAND_CONFIGr_SW_RESETf_SET(command_cfg, 0);
            ioerr += WRITE_COMMAND_CONFIGr(unit, port, command_cfg);
        }

        if (mac_lb || phy_lb) {
            BMD_PORT_STATUS_SET(unit, port, BMD_PST_LINK_UP | BMD_PST_FORCE_LINK);
        } else {
            BMD_PORT_STATUS_CLR(unit, port, BMD_PST_FORCE_LINK);
        }
    }

    return ioerr ? CDK_E_IO : rv;
}
#endif /* CDK_CONFIG_INCLUDE_BCM56840_A0 */
