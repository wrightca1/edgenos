/*
 * l2.c - L2 forwarding table management
 *
 * Programs MAC addresses into the ASIC L2 table via BMD.
 * Handles MAC learning events from netlink neighbor notifications.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <net/if.h>

#include "switchd.h"
#include "l2.h"
#include "portmap.h"

int l2_init(void)
{
    syslog(LOG_INFO, "L2 forwarding initialized");

    /*
     * BMD switching_init already set up:
     * - L2 table
     * - Default VLAN (1) with all ports
     * - MAC learning enabled
     * - L2 aging
     */
    return 0;
}

int l2_mac_add(const uint8_t *mac, int ifindex)
{
    char ifname[IFNAMSIZ];
    int swp;

    if (!if_indextoname(ifindex, ifname))
        return -1;

    if (strncmp(ifname, "swp", 3) != 0)
        return 0;  /* Not our interface */

    swp = atoi(ifname + 3);
    if (swp < 1 || swp > SWITCHD_MAX_PORTS)
        return -1;

    int logical = portmap_swp_to_logical(swp);
    if (logical < 0)
        return -1;

    syslog(LOG_DEBUG, "L2 add: %02x:%02x:%02x:%02x:%02x:%02x → port %d",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], logical);

    /*
     * TODO: Program ASIC L2 table
     * bmd_l2_addr_add(switchd.unit, logical, vlan, mac);
     */

    return 0;
}

int l2_mac_del(const uint8_t *mac, int ifindex)
{
    syslog(LOG_DEBUG, "L2 del: %02x:%02x:%02x:%02x:%02x:%02x ifidx=%d",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ifindex);

    /*
     * TODO: Remove from ASIC L2 table
     * bmd_l2_addr_delete(switchd.unit, mac);
     */

    return 0;
}
