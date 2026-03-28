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

/* Port-to-I2C bus mapping
 * SFP: port N (1-based) = I2C bus (21 + N)
 * QSFP: port 49=18, 50=19, 51=20, 52=21
 */
static const int port_to_i2c_bus[SWITCHD_MAX_PORTS] = {
    22, 23, 24, 25, 26, 27, 28, 29,   /* ports 1-8 */
    30, 31, 32, 33, 34, 35, 36, 37,   /* ports 9-16 */
    38, 39, 40, 41, 42, 43, 44, 45,   /* ports 17-24 */
    46, 47, 48, 49, 50, 51, 52, 53,   /* ports 25-32 */
    54, 55, 56, 57, 58, 59, 60, 61,   /* ports 33-40 */
    62, 63, 64, 65, 66, 67, 68, 69,   /* ports 41-48 */
    18, 19, 20, 21,                    /* ports 49-52 (QSFP) */
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
         * Set SERDES_MODE flag for single-lane ports.
         * Without this, the Warpcore PHY driver classifies ports as
         * 4-lane (IS_4LANE_PORT) and configures 10GBASE-CX4 encoding
         * instead of 10G XFI (single-lane 64b/66b for SFP+).
         * CX4 encoding is incompatible with 10GBASE-LR/SR optics.
         */
        rv = bmd_phy_mode_set(switchd.unit, port, "warpcore",
                         BMD_PHY_MODE_SERDES, 1);
        {
            phy_ctrl_t *pc_chk = BMD_PORT_PHY_CTRL(switchd.unit, port);
            if (pc_chk) {
                syslog(LOG_INFO, "Port %s: SERDES_MODE set rv=%d flags=0x%x 4lane=%d",
                       switchd.ports[i].ifname, rv,
                       PHY_CTRL_FLAGS(pc_chk),
                       (PHY_CTRL_FLAGS(pc_chk) & PHY_F_SERDES_MODE) == 0);
            } else {
                syslog(LOG_WARNING, "Port %s: no PHY ctrl for port %d",
                       switchd.ports[i].ifname, port);
            }
        }

        rv = bmd_port_mode_set(switchd.unit, port, mode, flags);
        if (rv < 0) {
            syslog(LOG_ERR, "Port %s: bmd_port_mode_set(%d, %dG) failed: %d",
                   switchd.ports[i].ifname, port, speed / 1000, rv);
            continue;
        }

        /* Read back MISC1r (0x8308) to verify speed_val was written */
        {
            phy_ctrl_t *pc_dbg = BMD_PORT_PHY_CTRL(switchd.unit, port);
            if (pc_dbg && i < 3) {
                uint32_t misc1_val = 0, misc3_val = 0, fwmode_val = 0;
                /* MISC1r = block 0x8300, reg 0x08 = devad 1, addr 0x8308 */
                PHY_BUS_READ(pc_dbg, 0x1f, &misc1_val); /* save block */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x8300);
                PHY_BUS_READ(pc_dbg, 0x08, &misc1_val);
                /* MISC3r = block 0x8350, reg 0x08 = devad 1, addr 0x8358 */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x8350);
                PHY_BUS_READ(pc_dbg, 0x08, &misc3_val);
                /* FIRMWARE_MODEr = block 0x8200, reg 0x0a = addr 0x820a */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x8200);
                PHY_BUS_READ(pc_dbg, 0x0a, &fwmode_val);
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x0000); /* restore block */
                syslog(LOG_INFO,
                       "Port %s: MISC1r=0x%04x MISC3r=0x%04x FWMODE=0x%04x",
                       switchd.ports[i].ifname,
                       misc1_val & 0xffff, misc3_val & 0xffff,
                       fwmode_val & 0xffff);
            }
        }

        /*
         * Set TX driver and pre-emphasis via direct Warpcore register writes.
         *
         * Values captured from Cumulus 2.5.1 switchd via GDB breakpoints
         * on soc_miim_write (March 27, 2026):
         *
         * 10G SFP+:
         *   Block 0x8370 reg 0x18 (TX_ANATXACONTROL6) = 0x0ACC
         *     idriver=10(0xA), ipredriver=12(0xC), post2=12(0xC)
         *   Block 0x8370 reg 0x10 (TX_ANATXACONTROL0) = 0x000E
         *     TX FIR enable
         *   Block 0x8370 reg 0x15 (TX_ANATXACONTROL5) = 0x0002
         *     TX FIR coefficient
         *
         * 40G QSFP:
         *   Block 0x8370 reg 0x18 (TX_ANATXACONTROL6) = 0x0AFF
         *     idriver=10(0xA), ipredriver=15(0xF), post2=15(0xF)
         *   Block 0x8370 reg 0x10 (TX_ANATXACONTROL0) = 0x000E
         *
         * Cumulus writes these as standard CL22 block-addressed MIIM,
         * NOT via firmware mailbox. Per-lane via AER (block 0xFFD0
         * reg 0x1E = 0x180N for lane N).
         */
        {
            phy_ctrl_t *pc = BMD_PORT_PHY_CTRL(switchd.unit, port);
            if (pc) {
                int lane;
                int is_qsfp = (switchd.ports[i].port_type == PORT_TYPE_QSFP);
                uint32_t tx_amp = is_qsfp ? 0x0AFF : 0x0ACC;

                for (lane = 0; lane < 4; lane++) {
                    /* Select lane via AER */
                    PHY_BUS_WRITE(pc, 0x1f, 0xffd0);
                    PHY_BUS_WRITE(pc, 0x1e, 0x1800 + lane);

                    /* TX_ANATXACONTROL6: driver current + pre-emphasis */
                    PHY_BUS_WRITE(pc, 0x1f, 0x8370);
                    PHY_BUS_WRITE(pc, 0x18, tx_amp);

                    /* TX_ANATXACONTROL0: TX FIR enable */
                    PHY_BUS_WRITE(pc, 0x10, 0x000e);

                    /* TX_ANATXACONTROL5: FIR coefficient (10G only) */
                    if (!is_qsfp)
                        PHY_BUS_WRITE(pc, 0x15, 0x0002);
                }

                /* Restore AER to broadcast */
                PHY_BUS_WRITE(pc, 0x1f, 0xffd0);
                PHY_BUS_WRITE(pc, 0x1e, 0x0000);
                PHY_BUS_WRITE(pc, 0x1f, 0x0000);

                syslog(LOG_INFO, "Port %s: TX driver set 0x%04x (%s)",
                       switchd.ports[i].ifname, tx_amp,
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
