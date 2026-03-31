/*
 * portmap.c - Port mapping for AS5610-52X
 *
 * Maps between:
 *   - Front-panel port number (swp1-52)
 *   - BCM logical port (1-52)
 *   - SerDes physical lane
 *   - I2C bus number (port N = bus 21+N for SFP, 18-21 for QSFP)
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "switchd.h"
#include "portmap.h"

/* BMD headers */
#include <bmd/bmd.h>
#include <bmd/bmd_phy.h>
#include <bmd/bmd_phy_ctrl.h>

/* PHY headers - for direct PHY_CONFIG_SET access */
#include <phy/phy.h>

/* Port-to-SerDes lane mapping for AS5610-52X
 * Index = front-panel port (0-based), value = SerDes lane
 */
static const int port_to_lane[SWITCHD_MAX_PORTS] = {
    /* SFP+ ports 1-8 */   65, 66, 67, 68, 69, 70, 71, 72,
    /* SFP+ ports 9-16 */   5,  6,  7,  8,  9, 10, 11, 12,
    /* SFP+ ports 17-24 */ 13, 14, 15, 16, 18, 17, 20, 19,
    /* SFP+ ports 25-32 */ 22, 21, 24, 23, 25, 26, 27, 28,
    /* SFP+ ports 33-40 */ 29, 30, 31, 32, 33, 34, 35, 36,
    /* SFP+ ports 41-48 */ 37, 38, 39, 40, 41, 42, 43, 44,
    /* QSFP+ ports 49-52 */ 49, 45, 61, 57,
};

/* Port-to-I2C bus mapping (EdgeNOS kernel 5.10)
 *
 * Kernel 5.10 enumerates I2C mux children depth-first (sub-mux children
 * get consecutive bus numbers immediately after parent), unlike Cumulus
 * kernel 3.2 which was breadth-first. See I2C_BUS_NUMBER_MAPPING.md.
 *
 * Bus 1 → mux 0x75 ch0 → bus 10 → sub-mux 0x74 → buses 11-18 (swp1-8)
 * Bus 1 → mux 0x75 ch1 → bus 19 → sub-mux 0x74 → buses 20-27 (swp9-16)
 * Bus 1 → mux 0x75 ch2 → bus 28 → sub-mux 0x74 → buses 29-36 (swp17-24)
 * Bus 1 → mux 0x75 ch3 → bus 37 → sub-mux 0x74 → buses 38-45 (swp25-32)
 * Bus 1 → mux 0x76 ch0 → bus 46 → sub-mux 0x74 → buses 47-54 (swp33-40)
 * Bus 1 → mux 0x76 ch1 → bus 55 → sub-mux 0x74 → buses 56-63 (swp41-48)
 * Bus 1 → mux 0x77 ch0-3 → buses 66-69 (QSFP swp49-52)
 */
static const int port_to_i2c_bus[SWITCHD_MAX_PORTS] = {
    11, 12, 13, 14, 15, 16, 17, 18,   /* ports 1-8   (group 1, sub-mux on bus 10) */
    20, 21, 22, 23, 24, 25, 26, 27,   /* ports 9-16  (group 2, sub-mux on bus 19) */
    29, 30, 31, 32, 33, 34, 35, 36,   /* ports 17-24 (group 3, sub-mux on bus 28) */
    38, 39, 40, 41, 42, 43, 44, 45,   /* ports 25-32 (group 4, sub-mux on bus 37) */
    47, 48, 49, 50, 51, 52, 53, 54,   /* ports 33-40 (group 5, sub-mux on bus 46) */
    56, 57, 58, 59, 60, 61, 62, 63,   /* ports 41-48 (group 6, sub-mux on bus 55) */
    66, 67, 68, 69,                    /* ports 49-52 (QSFP, mux 0x77) */
};

void portmap_parse_config(const char *key, const char *val)
{
    int port;
    int lane, speed;

    /* Parse portmap_N.0=lane:speed */
    if (sscanf(key, "portmap_%d.0", &port) != 1)
        return;
    if (sscanf(val, "%d:%d", &lane, &speed) != 2)
        return;

    if (port < 1 || port > SWITCHD_MAX_PORTS)
        return;

    int idx = port - 1;
    switchd.ports[idx].valid = 1;
    switchd.ports[idx].logical_port = port;
    switchd.ports[idx].physical_lane = lane;
    switchd.ports[idx].speed = speed * 1000;  /* Convert to Mbps */
    switchd.ports[idx].port_type = (speed == 40) ? PORT_TYPE_QSFP : PORT_TYPE_SFP;
    snprintf(switchd.ports[idx].ifname, sizeof(switchd.ports[idx].ifname),
             "swp%d", port);
}

int portmap_configure_ports(void)
{
    int i;
    int configured = 0;

    /* If no ports were parsed from config, use hardcoded defaults */
    if (switchd.ports[0].valid == 0) {
        syslog(LOG_INFO, "Using default port map");
        for (i = 0; i < SWITCHD_MAX_PORTS; i++) {
            switchd.ports[i].valid = 1;
            switchd.ports[i].logical_port = i + 1;
            switchd.ports[i].physical_lane = port_to_lane[i];
            switchd.ports[i].speed = (i >= SWITCHD_SFP_PORTS) ? 40000 : 10000;
            switchd.ports[i].port_type = (i >= SWITCHD_SFP_PORTS) ?
                                         PORT_TYPE_QSFP : PORT_TYPE_SFP;
            snprintf(switchd.ports[i].ifname,
                     sizeof(switchd.ports[i].ifname), "swp%d", i + 1);
        }
    }

    /* Configure each port on the ASIC via BMD */
    for (i = 0; i < SWITCHD_MAX_PORTS; i++) {
        if (!switchd.ports[i].valid)
            continue;

        /*
         * BMD functions expect CDK physical port number,
         * which is physical_lane on BCM56840.
         */
        int port = switchd.ports[i].physical_lane;
        int speed = switchd.ports[i].speed;
        bmd_port_mode_t mode;
        uint32_t flags = 0;
        int rv;

        /*
         * Select port mode based on speed.
         * From OpenMDK bcm56840_a0_bmd_port_mode_set.c:
         *   10G SFP+: bmdPortMode10000fd -> FV_fdr_10G_XFI (speed_val=0x25)
         *   40G QSFP: bmdPortMode40000fd -> FV_fdr_40G_KR4 (speed_val=0x27)
         *
         * This internally calls bcmi_warpcore_xgxs_speed_set() which:
         *   - Sets forced speed in MISC1r/MISC3r
         *   - Configures PLL mode (div66 for 10G, default for 40G)
         *   - Sets FIRMWARE_MODEr for SFP_DAC/XFI/KR
         *   - Restarts PLL sequencer
         *   - Configures MAC speed/duplex in XMAC_MODEr
         *   - Enables XMAC RX/TX
         *   - Updates EPC_LINK_BMAP
         */
        if (switchd.ports[i].port_type == PORT_TYPE_QSFP)
            mode = bmdPortMode40000fd;
        else
            mode = bmdPortMode10000fd;

        /*
         * Set SERDES_MODE flag for single-lane SFP+ ports ONLY.
         * Without this, the Warpcore PHY driver classifies ports as
         * 4-lane (IS_4LANE_PORT) and configures 10GBASE-CX4 encoding
         * instead of 10G XFI (single-lane 64b/66b for SFP+).
         * CX4 encoding is incompatible with 10GBASE-LR/SR optics.
         *
         * QSFP ports must NOT have SERDES_MODE set — they need 4-lane
         * mode for 40G. Setting SERDES_MODE on QSFP causes
         * bmd_port_mode_set(40G) to fail with CDK_E_PARAM (-4).
         */
        if (switchd.ports[i].port_type != PORT_TYPE_QSFP) {
            rv = bmd_phy_mode_set(switchd.unit, port, "warpcore",
                             BMD_PHY_MODE_SERDES, 1);
        } else {
            rv = 0; /* QSFP: keep 4-lane mode */
        }
        {
            phy_ctrl_t *pc_chk = BMD_PORT_PHY_CTRL(switchd.unit, port);
            if (pc_chk) {
                /*
                 * CRITICAL FIX: Clear PHY_F_CLAUSE45 flag.
                 *
                 * The Warpcore PHY init sets CLAUSE45 based on the
                 * MULTIMMDS_EN register. On BCM56846, this is enabled
                 * but the CMIC MIIM doesn't properly handle CL45 for
                 * internal SerDes. With CLAUSE45 set, phy_aer_iblk_write
                 * takes the early CL45 path (DEVAD+reg) instead of the
                 * CL22 block-select path (reg 0x1F + reg offset).
                 *
                 * Result: MISC1r, MISC3r, FIRMWARE_MODEr writes all
                 * silently fail → speed encoding never changes from
                 * default CX4 → PCS can't block-lock on XFI signal
                 * → link stays down despite TX/RX optical power.
                 *
                 * Cumulus SDK 6.3.8 likely has a different MIIM
                 * implementation that handles CL45 correctly for
                 * iProc-based switches. OpenMDK needs CL22 path.
                 */
                PHY_CTRL_FLAGS(pc_chk) &= ~PHY_F_CLAUSE45;

                syslog(LOG_INFO, "Port %s: flags=0x%x (CLAUSE45 cleared) bus=%s phy_addr=0x%03x inst=%d",
                       switchd.ports[i].ifname,
                       PHY_CTRL_FLAGS(pc_chk),
                       pc_chk->bus ? pc_chk->bus->drv_name : "NULL",
                       pc_chk->bus ? pc_chk->bus->phy_addr(port) : 0,
                       PHY_CTRL_INST(pc_chk));
            } else {
                syslog(LOG_WARNING, "Port %s: no PHY ctrl for port %d",
                       switchd.ports[i].ifname, port);
            }
        }

        /*
         * Force port mode re-set by first disabling, then enabling.
         * bmd_switching_init already called bmd_port_mode_set(Auto) which
         * set CX4 mode (speed_val=0x05) because CLAUSE45 was set during
         * that call. Now with CLAUSE45 cleared, a fresh port_mode_set
         * will correctly write MISC1r + MISC3r for XFI (speed_val=0x25).
         *
         * Without the Disable→Enable cycle, bmd_port_mode_set sees
         * "already at 10G" and skips the PHY speed_set entirely.
         */
        bmd_port_mode_set(switchd.unit, port, bmdPortModeDisabled, 0);

        rv = bmd_port_mode_set(switchd.unit, port, mode, flags);
        if (rv < 0) {
            syslog(LOG_ERR, "Port %s: bmd_port_mode_set(%d, %dG) failed: %d",
                   switchd.ports[i].ifname, port, speed / 1000, rv);
            continue;
        }

        /*
         * Fix: Explicitly release TX/RX ASIC reset and set IND_40BITIF.
         *
         * The Disable→Enable cycle leaves MISC6r reset_rx=1 (RX stuck
         * in reset) and MISC3r IND_40BITIF=0 (40-bit interface not
         * enabled). Without these, PCS can never lock.
         *
         * speed_set (line 1589-1594 of xgxs_drv.c) should release
         * the reset, but the write doesn't persist — possibly because
         * the Disable call's MISC6r state overrides the Enable's write.
         */
        {
            phy_ctrl_t *pc_fix = BMD_PORT_PHY_CTRL(switchd.unit, port);
            if (pc_fix) {
                /*
                 * Release TX/RX ASIC reset via MISC6r.
                 * MISC6r = CDK 0x8345 = block 0x8340, offset 0x5, CL22 reg 0x15
                 * (NOT reg 0x19 — that's MISC7r at CDK 0x8349!)
                 * RESET_RX_ASIC = bit 15, RESET_TX_ASIC = bit 14
                 */
                PHY_BUS_WRITE(pc_fix, 0x1f, 0x8340);
                uint32_t misc6_val = 0;
                PHY_BUS_READ(pc_fix, 0x15, &misc6_val);
                misc6_val &= ~((1 << 15) | (1 << 14));  /* Clear RESET_RX_ASIC and RESET_TX_ASIC */
                PHY_BUS_WRITE(pc_fix, 0x15, misc6_val);

                /* Set IND_40BITIF=1 in MISC3r for XFI */
                PHY_BUS_WRITE(pc_fix, 0x1f, 0x8330);
                uint32_t misc3_val = 0;
                PHY_BUS_READ(pc_fix, 0x1c, &misc3_val);
                misc3_val |= (1 << 6);  /* IND_40BITIF = bit 6 */
                PHY_BUS_WRITE(pc_fix, 0x1c, misc3_val);

                /*
                 * Restart PLL sequencer after MISC3r/MISC6r changes.
                 *
                 * The speed_set code restarts the sequencer during port
                 * enable, but our IND_40BITIF fix above happens AFTER
                 * that. Without a sequencer restart, the PLL is running
                 * with the old (wrong) interface mode → PCS never locks.
                 *
                 * XGXSCONTROLr = block 0x8000, offset 0x00, CL22 reg 0x10
                 * START_SEQUENCER = bit 13
                 */
                PHY_BUS_WRITE(pc_fix, 0x1f, 0x8000);
                uint32_t xgxs_ctrl = 0;
                PHY_BUS_READ(pc_fix, 0x10, &xgxs_ctrl);
                xgxs_ctrl &= ~(1 << 13);  /* Stop sequencer */
                PHY_BUS_WRITE(pc_fix, 0x10, xgxs_ctrl);
                xgxs_ctrl |= (1 << 13);   /* Start sequencer */
                PHY_BUS_WRITE(pc_fix, 0x10, xgxs_ctrl);

                PHY_BUS_WRITE(pc_fix, 0x1f, 0x0000);

                syslog(LOG_INFO, "Port %s: forced reset_rx=0 IND_40BITIF=1 seq_restart",
                       switchd.ports[i].ifname);
            }
        }

        /* Debug: comprehensive register dump after config */
        {
            phy_ctrl_t *pc_dbg = BMD_PORT_PHY_CTRL(switchd.unit, port);
            if (pc_dbg && i < 2) {
                uint32_t val = 0;

                /* Wait 100ms for PLL and CDR to settle */
                usleep(100000);

                /* Set AER lane for per-lane register reads */
                int wc_lane = PHY_CTRL_INST(pc_dbg) & 0x3;
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0xffd0);
                PHY_BUS_WRITE(pc_dbg, 0x1e, wc_lane);
                syslog(LOG_INFO, "Port %s: AER lane=%d (inst=0x%x)",
                       switchd.ports[i].ifname, wc_lane,
                       (unsigned)PHY_CTRL_INST(pc_dbg));

                /* XGXSBLK0: control/status, PLL lock */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x8000);
                PHY_BUS_READ(pc_dbg, 0x10, &val);
                syslog(LOG_INFO, "Port %s: XGXSCONTROLr=0x%04x seq=%d",
                       switchd.ports[i].ifname, val & 0xffff, (val >> 13) & 1);
                PHY_BUS_READ(pc_dbg, 0x11, &val);
                syslog(LOG_INFO, "Port %s: XGXSSTATUSr=0x%04x pll_lock=%d",
                       switchd.ports[i].ifname, val & 0xffff, (val >> 11) & 1);

                /* MISC1r: speed[4:0], PLL mode */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x8300);
                PHY_BUS_READ(pc_dbg, 0x18, &val);
                syslog(LOG_INFO, "Port %s: MISC1r=0x%04x spd[4:0]=%d pll=0x%x",
                       switchd.ports[i].ifname, val & 0xffff,
                       val & 0x1f, (val >> 8) & 0xf);

                /* MISC3r: speed B5, IND_40BITIF */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x8330);
                PHY_BUS_READ(pc_dbg, 0x1c, &val);
                {
                    uint32_t m3 = val & 0xffff;
                    uint32_t b5 = (m3 >> 7) & 1;
                    uint32_t ind40 = (m3 >> 6) & 1;
                    syslog(LOG_INFO, "Port %s: MISC3r=0x%04x B5=%d IND40=%d speed=0x%02x",
                           switchd.ports[i].ifname, m3, b5, ind40,
                           (5) | (b5 << 5));
                }

                /* MISC6r: reset, USE_BRCM6466 */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x8340);
                PHY_BUS_READ(pc_dbg, 0x15, &val);
                syslog(LOG_INFO, "Port %s: MISC6r=0x%04x rst_rx=%d rst_tx=%d brcm6466=%d",
                       switchd.ports[i].ifname, val & 0xffff,
                       (val >> 15) & 1, (val >> 14) & 1, (val >> 7) & 1);

                /* MISC7r: force_oscdr, CL49 in all mode */
                PHY_BUS_READ(pc_dbg, 0x19, &val);
                syslog(LOG_INFO, "Port %s: MISC7r=0x%04x cl49_all=%d oscdr_force=%d oscdr_val=0x%x",
                       switchd.ports[i].ifname, val & 0xffff,
                       (val >> 5) & 1, (val >> 9) & 1, (val >> 10) & 0xf);

                /* FIRMWARE_MODEr */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x8200);
                PHY_BUS_READ(pc_dbg, 0x1a, &val);
                syslog(LOG_INFO, "Port %s: FWMODE=0x%04x l0=%d l1=%d l2=%d l3=%d",
                       switchd.ports[i].ifname, val & 0xffff,
                       val & 0xf, (val >> 4) & 0xf,
                       (val >> 8) & 0xf, (val >> 12) & 0xf);

                /* CONTROL1000X1r (CDK 0x8300): fiber mode, autoneg */
                /* Wait — CONTROL1000X1r is in DIGITAL block, CDK 0x8300 = ... */
                /* Actually, CONTROL1000X1r is at block 0x8300, offset 0x10 = reg 0x10+0x10...
                 * No — let me use the CDK address. CONTROL1000X1r CDK might be elsewhere */

                /* DIGITAL block: CONTROL1000X1r = CDK 0x8300 (wait, that's MISC1r block)
                 * Actually SERDESDIGITAL_CONTROL1000X1r = 0x8300 in some drivers,
                 * but in WC it's at a different address. Let me read from block 0x8300 regs. */

                /* CL49 LSM status (64b/66b PCS block lock) */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x8360);
                PHY_BUS_READ(pc_dbg, 0x17, &val);
                syslog(LOG_INFO, "Port %s: CL49_LSM=0x%04x block_lock=%d",
                       switchd.ports[i].ifname, val & 0xffff,
                       (val >> 15) & 1);

                /* ANARXSTATUSr: signal detect, CDR status
                 * Read with multiple rxtestsel values to find actual sigdet */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x80b0);
                PHY_BUS_READ(pc_dbg, 0x11, &val);
                {
                    uint32_t rxctrl_orig = val & 0xffff;
                    int sel;
                    for (sel = 0; sel <= 7; sel++) {
                        uint32_t rxctrl_new = (rxctrl_orig & ~0x1f) | sel;
                        PHY_BUS_WRITE(pc_dbg, 0x11, rxctrl_new);
                        PHY_BUS_READ(pc_dbg, 0x10, &val);
                        syslog(LOG_INFO, "Port %s: ANARXSTAT[sel=%d]=0x%04x",
                               switchd.ports[i].ifname, sel, val & 0xffff);
                    }
                    /* Restore */
                    PHY_BUS_WRITE(pc_dbg, 0x11, rxctrl_orig);
                }

                /* RX66_CONTROLr: clock compensation enable
                 * CDK 0x83C0 = block 0x83C0, offset 0x0, CL22 reg 0x10 */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x83c0);
                PHY_BUS_READ(pc_dbg, 0x10, &val);
                syslog(LOG_INFO, "Port %s: RX66_CTRL=0x%04x cc_en=%d cc_data=%d",
                       switchd.ports[i].ifname, val & 0xffff,
                       val & 1, (val >> 1) & 1);

                /* COMBO_MIICNTLr (IEEE MII control, reg 0x00 page 0)
                 * autoneg_enable = bit 12 */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x0000);
                PHY_BUS_READ(pc_dbg, 0x00, &val);
                syslog(LOG_INFO, "Port %s: MII_CTRL=0x%04x an_en=%d loopback=%d",
                       switchd.ports[i].ifname, val & 0xffff,
                       (val >> 12) & 1, (val >> 14) & 1);

                /* MII_STATUS (IEEE reg 1): link, AN complete */
                PHY_BUS_READ(pc_dbg, 0x01, &val);
                syslog(LOG_INFO, "Port %s: MII_STAT=0x%04x link=%d an_done=%d",
                       switchd.ports[i].ifname, val & 0xffff,
                       (val >> 2) & 1, (val >> 5) & 1);

                /* AN_IEEECONTROL1r: CL73 AN enable
                 * Block 0x0010 (AN block), reg 0x10 (offset 0) */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x0010);
                PHY_BUS_READ(pc_dbg, 0x10, &val);
                syslog(LOG_INFO, "Port %s: AN_CTRL1=0x%04x cl73_an_en=%d restart=%d",
                       switchd.ports[i].ifname, val & 0xffff,
                       (val >> 12) & 1, (val >> 9) & 1);

                /* TENGBASE_KR_PMD_CONTROL_150r: CL72 training enable
                 * CDK 0x0096 → IEEE CL45 device 1, reg 0x96
                 * In CL22 AER block: block 0x0090, offset 6, reg 0x16 */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x0090);
                PHY_BUS_READ(pc_dbg, 0x16, &val);
                syslog(LOG_INFO, "Port %s: KR_PMD_CTRL=0x%04x training_en=%d",
                       switchd.ports[i].ifname, val & 0xffff,
                       (val >> 1) & 1);

                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x0000);
            }
        }

        /*
         * Set TX driver current and pre-emphasis.
         *
         * Values captured from Cumulus 2.5.1 switchd via GDB breakpoints
         * on soc_miim_write (March 27, 2026).
         *
         * Cumulus writes two sets of registers:
         *
         * 1. TX0_TX_DRIVERr (block 0x8060, reg 0x07, addr 0x8067):
         *    OpenMDK PHY_CONFIG_SET API — sets IDRIVER and IPREDRIVER fields.
         *    This is the standard SDK path via PhyConfig_TxIDrv/TxPreIDrv.
         *
         * 2. TX_ANATXACONTROL (block 0x8370, regs 0x10/0x15/0x18):
         *    Analog TX pre-emphasis / FIR filter config.
         *    Written directly via CL22 block addressing.
         *    10G: reg 0x18 = 0x0ACC, reg 0x10 = 0x000E, reg 0x15 = 0x0002
         *    40G: reg 0x18 = 0x0AFF, reg 0x10 = 0x000E
         *
         * Both are needed: TX_DRIVERr sets the output current,
         * ANATXACONTROL sets the FIR/pre-emphasis shaping.
         */
        {
            phy_ctrl_t *pc = BMD_PORT_PHY_CTRL(switchd.unit, port);
            if (pc) {
                int is_qsfp = (switchd.ports[i].port_type == PORT_TYPE_QSFP);
                int lane;

                /*
                 * Part 1: TX_DRIVERr via OpenMDK PHY_CONFIG_SET API.
                 * Decoded from Cumulus capture of TX0_TX_DRIVERr (0x8067):
                 *   IDRIVER = 10 (0xA), IPREDRIVER = 12 (0xC) for 10G
                 *   IDRIVER = 10 (0xA), IPREDRIVER = 15 (0xF) for 40G
                 */
                rv = PHY_CONFIG_SET(pc, PhyConfig_TxIDrv, 10, NULL);
                if (rv < 0)
                    syslog(LOG_WARNING, "Port %s: PhyConfig_TxIDrv failed: %d",
                           switchd.ports[i].ifname, rv);

                rv = PHY_CONFIG_SET(pc, PhyConfig_TxPreIDrv,
                                    is_qsfp ? 15 : 12, NULL);
                if (rv < 0)
                    syslog(LOG_WARNING, "Port %s: PhyConfig_TxPreIDrv failed: %d",
                           switchd.ports[i].ifname, rv);

                /*
                 * Part 2: TX_ANATXACONTROL (block 0x8370) via direct
                 * CL22 block access. Per-lane via AER lane select.
                 */
                for (lane = 0; lane < 4; lane++) {
                    /* Select lane via AER */
                    PHY_BUS_WRITE(pc, 0x1f, 0xffd0);
                    PHY_BUS_WRITE(pc, 0x1e, 0x1800 + lane);

                    /* TX_ANATXACONTROL6 (reg 0x18): amplitude */
                    PHY_BUS_WRITE(pc, 0x1f, 0x8370);
                    PHY_BUS_WRITE(pc, 0x18, is_qsfp ? 0x0AFF : 0x0ACC);

                    /* TX_ANATXACONTROL0 (reg 0x10): FIR enable */
                    PHY_BUS_WRITE(pc, 0x10, 0x000E);

                    /* TX_ANATXACONTROL5 (reg 0x15): FIR coeff (10G only) */
                    if (!is_qsfp)
                        PHY_BUS_WRITE(pc, 0x15, 0x0002);
                }

                /* Restore AER to broadcast */
                PHY_BUS_WRITE(pc, 0x1f, 0xffd0);
                PHY_BUS_WRITE(pc, 0x1e, 0x0000);
                PHY_BUS_WRITE(pc, 0x1f, 0x0000);

                syslog(LOG_INFO, "Port %s: TX driver idrv=10 ipre=%d + ANATX 0x%04x (%s)",
                       switchd.ports[i].ifname,
                       is_qsfp ? 15 : 12,
                       is_qsfp ? 0x0AFF : 0x0ACC,
                       is_qsfp ? "40G QSFP" : "10G SFP+");
            } else {
                syslog(LOG_WARNING, "Port %s: no PHY control (port %d)",
                       switchd.ports[i].ifname, port);
            }
        }

        switchd.ports[i].enabled = 1;
        configured++;

        syslog(LOG_INFO, "Port %s: logical=%d lane=%d speed=%dG type=%s mode=0x%x OK",
               switchd.ports[i].ifname,
               switchd.ports[i].logical_port,
               switchd.ports[i].physical_lane,
               switchd.ports[i].speed / 1000,
               switchd.ports[i].port_type == PORT_TYPE_QSFP ? "QSFP" : "SFP",
               mode);
    }

    switchd.num_ports = configured;
    syslog(LOG_INFO, "Configured %d ports", configured);
    return 0;
}

int portmap_swp_to_logical(int swp)
{
    if (swp < 1 || swp > SWITCHD_MAX_PORTS)
        return -1;
    return switchd.ports[swp - 1].logical_port;
}

int portmap_logical_to_swp(int logical)
{
    int i;
    for (i = 0; i < SWITCHD_MAX_PORTS; i++) {
        if (switchd.ports[i].logical_port == logical)
            return i + 1;
    }
    return -1;
}

/* Map CDK physical port number to front-panel swp number */
int portmap_phys_to_swp(int phys_port)
{
    int i;
    for (i = 0; i < SWITCHD_MAX_PORTS; i++) {
        if (switchd.ports[i].physical_lane == phys_port)
            return i + 1;
    }
    return -1;
}

int portmap_swp_to_i2c_bus(int swp)
{
    if (swp < 1 || swp > SWITCHD_MAX_PORTS)
        return -1;
    return port_to_i2c_bus[swp - 1];
}

/*
 * Poll all ports for link state changes.
 *
 * Called from the main loop. Uses bmd_port_mode_update() which:
 *   1. Reads PHY link status via MIIM (MII_STATUS on WC page 0x1800)
 *   2. On link-up: enables MAC RX, sets EPC_LINK_BMAP bit
 *   3. On link-down: disables MAC RX, clears EPC_LINK_BMAP bit
 *   4. Updates LED data at LED_DATA_OFFSET (0x80) for CMIC LED processor
 *
 * Cumulus polls at ~30ms per port. We poll all ports each call,
 * called from main loop with appropriate interval.
 */
static int link_poll_count;

int portmap_link_poll(void)
{
    int i;
    int changes = 0;
    int first_poll = (link_poll_count++ == 0);

    for (i = 0; i < SWITCHD_MAX_PORTS; i++) {
        if (!switchd.ports[i].valid || !switchd.ports[i].enabled)
            continue;

        int port = switchd.ports[i].physical_lane;  /* CDK port */
        int old_link = switchd.ports[i].link_up;

        /*
         * bmd_port_mode_update() does the full link state machine:
         *   - Calls bmd_phy_link_get() which reads WC MII_STATUS
         *   - If link changed: calls port_enable_set() to update MAC + EPC
         *   - Updates LED linkscan data at 0x80+port
         *
         * From bcm56840_a0_bmd_port_mode_update.c:
         *   bmd_link_update() -> bmd_phy_link_get() -> warpcore link_get()
         *   -> READ page 0x1800 reg 0x01 (MII_STATUS)
         *   -> if link: port_enable_set(1) -> XMAC_CTRL RX_EN=1
         *   -> else:    port_enable_set(0) -> XMAC_CTRL RX_EN=0
         */
        /*
         * bmd_port_mode_update() reads PHY link status and updates
         * MAC enable + EPC_LINK_BMAP. If it fails, we still try
         * bmd_phy_link_get() to check the physical link state.
         */
        bmd_port_mode_update(switchd.unit, port);

        /* Check physical link state */
        int link = 0;
        int autoneg_done = 0;
        int rv = bmd_phy_link_get(switchd.unit, port, &link, &autoneg_done);
        /* Log first poll result for debugging */
        if (first_poll && (i < 3 || switchd.ports[i].port_type == PORT_TYPE_QSFP)) {
            syslog(LOG_INFO, "link_poll[%s]: port=%d rv=%d link=%d an=%d",
                   switchd.ports[i].ifname, port, rv, link, autoneg_done);
        }
        if (rv == 0) {
            switchd.ports[i].link_up = link;

            /*
             * Ensure BMD_PST_LINK_UP matches actual PHY link state.
             * bmd_port_mode_update may fail on some ports but the
             * PHY link is real. Without BMD_PST_LINK_UP, bmd_tx
             * silently drops all packets.
             */
            if (link) {
                BMD_PORT_STATUS_SET(switchd.unit, port, BMD_PST_LINK_UP);
            } else {
                BMD_PORT_STATUS_CLR(switchd.unit, port, BMD_PST_LINK_UP);
            }

            if (link != old_link) {
                syslog(LOG_INFO, "BMD link %s: port %d (%s)",
                       link ? "UP" : "DOWN", port,
                       switchd.ports[i].ifname);
                changes++;
            }
        }
    }

    return changes;
}
