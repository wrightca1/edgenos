/*
 * l3.c - L3 routing / FIB programming
 *
 * Translates kernel route and ARP/ND events into ASIC L3 table entries.
 * When OpenBCM SDK is integrated, this calls bcm_l3_route_add(),
 * bcm_l3_egress_create(), bcm_l3_host_add(), etc.
 *
 * With OpenMDK only, L3 hardware offload is limited. This module
 * tracks routes for future OpenBCM integration.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <net/if.h>

#include "switchd.h"
#include "l3.h"
#include "portmap.h"

int l3_init(void)
{
    syslog(LOG_INFO, "L3 routing initialized (software forwarding mode)");

    /*
     * With OpenMDK, L3 is software-forwarded via TUN interfaces.
     * The kernel routes traffic, and switchd handles packet I/O.
     *
     * For hardware L3 offload, OpenBCM SDK is needed:
     * - bcm_l3_init(unit)
     * - bcm_l3_intf_create() for each routed VLAN
     * - bcm_l3_egress_create() for each next-hop
     * - bcm_l3_route_add() for each FIB entry
     */

    return 0;
}

int l3_route_add(int family, const void *dst, int prefix_len,
                 const void *gw, int oif)
{
    char dst_str[INET6_ADDRSTRLEN] = "?";
    char gw_str[INET6_ADDRSTRLEN] = "?";
    char ifname[IFNAMSIZ] = "?";

    inet_ntop(family, dst, dst_str, sizeof(dst_str));
    if (gw)
        inet_ntop(family, gw, gw_str, sizeof(gw_str));
    if (oif)
        if_indextoname(oif, ifname);

    syslog(LOG_INFO, "L3 route add: %s/%d via %s dev %s",
           dst_str, prefix_len, gw_str, ifname);

    /*
     * TODO: With OpenBCM SDK:
     * 1. Create egress object: bcm_l3_egress_create()
     * 2. Add route: bcm_l3_route_add(unit, &route)
     *    - route.l3a_subnet = dst
     *    - route.l3a_ip_mask = prefix_to_mask(prefix_len)
     *    - route.l3a_intf = egress_intf_id
     */

    return 0;
}

int l3_route_del(int family, const void *dst, int prefix_len)
{
    char dst_str[INET6_ADDRSTRLEN] = "?";
    inet_ntop(family, dst, dst_str, sizeof(dst_str));

    syslog(LOG_INFO, "L3 route del: %s/%d", dst_str, prefix_len);

    /*
     * TODO: bcm_l3_route_delete(unit, &route)
     */

    return 0;
}

int l3_host_add(int family, const void *addr, const uint8_t *mac, int ifindex)
{
    char addr_str[INET6_ADDRSTRLEN] = "?";
    inet_ntop(family, addr, addr_str, sizeof(addr_str));

    syslog(LOG_DEBUG, "L3 host add: %s → %02x:%02x:%02x:%02x:%02x:%02x",
           addr_str, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /*
     * TODO: bcm_l3_host_add(unit, &host)
     *   - host.l3a_ip_addr = addr
     *   - host.l3a_nexthop_mac = mac
     *   - host.l3a_intf = egress_intf_for(ifindex)
     */

    return 0;
}

int l3_host_del(int family, const void *addr)
{
    char addr_str[INET6_ADDRSTRLEN] = "?";
    inet_ntop(family, addr, addr_str, sizeof(addr_str));

    syslog(LOG_DEBUG, "L3 host del: %s", addr_str);

    /*
     * TODO: bcm_l3_host_delete(unit, &host)
     */

    return 0;
}
