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

        int port = switchd.ports[i].logical_port;
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
        if (speed == 40000) {
            mode = bmdPortMode40000fd;
        } else {
            mode = bmdPortMode10000fd;
        }

        rv = bmd_port_mode_set(switchd.unit, port, mode, flags);
        if (rv < 0) {
            syslog(LOG_ERR, "Port %s: bmd_port_mode_set(%d, %dG) failed: %d",
                   switchd.ports[i].ifname, port, speed / 1000, rv);
            continue;
        }

        /*
         * Set TX driver current via OpenMDK PHY driver.
         *
         * Values from Cumulus RE capture (TX_DRIVERr = 0x0230):
         *   idriver = 2, predriver = 3
         *
         * API path (100% OpenMDK, no Cumulus code):
         *   BMD_PORT_PHY_CTRL(unit, port)  -> get phy_ctrl_t for this port
         *   PHY_CONFIG_SET(pc, cfg, val)   -> dispatch to WC driver
         *   bcmi_warpcore_xgxs_config_set  -> WRITE TXB_TX_DRIVERr via AER
         *
         * This works because bmd_init() loaded OpenMDK firmware v0x0101
         * which does NOT intercept direct register writes (unlike the
         * Cumulus firmware v0x0103/v0x242F which overrides TX_DRIVERr
         * via a firmware mailbox protocol).
         */
        {
            phy_ctrl_t *pc = BMD_PORT_PHY_CTRL(switchd.unit, port);
            if (pc) {
                rv = PHY_CONFIG_SET(pc, PhyConfig_TxIDrv, 2, NULL);
                if (rv < 0)
                    syslog(LOG_WARNING, "Port %s: PhyConfig_TxIDrv failed: %d",
                           switchd.ports[i].ifname, rv);

                rv = PHY_CONFIG_SET(pc, PhyConfig_TxPreIDrv, 3, NULL);
                if (rv < 0)
                    syslog(LOG_WARNING, "Port %s: PhyConfig_TxPreIDrv failed: %d",
                           switchd.ports[i].ifname, rv);
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
int portmap_link_poll(void)
{
    int i;
    int changes = 0;

    for (i = 0; i < SWITCHD_MAX_PORTS; i++) {
        if (!switchd.ports[i].valid || !switchd.ports[i].enabled)
            continue;

        int port = switchd.ports[i].logical_port;
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
        int rv = bmd_port_mode_update(switchd.unit, port);
        if (rv < 0) {
            /* Transient errors during link negotiation are normal */
            continue;
        }

        /* Check if link state changed by reading port status */
        int link = 0;
        int autoneg_done = 0;
        rv = bmd_phy_link_get(switchd.unit, port, &link, &autoneg_done);
        if (rv == 0) {
            switchd.ports[i].link_up = link;
            if (link != old_link) {
                syslog(LOG_INFO, "Port %s: link %s",
                       switchd.ports[i].ifname,
                       link ? "UP" : "DOWN");
                changes++;
            }
        }
    }

    return changes;
}
