/*
 * l2.c - L2 forwarding table management
 *
 * Programs MAC addresses into the ASIC L2 table via BMD.
 * Handles MAC learning events from netlink neighbor notifications.
 *
 * ASIC L2 table details (from RE docs):
 *   Table ID: 0x1547 (L2_USER_ENTRY)
 *   Entry size: 36 bytes (0x24)
 *   Layout: MAC at offset 0x08, VLAN at 0x14, port at 0x2c
 *   Access: via S-Channel WRITE_MEMORY at soc_schan_op (0x108623e4)
 *
 * BMD L2 API:
 *   bmd_port_mac_addr_add(unit, port, vlan, mac) - static MAC entry
 *   bmd_cpu_mac_addr_add(unit, vlan, mac) - CPU-destined MAC
 *   Hardware MAC learning is enabled by bmd_switching_init()
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <net/if.h>

#include "switchd.h"
#include "l2.h"
#include "portmap.h"

/* BMD headers */
#include <bmd/bmd.h>

/* Default VLAN */
#define DEFAULT_VLAN 1

int l2_init(void)
{
    syslog(LOG_INFO, "L2 forwarding initialized");

    /*
     * bmd_switching_init() already configured:
     * - L2 table reset
     * - Default VLAN (1) with all ports as members
     * - Hardware MAC learning enabled
     * - L2 aging timer active
     *
     * Add CPU MAC address so packets destined to the switch
     * management address get forwarded to CPU (via RX DMA).
     */

    /* TODO: Read management MAC from EEPROM (at24 on I2C bus 3/4)
     * For now use the base MAC from switchd HAL port map:
     * 80:a2:35:81:ca:af (swp1 MAC from Cumulus)
     */

    return 0;
}

int l2_mac_add(const uint8_t *mac, int ifindex)
{
    char ifname[IFNAMSIZ];
    int swp, logical, rv;

    if (!if_indextoname(ifindex, ifname))
        return -1;

    if (strncmp(ifname, "swp", 3) != 0)
        return 0;  /* Not our interface */

    swp = atoi(ifname + 3);
    if (swp < 1 || swp > SWITCHD_MAX_PORTS)
        return -1;

    /* Use CDK physical port for BMD calls */
    logical = switchd.ports[swp - 1].physical_lane;
    if (logical <= 0)
        return -1;

    syslog(LOG_DEBUG, "L2 add: %02x:%02x:%02x:%02x:%02x:%02x on port %d (swp%d)",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], logical, swp);

    /*
     * Program static MAC entry into ASIC L2 table.
     * bmd_port_mac_addr_add() internally:
     *   1. Builds L2_USER_ENTRY in 36-byte buffer
     *   2. Sets MAC, VLAN, port, STATIC bit
     *   3. Writes via S-Channel WRITE_MEMORY to table 0x1547
     *
     * From OpenMDK bcm56840_a0_bmd_port_mac_addr_add.c:
     *   L2Xm_MAC_ADDRf_SET() - sets MAC in entry
     *   L2Xm_VLAN_IDf_SET() - sets VLAN
     *   L2Xm_PORT_NUMf_SET() - sets destination port
     *   L2Xm_STATIC_BITf_SET() - marks as static (won't age)
     *   L2Xm_VALIDf_SET() - marks entry valid
     *   WRITE_L2Xm() - writes to ASIC via S-Channel
     */
    bmd_mac_addr_t bmac;
    memcpy(bmac.b, mac, 6);
    rv = bmd_port_mac_addr_add(switchd.unit, logical, DEFAULT_VLAN, &bmac);
    if (rv < 0) {
        syslog(LOG_WARNING, "L2 add failed: %02x:%02x:%02x:%02x:%02x:%02x "
               "port %d: rv=%d",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], logical, rv);
        return -1;
    }

    return 0;
}

int l2_mac_del(const uint8_t *mac, int ifindex)
{
    char ifname[IFNAMSIZ];
    int swp, logical, rv;

    if (!if_indextoname(ifindex, ifname))
        return -1;

    if (strncmp(ifname, "swp", 3) != 0)
        return 0;

    swp = atoi(ifname + 3);
    if (swp < 1 || swp > SWITCHD_MAX_PORTS)
        return -1;

    /* Use CDK physical port for BMD calls */
    logical = switchd.ports[swp - 1].physical_lane;
    if (logical <= 0)
        return -1;

    syslog(LOG_DEBUG, "L2 del: %02x:%02x:%02x:%02x:%02x:%02x on port %d (swp%d)",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], logical, swp);

    /*
     * Remove MAC entry from ASIC L2 table.
     * bmd_port_mac_addr_remove() invalidates the entry.
     */
    bmd_mac_addr_t bmac;
    memcpy(bmac.b, mac, 6);
    rv = bmd_port_mac_addr_remove(switchd.unit, logical, DEFAULT_VLAN, &bmac);
    if (rv < 0) {
        syslog(LOG_DEBUG, "L2 del failed (may not exist): rv=%d", rv);
    }

    return 0;
}
