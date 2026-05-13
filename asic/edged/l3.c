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

#include "edged.h"
#include "l3.h"
#include "portmap.h"

/* BMD/CDK headers — for SCHAN memory writes into chip L3 tables */
#include <bmd/bmd.h>
#include <cdk/cdk_device.h>
#include <cdk/chip/bcm56840_a0_defs.h>

/*
 * Convert a 6-byte MAC into the 2-word field format used by chip
 * tables (BMD_PORT_MAC, L2X, MY_STATION_TCAM all use the same
 * encoding from xgs_mac_to_field_val).
 */
static void mac_to_fval(const uint8_t *mac, uint32_t *fval)
{
    fval[0] = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16)
            | ((uint32_t)mac[4] <<  8) |  (uint32_t)mac[5];
    fval[1] = ((uint32_t)mac[0] <<  8) |  (uint32_t)mac[1];
}

/*
 * Add a MY_STATION_TCAM entry for one of our front-panel MACs.
 *
 * Without an entry here, the chip's L3 pipeline doesn't recognise
 * frames addressed to our swpN MAC as "destined for this router",
 * so IPv4/IPv6 unicast frames silently drop after the L3 lookup
 * misses.  ARP works because it's L2-only and bypasses this stage.
 *
 * IPV4_TERMINATION_ALLOWED + IPV6_TERMINATION_ALLOWED tell the chip
 * to terminate IP at this MAC (i.e., process as router endpoint).
 */
static int l3_my_station_idx;

int l3_my_station_add(const uint8_t *mac, int vlan)
{
    MY_STATION_TCAMm_t my;
    uint32_t mac_fval[2];
    uint32_t mac_mask[2] = { 0xffffffff, 0x0000ffff };
    int idx, rv;

    if (l3_my_station_idx > MY_STATION_TCAMm_MAX) {
        syslog(LOG_ERR, "MY_STATION_TCAM full at idx %d", l3_my_station_idx);
        return -1;
    }
    idx = l3_my_station_idx++;

    MY_STATION_TCAMm_CLR(my);

    mac_to_fval(mac, mac_fval);
    MY_STATION_TCAMm_MAC_ADDRf_SET(my, mac_fval);
    MY_STATION_TCAMm_VLAN_IDf_SET(my, vlan);

    /* Mask: exact match on MAC+VID, ANY ingress port. */
    MY_STATION_TCAMm_MAC_ADDR_MASKf_SET(my, mac_mask);
    MY_STATION_TCAMm_VLAN_ID_MASKf_SET(my, 0xfff);
    MY_STATION_TCAMm_ING_PORT_NUM_MASKf_SET(my, 0);

    /* Allow IPv4 and IPv6 termination on this MAC. */
    MY_STATION_TCAMm_IPV4_TERMINATION_ALLOWEDf_SET(my, 1);
    MY_STATION_TCAMm_IPV6_TERMINATION_ALLOWEDf_SET(my, 1);

    MY_STATION_TCAMm_VALIDf_SET(my, 1);

    rv = WRITE_MY_STATION_TCAMm(edged.unit, idx, my);
    if (rv < 0) {
        syslog(LOG_ERR, "MY_STATION_TCAM[%d] write failed: %d", idx, rv);
        return -1;
    }
    syslog(LOG_INFO,
           "MY_STATION_TCAM[%d] = %02x:%02x:%02x:%02x:%02x:%02x VID=%d "
           "(IPv4+IPv6 terminate)",
           idx, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], vlan);
    return 0;
}

int l3_init(void)
{
    int ioerr = 0;
    CPU_CONTROL_1r_t cpuc1;

    syslog(LOG_INFO, "L3 routing initialized (software forwarding mode)");

    /*
     * Trap L3 lookup misses to CPU.
     *
     * With MY_STATION_TCAM entries programmed for each swpN MAC,
     * the chip enters the L3 pipeline for IPv4/IPv6 unicast frames
     * addressed to one of our MACs.  But we don't program L3 host
     * routes (the bcm_l3_host_add path isn't implemented), so the
     * L3 lookup always misses.  By default the chip silently drops
     * on miss.
     *
     * V4L3DSTMISS_TOCPU / V6L3DSTMISS_TOCPU change that: on miss,
     * trap the frame to CPU instead of dropping.  Linux then runs
     * its normal IP stack on the punted frame and answers (ICMP
     * reply, route the packet, etc.).
     *
     * UUCAST_TOCPU is the L2 equivalent: copy unknown unicast to
     * CPU (covers the case where a frame arrives whose dst MAC
     * isn't in the L2 table — e.g. before MAC learning settles).
     */
    ioerr += READ_CPU_CONTROL_1r(edged.unit, &cpuc1);
    CPU_CONTROL_1r_V4L3DSTMISS_TOCPUf_SET(cpuc1, 1);
    CPU_CONTROL_1r_V6L3DSTMISS_TOCPUf_SET(cpuc1, 1);
    CPU_CONTROL_1r_UUCAST_TOCPUf_SET(cpuc1, 1);
    ioerr += WRITE_CPU_CONTROL_1r(edged.unit, cpuc1);
    if (ioerr) {
        syslog(LOG_WARNING,
               "L3: CPU_CONTROL_1 write returned ioerr=%d (continuing)",
               ioerr);
    } else {
        syslog(LOG_INFO,
               "L3: CPU_CONTROL_1 V4/V6 L3 DST miss + L2 UUCAST -> CPU");
    }

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
