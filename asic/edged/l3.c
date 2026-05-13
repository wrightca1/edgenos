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
#include "vlan.h"   /* edged_resv_vid_for_port */

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
    syslog(LOG_INFO, "L3: CPU_CONTROL_1 before = 0x%08x", cpuc1.cpu_control_1[0]);
    CPU_CONTROL_1r_V4L3DSTMISS_TOCPUf_SET(cpuc1, 1);
    CPU_CONTROL_1r_V6L3DSTMISS_TOCPUf_SET(cpuc1, 1);
    CPU_CONTROL_1r_UUCAST_TOCPUf_SET(cpuc1, 1);
    ioerr += WRITE_CPU_CONTROL_1r(edged.unit, cpuc1);

    /* Read back to confirm the write actually stuck.  Some chip registers
     * are SCHAN-only and silently fail if the BLKTYPE is wrong; some are
     * read-write but only certain bits are mutable. */
    {
        CPU_CONTROL_1r_t verify;
        int rb = READ_CPU_CONTROL_1r(edged.unit, &verify);
        syslog(LOG_INFO,
               "L3: CPU_CONTROL_1 after  = 0x%08x  (V4=%d V6=%d UUCAST=%d rb=%d)",
               verify.cpu_control_1[0],
               CPU_CONTROL_1r_V4L3DSTMISS_TOCPUf_GET(verify),
               CPU_CONTROL_1r_V6L3DSTMISS_TOCPUf_GET(verify),
               CPU_CONTROL_1r_UUCAST_TOCPUf_GET(verify),
               rb);
    }
    if (ioerr) {
        syslog(LOG_WARNING,
               "L3: CPU_CONTROL_1 write ioerr=%d (continuing)", ioerr);
    }

    /* Also read back MY_STATION_TCAM[0] and [1] (swp1, swp2) to verify
     * the encoding matches what the chip actually stored. */
    {
        MY_STATION_TCAMm_t my;
        uint32_t mac_fval[2] = {0, 0};
        int rb = READ_MY_STATION_TCAMm(edged.unit, 1, &my);
        MY_STATION_TCAMm_MAC_ADDRf_GET(my, mac_fval);
        syslog(LOG_INFO,
               "L3: MY_STATION_TCAM[1] = mac=%04x:%08x vid=%d "
               "valid=%d ipv4_term=%d (rb=%d)",
               (unsigned)mac_fval[1], (unsigned)mac_fval[0],
               MY_STATION_TCAMm_VLAN_IDf_GET(my),
               MY_STATION_TCAMm_VALIDf_GET(my),
               MY_STATION_TCAMm_IPV4_TERMINATION_ALLOWEDf_GET(my),
               rb);
    }

    /* Enable per-port L3 IPv4/IPv6 forwarding via LPORT_TAB.
     *
     * Without LPORT_TAB.V4L3_ENABLE=1 on the ingress port, the chip
     * never enters the L3 pipeline for IPv4 frames received on that
     * port — MY_STATION_TCAM is irrelevant because it's queried only
     * inside the L3 pipeline.  Iterating over all 52 front-panel ports
     * and the CPU port to make sure the V4/V6 L3 ENABLE bits are set. */
    {
        int p, enabled = 0;
        for (p = 0; p < EDGED_MAX_PORTS; p++) {
            int phys = edged.ports[p].physical_lane;
            if (!edged.ports[p].valid || phys <= 0)
                continue;
            if (phys > LPORT_TABm_MAX) continue;

            LPORT_TABm_t lp;
            int rb = READ_LPORT_TABm(edged.unit, phys, &lp);
            uint32_t was = lp.lport_tab[0];
            LPORT_TABm_V4L3_ENABLEf_SET(lp, 1);
            LPORT_TABm_V6L3_ENABLEf_SET(lp, 1);
            int wr = WRITE_LPORT_TABm(edged.unit, phys, lp);
            if (rb || wr) {
                syslog(LOG_WARNING,
                       "LPORT_TAB[%d] rb=%d wr=%d", phys, rb, wr);
            } else {
                if (p < 3 || phys == 66) {
                    syslog(LOG_INFO,
                           "LPORT_TAB[%d] (%s): was=0x%08x now=0x%08x "
                           "V4=1 V6=1", phys, edged.ports[p].ifname,
                           was, lp.lport_tab[0]);
                }
                enabled++;
            }
        }
        syslog(LOG_INFO, "L3: enabled V4/V6 L3 on %d ports", enabled);
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

/*
 * Index allocation for the four-table L3 chain.
 *
 *   L3_ENTRY_IPV4_UNICAST  [hash of {ip, vrf}] -> IPV4UC_NEXT_HOP_INDEX
 *   ING_L3_NEXT_HOP        [next_hop_idx]      -> egress port info
 *   EGR_L3_NEXT_HOP        [next_hop_idx]      -> dst MAC, L3_INTF_NUM
 *   EGR_L3_INTF            [l3_intf_num]       -> src MAC, VID
 *
 * One next-hop index per kernel neighbor.
 * One L3_INTF index per L3 interface (one per swpN).  We use
 *   l3_intf_num = logical_port (1..52), since the chip has 4096
 *   L3_INTFm entries.
 */
static int next_hop_idx = 1;   /* index 0 reserved as 'invalid' */

/* Map a swpN logical port to its L3_INTF index.  Simple 1:1. */
static int l3_intf_for_logical_port(int logical_port)
{
    return logical_port;
}

/*
 * Ensure EGR_L3_INTF for the given swpN is programmed.
 * Idempotent: writing the same data again is fine.
 */
static int l3_egr_intf_program(int logical_port, const uint8_t *src_mac, int vid)
{
    EGR_L3_INTFm_t intf;
    uint32_t mac_fval[2];
    int idx = l3_intf_for_logical_port(logical_port);

    EGR_L3_INTFm_CLR(intf);
    mac_to_fval(src_mac, mac_fval);
    EGR_L3_INTFm_MAC_ADDRESSf_SET(intf, mac_fval);
    EGR_L3_INTFm_VIDf_SET(intf, vid);

    int rv = WRITE_EGR_L3_INTFm(edged.unit, idx, intf);
    if (rv < 0) {
        syslog(LOG_WARNING, "EGR_L3_INTF[%d] write failed: %d", idx, rv);
        return -1;
    }
    return idx;
}

int l3_host_add(int family, const void *addr, const uint8_t *mac, int ifindex)
{
    char addr_str[INET6_ADDRSTRLEN] = "?";
    char ifname[IFNAMSIZ] = "?";
    int swp, port_idx, logical_port, phys, vid, nh_idx, intf_idx;
    uint32_t mac_fval[2];
    int rv = 0;

    inet_ntop(family, addr, addr_str, sizeof(addr_str));
    if (ifindex)
        if_indextoname(ifindex, ifname);

    if (family != AF_INET) {
        /* IPv6 path not implemented yet — kernel still routes via TUN. */
        syslog(LOG_DEBUG, "L3 host add (v6 not chip-routed): %s dev %s",
               addr_str, ifname);
        return 0;
    }

    /* Map dev "swpN" -> our port_state -> physical_lane + service VID. */
    if (strncmp(ifname, "swp", 3) != 0) {
        syslog(LOG_DEBUG, "L3 host add: %s on non-swp dev %s, skipping chip",
               addr_str, ifname);
        return 0;
    }
    swp = atoi(ifname + 3);
    if (swp < 1 || swp > EDGED_MAX_PORTS)
        return -1;
    port_idx = swp - 1;
    logical_port = edged.ports[port_idx].logical_port;
    phys = edged.ports[port_idx].physical_lane;
    vid = edged_resv_vid_for_port(logical_port);  /* service VID per port */

    /* 1) EGR_L3_INTF — needs the swpN's MAC as src MAC for routed
     * packets exiting this L3 interface.  Get the MAC from
     * /sys/class/net/swpN/address. */
    uint8_t our_mac[6];
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname);
    FILE *f = fopen(path, "r");
    if (!f) {
        syslog(LOG_WARNING, "L3 host add: can't read %s", path);
        return -1;
    }
    unsigned int m[6];
    if (fscanf(f, "%x:%x:%x:%x:%x:%x",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        fclose(f);
        syslog(LOG_WARNING, "L3 host add: can't parse MAC from %s", path);
        return -1;
    }
    fclose(f);
    for (int i = 0; i < 6; i++) our_mac[i] = (uint8_t)m[i];

    intf_idx = l3_egr_intf_program(logical_port, our_mac, vid);
    if (intf_idx < 0)
        return -1;

    /* 2) Allocate a next-hop index and program both halves. */
    nh_idx = next_hop_idx++;

    /* Ingress L3 next-hop — tells egress port + module for routed pkt.
     * We need to encode the physical port into the chip's "MODULE+PORT"
     * format.  For local-only forwarding mod=0, port=phys. */
    {
        ING_L3_NEXT_HOPm_t ing;
        ING_L3_NEXT_HOPm_CLR(ing);
        /* The chip's "PORT_NUM" + "MODULE_ID" fields together identify
         * where the frame egresses.  For our chassis everything is local
         * (single switch) so MODULE_ID=0. */
        ING_L3_NEXT_HOPm_PORT_NUMf_SET(ing, phys);
        ING_L3_NEXT_HOPm_MODULE_IDf_SET(ing, 0);
        ING_L3_NEXT_HOPm_ENTRY_TYPEf_SET(ing, 0);   /* L3 unicast */
        rv = WRITE_ING_L3_NEXT_HOPm(edged.unit, nh_idx, ing);
        if (rv < 0) {
            syslog(LOG_WARNING,
                   "ING_L3_NEXT_HOP[%d] write failed: %d", nh_idx, rv);
            return -1;
        }
    }

    /* Egress L3 next-hop — dst MAC + L3_INTF_NUM pointing at our
     * EGR_L3_INTF entry.
     *
     * Disable VLAN tag manipulation on this next-hop: the chip's
     * per-port VLAN egress profile already strips the service VID
     * tag for untagged members (which is what we want).  If we let
     * the L3 pipeline force its own OVID, the wire frame ends up
     * tagged when the Nexus's L3 routed eth1/34 expects untagged.
     */
    {
        EGR_L3_NEXT_HOPm_t egr;
        EGR_L3_NEXT_HOPm_CLR(egr);
        mac_to_fval(mac, mac_fval);
        EGR_L3_NEXT_HOPm_L3_MAC_ADDRESSf_SET(egr, mac_fval);
        EGR_L3_NEXT_HOPm_L3_INTF_NUMf_SET(egr, intf_idx);
        EGR_L3_NEXT_HOPm_L3_OVIDf_SET(egr, vid);
        EGR_L3_NEXT_HOPm_ENTRY_TYPEf_SET(egr, 0);          /* L3 unicast */
        EGR_L3_NEXT_HOPm_L3_L3_UC_VLAN_DISABLEf_SET(egr, 1); /* don't add tag */
        rv = WRITE_EGR_L3_NEXT_HOPm(edged.unit, nh_idx, egr);
        if (rv < 0) {
            syslog(LOG_WARNING,
                   "EGR_L3_NEXT_HOP[%d] write failed: %d", nh_idx, rv);
            return -1;
        }
    }

    /* 3) L3 host entry: map host IP -> our nh_idx. */
    {
        L3_ENTRY_IPV4_UNICASTm_t hst;
        const uint8_t *ipb = (const uint8_t *)addr;
        uint32_t ip_be = ((uint32_t)ipb[0] << 24) | ((uint32_t)ipb[1] << 16)
                       | ((uint32_t)ipb[2] <<  8) |  (uint32_t)ipb[3];

        L3_ENTRY_IPV4_UNICASTm_CLR(hst);
        L3_ENTRY_IPV4_UNICASTm_KEY_TYPEf_SET(hst, 0);   /* unicast host key */
        L3_ENTRY_IPV4_UNICASTm_V6f_SET(hst, 0);
        L3_ENTRY_IPV4_UNICASTm_IPV4UC_IP_ADDRf_SET(hst, ip_be);
        L3_ENTRY_IPV4_UNICASTm_IPV4UC_VRF_IDf_SET(hst, 0);
        L3_ENTRY_IPV4_UNICASTm_IPV4UC_NEXT_HOP_INDEXf_SET(hst, nh_idx);
        L3_ENTRY_IPV4_UNICASTm_HITf_SET(hst, 0);
        L3_ENTRY_IPV4_UNICASTm_VALIDf_SET(hst, 1);

        /* Pick an index by hashing IP and trying that slot.  For now
         * just use nh_idx as the slot — works as long as no two
         * neighbors collide there. */
        rv = WRITE_L3_ENTRY_IPV4_UNICASTm(edged.unit, nh_idx, hst);
        if (rv < 0) {
            syslog(LOG_WARNING,
                   "L3_ENTRY_IPV4_UNICAST[%d] write failed: %d",
                   nh_idx, rv);
            return -1;
        }
    }

    syslog(LOG_INFO,
           "L3 host add: %s -> %02x:%02x:%02x:%02x:%02x:%02x via %s "
           "(nh_idx=%d intf_idx=%d vid=%d port=%d)",
           addr_str,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           ifname, nh_idx, intf_idx, vid, phys);
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
