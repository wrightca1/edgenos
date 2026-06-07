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
#include <unistd.h>
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
#include <cdk/arch/xgs_schan.h>
#include <cdk/arch/xgs_chip.h>

/*
 * SCHAN-mediated hash insert/lookup for L3_ENTRY_IPV4_UNICASTm.
 *
 * L3_ENTRY_IPV4_UNICASTm is a hash-indexed table — the chip computes
 * the bucket from {IP, VRF}.  Blind MEM_WRITE to indices 0..MAX does
 * NOT make the lookup hit because the chip's hash machinery only
 * compares against the bucket(s) its hash function picks.  The
 * chip-side opcodes TABLE_INSERT_CMD_MSG / TABLE_LOOKUP_CMD_MSG let
 * the chip find/place the entry itself.
 *
 * Pattern stolen from bmd_port_mac_addr_add (L2X hash insert) — same
 * chip family, same SCHAN op layout, just a different target table.
 */
static int l3_v4_schan_insert(int unit, L3_ENTRY_IPV4_UNICASTm_t *e)
{
    schan_msg_t schan_msg;
    int ipipe_blk = cdk_xgs_block_number(unit, BLKTYPE_IPIPE, 0);
    int rv, type;

    if (ipipe_blk < 0)
        return -1;

    SCHAN_MSG_CLEAR(&schan_msg);
    SCMH_OPCODE_SET(schan_msg.gencmd.header, TABLE_INSERT_CMD_MSG);
    SCMH_SRCBLK_SET(schan_msg.gencmd.header, CDK_XGS_CMIC_BLOCK(unit));
    SCMH_DSTBLK_SET(schan_msg.gencmd.header, ipipe_blk);
    SCMH_DATALEN_SET(schan_msg.gencmd.header, 12);  /* 3 words */
    schan_msg.gencmd.address = L3_ENTRY_IPV4_UNICASTm;
    CDK_MEMCPY(schan_msg.gencmd.data, &e->_l3_entry_ipv4_unicast, 12);

    /* writes = 1 (header) + 1 (address) + 3 (data) = 5
     * reads  = 1 (header) + 1 (response status word)    = 2 */
    rv = cdk_xgs_schan_op(unit, &schan_msg, 5, 2);
    if (rv < 0)
        return rv;

    type = SCGR_TYPE_GET(schan_msg.genresp.response);
    if (type == SCGR_TYPE_INSERTED || type == SCGR_TYPE_REPLACED)
        return SCGR_INDEX_GET(schan_msg.genresp.response);
    if (type == SCGR_TYPE_FULL)
        return -2;
    return -3;
}

static int l3_v4_schan_lookup(int unit, L3_ENTRY_IPV4_UNICASTm_t *key,
                              L3_ENTRY_IPV4_UNICASTm_t *out)
{
    schan_msg_t schan_msg;
    int ipipe_blk = cdk_xgs_block_number(unit, BLKTYPE_IPIPE, 0);
    int rv, type;

    if (ipipe_blk < 0)
        return -1;

    SCHAN_MSG_CLEAR(&schan_msg);
    SCMH_OPCODE_SET(schan_msg.gencmd.header, TABLE_LOOKUP_CMD_MSG);
    SCMH_SRCBLK_SET(schan_msg.gencmd.header, CDK_XGS_CMIC_BLOCK(unit));
    SCMH_DSTBLK_SET(schan_msg.gencmd.header, ipipe_blk);
    SCMH_DATALEN_SET(schan_msg.gencmd.header, 12);
    schan_msg.gencmd.address = L3_ENTRY_IPV4_UNICASTm;
    CDK_MEMCPY(schan_msg.gencmd.data, &key->_l3_entry_ipv4_unicast, 12);

    rv = cdk_xgs_schan_op(unit, &schan_msg, 5, 5);
    if (rv < 0)
        return rv;

    type = SCGR_TYPE_GET(schan_msg.genresp.response);
    if (type == SCGR_TYPE_NOT_FOUND)
        return -2;
    if (out)
        CDK_MEMCPY(&out->_l3_entry_ipv4_unicast, schan_msg.genresp.data, 12);
    return SCGR_INDEX_GET(schan_msg.genresp.response);
}

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

    /* FIX #2 (L2-punt) test toggle: if /tmp/no_mystation exists, skip the
     * MY_STATION entry so inbound IPv4 to our MAC is NOT L3-terminated — it
     * stays L2 and punts to the CPU via the {swpN MAC, service VID}->CPU L2
     * entry (the same path ARP already uses).  This sidesteps the L3-host
     * lookup miss (RIPD4 drop) entirely.  Remove the file + restart edged to
     * return to the L3-termination path. */
    if (access("/tmp/no_mystation", F_OK) == 0) {
        syslog(LOG_INFO,
               "MY_STATION: SKIPPED for %02x:%02x:%02x:%02x:%02x:%02x VID=%d "
               "(/tmp/no_mystation present -> L2-punt mode)",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], vlan);
        return 0;
    }

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

    /* Additional TOCPU traps — these are what Cumulus also enables (captured
     * 2026-05-13 from Cumulus 2.5.0 live CPU_CONTROL_1 state).  Together they
     * let routing protocols (OSPF/BGP), ICMP, and slowpath traffic reach the
     * kernel without needing FP/ACL TCAM rules. */
    CPU_CONTROL_1r_UMC_TOCPUf_SET(cpuc1, 1);             /* unknown multicast (OSPF Hello 224.0.0.5) */
    CPU_CONTROL_1r_IPMCPORTMISS_TOCPUf_SET(cpuc1, 1);    /* IPMC L3 lookup port-miss */
    CPU_CONTROL_1r_L3_SLOWPATH_TOCPUf_SET(cpuc1, 1);     /* L3 slowpath (options, fragments) */
    CPU_CONTROL_1r_L3_MTU_FAIL_TOCPUf_SET(cpuc1, 1);     /* MTU fail — needed for path-MTU discovery */
    CPU_CONTROL_1r_L3UC_TTL_ERR_TOCPUf_SET(cpuc1, 1);    /* TTL=1 unicast → trace route etc */
    CPU_CONTROL_1r_IPMC_TTL_ERR_TOCPUf_SET(cpuc1, 1);    /* TTL fail multicast */
    CPU_CONTROL_1r_V4L3ERR_TOCPUf_SET(cpuc1, 1);         /* generic IPv4 L3 errors */
    CPU_CONTROL_1r_V6L3ERR_TOCPUf_SET(cpuc1, 1);         /* generic IPv6 L3 errors */
    CPU_CONTROL_1r_MARTIAN_ADDR_TOCPUf_SET(cpuc1, 1);    /* martian source → kernel logs */
    CPU_CONTROL_1r_UNRESOLVEDL3SRC_TOCPUf_SET(cpuc1, 1); /* unresolved L3 src → ARP solicit */
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

    /* Enable per-port L3 IPv4/IPv6 forwarding in PORT_TABm.
     *
     * THE GATE for the L3 route lookup (root cause of the cold-boot
     * ICMP-to-self drop, found 2026-06-04 via BCM SDK study + Cumulus dump):
     * the per-port "do IPv4/IPv6 L3" enable lives in PORT_TABm[logical_port]
     * (V4L3_ENABLE/V6L3_ENABLE), the same per-port table that holds PORT_VID.
     * We previously set these in LPORT_TABm — but LPORT_TAB is a 128-entry
     * PROFILE table indexed by SOURCE_TRUNK_MAP_TABLE.LPORT_PROFILE_IDX (=0 for
     * all our ports), NOT the per-port table, so the enable never reached the
     * ingress port -> the chip never entered the L3 pipeline -> the DEFIP/
     * L3_ENTRY search never ran (a match-ALL DEFIP entry's HIT bit stayed 0)
     * -> inbound IPv4 to our own IP was RIPD4-discarded.  Cumulus's live dump
     * confirms: PORT_TAB[1..52].V4L3_ENABLE=1, LPORT_TAB none.
     * Read-modify-write to preserve PORT_VID/FILTER_ENABLE already set there. */
    {
        int p, enabled = 0;
        for (p = 0; p < EDGED_MAX_PORTS; p++) {
            int lport = edged.ports[p].logical_port;
            if (!edged.ports[p].valid || lport <= 0)
                continue;
            if (lport > PORT_TABm_MAX) continue;

            PORT_TABm_t pt;
            int rb = READ_PORT_TABm(edged.unit, lport, &pt);
            int was4 = PORT_TABm_V4L3_ENABLEf_GET(pt);
            PORT_TABm_V4L3_ENABLEf_SET(pt, 1);
            PORT_TABm_V6L3_ENABLEf_SET(pt, 1);
            int wr = WRITE_PORT_TABm(edged.unit, lport, pt);
            if (rb || wr) {
                syslog(LOG_WARNING,
                       "PORT_TAB[lport=%d] L3-enable rb=%d wr=%d", lport, rb, wr);
            } else {
                if (p < 3) {
                    syslog(LOG_INFO,
                           "PORT_TAB[lport=%d] (%s): V4L3 was=%d now=1 V6L3=1 "
                           "(PORT_VID=%d)", lport, edged.ports[p].ifname,
                           was4, PORT_TABm_PORT_VIDf_GET(pt));
                }
                enabled++;
            }
        }
        syslog(LOG_INFO, "L3: enabled V4/V6 L3 on %d ports (logical-indexed)",
               enabled);
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

/*
 * Gateway -> chip next-hop index map.  Populated by l3_host_add() whenever the
 * kernel resolves a neighbor (ARP/ND) on a swpN port.  ECMP route programming
 * looks up each gateway IP here to find the chip next-hop to put in the group.
 * Host byte order (MSB = first octet), matching l3_local_host_add()'s convention.
 */
#define L3_NEIGH_MAX 256
static struct { uint32_t ip; int nh_idx; int valid; } l3_neigh_nh[L3_NEIGH_MAX];

static void l3_neigh_nh_record(uint32_t ip_host, int nh_idx)
{
    int free_slot = -1;
    for (int i = 0; i < L3_NEIGH_MAX; i++) {
        if (l3_neigh_nh[i].valid && l3_neigh_nh[i].ip == ip_host) {
            l3_neigh_nh[i].nh_idx = nh_idx;   /* refresh */
            return;
        }
        if (free_slot < 0 && !l3_neigh_nh[i].valid)
            free_slot = i;
    }
    if (free_slot >= 0) {
        l3_neigh_nh[free_slot].ip = ip_host;
        l3_neigh_nh[free_slot].nh_idx = nh_idx;
        l3_neigh_nh[free_slot].valid = 1;
    }
}

static int l3_neigh_nh_lookup(uint32_t ip_host)
{
    for (int i = 0; i < L3_NEIGH_MAX; i++)
        if (l3_neigh_nh[i].valid && l3_neigh_nh[i].ip == ip_host)
            return l3_neigh_nh[i].nh_idx;
    return -1;
}

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

/*
 * Add a CPU-bound L3 host route for one of our own swpN IPv4 addresses.
 *
 * BCM Trident default behaviour: when MY_STATION_TCAM matches the dst
 * MAC and the L3 lookup misses, V4L3DSTMISS_TOCPU=1 *should* trap to
 * CPU.  In practice on this chip (verified by `rx_drops` incrementing
 * exactly N times when Nexus sends N pings to us), the miss-trap path
 * just drops the frame.  Cumulus avoids that by programming *its own*
 * IP as an L3_HOST entry whose next-hop egresses to the CPU port,
 * making the L3 lookup HIT instead of miss.
 */
int l3_local_host_add(uint32_t ipv4_addr, int logical_port)
{
    int nh_idx, intf_idx, vid, rv;
    uint8_t our_mac[6];
    uint32_t mac_fval[2];
    int port_idx = logical_port - 1;
    int phys;

    if (port_idx < 0 || port_idx >= EDGED_MAX_PORTS)
        return -1;
    phys = edged.ports[port_idx].physical_lane;
    vid = edged_resv_vid_for_port(logical_port);

    /* Read swpN's MAC. */
    char ifname[IFNAMSIZ];
    snprintf(ifname, sizeof(ifname), "swp%d", logical_port);
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    unsigned int m[6];
    if (fscanf(f, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        fclose(f); return -1;
    }
    fclose(f);
    for (int i = 0; i < 6; i++) our_mac[i] = (uint8_t)m[i];

    /* Reuse the same EGR_L3_INTF index as the swpN's other host
     * entries — src MAC + service VID are identical. */
    intf_idx = l3_egr_intf_program(logical_port, our_mac, vid);
    if (intf_idx < 0) return -1;

    nh_idx = next_hop_idx++;

    /* ING_L3_NEXT_HOP with COPY_TO_CPU=1.  This is the chip-side
     * equivalent of bcm_l3_host_add with BCM_L3_COPY_TO_CPU flag:
     * the chip's L3 lookup hit on our own IP and this next-hop
     * carries the frame straight to the CMIC queue our RX DMA
     * is polling.  Without COPY_TO_CPU, the chip merely picked
     * the egress port number 0 from PORT_NUM and tried to
     * "transmit" there as if it were a regular Ethernet port —
     * frame went nowhere (rx_drops stayed 0 but no CPU RX). */
    {
        ING_L3_NEXT_HOPm_t ing;
        ING_L3_NEXT_HOPm_CLR(ing);
        ING_L3_NEXT_HOPm_COPY_TO_CPUf_SET(ing, 1);    /* punt to CPU */
        ING_L3_NEXT_HOPm_PORT_NUMf_SET(ing, 0);       /* CMIC */
        ING_L3_NEXT_HOPm_MODULE_IDf_SET(ing, 0);
        ING_L3_NEXT_HOPm_ENTRY_TYPEf_SET(ing, 0);
        rv = WRITE_ING_L3_NEXT_HOPm(edged.unit, nh_idx, ing);
        if (rv < 0) {
            syslog(LOG_WARNING, "local-host ING_L3_NEXT_HOP[%d] wr=%d",
                   nh_idx, rv); return -1;
        }
    }

    /* EGR_L3_NEXT_HOP: dst MAC = our own MAC (so the frame egresses
     * to CPU port still has a sane L2 header), VLAN/SA/DA/TTL
     * rewrites all DISABLED — the chip should pass the frame through
     * unmodified for CPU delivery. */
    {
        EGR_L3_NEXT_HOPm_t egr;
        EGR_L3_NEXT_HOPm_CLR(egr);
        mac_to_fval(our_mac, mac_fval);
        EGR_L3_NEXT_HOPm_L3_MAC_ADDRESSf_SET(egr, mac_fval);
        EGR_L3_NEXT_HOPm_L3_INTF_NUMf_SET(egr, intf_idx);
        EGR_L3_NEXT_HOPm_L3_OVIDf_SET(egr, vid);
        EGR_L3_NEXT_HOPm_ENTRY_TYPEf_SET(egr, 0);
        EGR_L3_NEXT_HOPm_L3_L3_UC_VLAN_DISABLEf_SET(egr, 1);
        EGR_L3_NEXT_HOPm_L3_L3_UC_SA_DISABLEf_SET(egr, 1);
        EGR_L3_NEXT_HOPm_L3_L3_UC_DA_DISABLEf_SET(egr, 1);
        EGR_L3_NEXT_HOPm_L3_L3_UC_TTL_DISABLEf_SET(egr, 1);
        rv = WRITE_EGR_L3_NEXT_HOPm(edged.unit, nh_idx, egr);
        if (rv < 0) {
            syslog(LOG_WARNING, "local-host EGR_L3_NEXT_HOP[%d] wr=%d",
                   nh_idx, rv); return -1;
        }
    }

    /* FIX #1: L3_DEFIP /32 -> CPU (the Cumulus recipe).
     *
     * The L3_ENTRY *hash* entry below round-trips via schan ("lookup OK") but
     * the hardware datapath lookup misses it -> inbound IPv4 to our own IP is
     * discarded as RIPD4 (verified 2026-06-04: rdbgc3 +N per N pings, ingress
     * VRF confirmed 0 so it is NOT a VRF mismatch).  Cumulus avoids the hash
     * path entirely and installs our own /32 in the L3_DEFIP TCAM (deterministic,
     * no hashing).  We mirror that here: one /32 entry per local host -> the same
     * COPY_TO_CPU next-hop (nh_idx).
     *
     * Field encoding replicated from a live Cumulus L3_DEFIP dump (entry 2564,
     * 10.101.101.1/32 -> TO_CPU): half 0 used, half 1 unused; VRF 0 exact-match
     * (VRF_ID_MASK0=0x3ff); IP_ADDR0/IP_ADDR_MASK0 give a /32; MODE0=0/MODE_MASK0=1.
     * KEY0/MASK0 are composite views over MODE+IP+VRF, so setting the structured
     * fields sets them automatically. */
    {
        static int defip_slot = 2560;  /* Cumulus /32 band base */
        L3_DEFIPm_t d;
        int slot = defip_slot++;
        L3_DEFIPm_CLR(d);
        L3_DEFIPm_VALID0f_SET(d, 1);
        L3_DEFIPm_MODE0f_SET(d, 0);
        L3_DEFIPm_MODE_MASK0f_SET(d, 1);
        L3_DEFIPm_IP_ADDR0f_SET(d, ipv4_addr);
        L3_DEFIPm_IP_ADDR_MASK0f_SET(d, 0xffffffff);
        L3_DEFIPm_VRF_ID_0f_SET(d, 0);
        L3_DEFIPm_VRF_ID_MASK0f_SET(d, 0x3ff);
        L3_DEFIPm_NEXT_HOP_INDEX0f_SET(d, nh_idx);
        /* half 1 left unused (VALID1=0, MODE_MASK1=0) by CLR. */
        rv = WRITE_L3_DEFIPm(edged.unit, slot, d);
        syslog(LOG_INFO,
               "local-host L3_DEFIP[%d] /32 %u.%u.%u.%u -> nh_idx=%d (CPU) wr=%d",
               slot,
               (ipv4_addr >> 24) & 0xff, (ipv4_addr >> 16) & 0xff,
               (ipv4_addr >> 8) & 0xff, ipv4_addr & 0xff, nh_idx, rv);
    }

    /* DIAGNOSTIC (one-shot, logical_port 1 only): a CATCH-ALL L3_DEFIP entry
     * that matches ANY routed IPv4 in ANY VRF (IP mask=0, VRF mask=0) -> the
     * same COPY_TO_CPU next-hop.  Decides whether the DEFIP TCAM is consulted at
     * all: if inbound IPv4 then punts (RIPD4 stops), the lookup works and the
     * earlier /32 miss was a key/VRF detail; if it STILL RIPD4s, the L3 dst
     * lookup is not being performed -> deeper soc_init gap. Slot 8000 (lowest
     * priority). Remove after diagnosis. */
    if (logical_port == 1) {
        L3_DEFIPm_t d;
        L3_DEFIPm_CLR(d);
        L3_DEFIPm_VALID0f_SET(d, 1);
        L3_DEFIPm_MODE0f_SET(d, 0);
        L3_DEFIPm_MODE_MASK0f_SET(d, 0);         /* match ANY mode too */
        L3_DEFIPm_IP_ADDR0f_SET(d, 0);
        L3_DEFIPm_IP_ADDR_MASK0f_SET(d, 0);      /* match any IP */
        L3_DEFIPm_VRF_ID_0f_SET(d, 0);
        L3_DEFIPm_VRF_ID_MASK0f_SET(d, 0);       /* match any VRF */
        L3_DEFIPm_NEXT_HOP_INDEX0f_SET(d, nh_idx);
        rv = WRITE_L3_DEFIPm(edged.unit, 8000, d);
        syslog(LOG_INFO,
               "DIAG L3_DEFIP[8000] CATCH-ALL (any mode/IP/VRF) -> nh_idx=%d (CPU) wr=%d",
               nh_idx, rv);
    }

    /* L3 host entry: our IP -> nh_idx (-> CPU port).
     *
     * L3_ENTRY_IPV4_UNICASTm is hash-indexed. Use TABLE_INSERT_CMD_MSG
     * (chip computes the bucket itself) instead of blind MEM_WRITE — the
     * chip's hash lookup only checks the bucket(s) determined by its
     * internal hash, so manual MEM_WRITE to all 8192 slots is wasted.
     */
    {
        L3_ENTRY_IPV4_UNICASTm_t hst;
        L3_ENTRY_IPV4_UNICASTm_CLR(hst);
        L3_ENTRY_IPV4_UNICASTm_KEY_TYPEf_SET(hst, 0);
        L3_ENTRY_IPV4_UNICASTm_V6f_SET(hst, 0);
        L3_ENTRY_IPV4_UNICASTm_IPV4UC_IP_ADDRf_SET(hst, ipv4_addr);
        L3_ENTRY_IPV4_UNICASTm_IPV4UC_VRF_IDf_SET(hst, 0);
        L3_ENTRY_IPV4_UNICASTm_IPV4UC_NEXT_HOP_INDEXf_SET(hst, nh_idx);
        L3_ENTRY_IPV4_UNICASTm_HITf_SET(hst, 0);
        L3_ENTRY_IPV4_UNICASTm_VALIDf_SET(hst, 1);

        int idx = l3_v4_schan_insert(edged.unit, &hst);
        syslog(LOG_INFO,
               "local-host L3_HOST: %u.%u.%u.%u -> CPU schan_insert -> %d",
               (ipv4_addr >> 24) & 0xff, (ipv4_addr >> 16) & 0xff,
               (ipv4_addr >> 8) & 0xff, ipv4_addr & 0xff, idx);

        /* Read back via HASH_LOOKUP so we can confirm the chip placed
         * the entry where it's reachable.  If lookup returns NOT_FOUND
         * (-2), the field encoding (KEY_TYPE/VRF/IP_ADDR) doesn't match
         * what the chip hashes against. */
        L3_ENTRY_IPV4_UNICASTm_t key, got;
        L3_ENTRY_IPV4_UNICASTm_CLR(key);
        L3_ENTRY_IPV4_UNICASTm_KEY_TYPEf_SET(key, 0);
        L3_ENTRY_IPV4_UNICASTm_V6f_SET(key, 0);
        L3_ENTRY_IPV4_UNICASTm_IPV4UC_IP_ADDRf_SET(key, ipv4_addr);
        L3_ENTRY_IPV4_UNICASTm_IPV4UC_VRF_IDf_SET(key, 0);
        int lk = l3_v4_schan_lookup(edged.unit, &key, &got);
        if (lk >= 0) {
            syslog(LOG_INFO,
                   "local-host L3_HOST lookup OK: idx=%d nhi=%d valid=%d",
                   lk,
                   L3_ENTRY_IPV4_UNICASTm_IPV4UC_NEXT_HOP_INDEXf_GET(got),
                   L3_ENTRY_IPV4_UNICASTm_VALIDf_GET(got));
        } else {
            syslog(LOG_WARNING,
                   "local-host L3_HOST lookup FAIL rv=%d (encoding mismatch?)",
                   lk);
        }
    }

    syslog(LOG_INFO,
           "L3 local-host: %u.%u.%u.%u -> CPU (nh_idx=%d intf_idx=%d "
           "vid=%d phys=%d swp%d)",
           (ipv4_addr >> 24) & 0xff, (ipv4_addr >> 16) & 0xff,
           (ipv4_addr >> 8) & 0xff, ipv4_addr & 0xff,
           nh_idx, intf_idx, vid, phys, logical_port);
    return 0;
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

        /* SCHAN HASH_INSERT — chip picks the bucket itself based on
         * its internal hash over (IP, VRF, KEY_TYPE). */
        int idx = l3_v4_schan_insert(edged.unit, &hst);
        if (idx < 0) {
            syslog(LOG_WARNING,
                   "L3_ENTRY_IPV4_UNICAST schan_insert failed: %d",
                   idx);
            return -1;
        }
        syslog(LOG_DEBUG, "L3 host schan_insert placed at idx=%d", idx);

        /* Register this resolved neighbor's chip next-hop so ECMP route
         * programming can reference it by gateway IP. */
        l3_neigh_nh_record(ip_be, nh_idx);
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

/*
 * ECMP group programming — mirrors Cumulus's multipath model decoded
 * 2026-05-13 (see project_cumulus_chip_init_complete + l3_chip_format_decoded
 * in memory).
 *
 * Cumulus's model:
 *   - L3_ECMP table has 16K slots.  Each slot holds one NEXT_HOP_INDEX
 *     (pointer into ING_L3_NEXT_HOP and EGR_L3_NEXT_HOP).
 *   - A "group" is a consecutive run of slots in L3_ECMP, e.g. group_base=0
 *     count=2 means slots 0+1 form a 2-way ECMP group.
 *   - L3_DEFIP entries that use ECMP set ECMP_PTR0/1 = group_base and ECMP0/1
 *     = 1 to indicate "use ECMP, look up at L3_ECMP[ECMP_PTR + hash%count]".
 *   - L3_ECMP_COUNT table (1K entries) holds per-group member count.
 *
 * For EdgeNOS we use a very simple bump allocator: groups are appended at the
 * next free slot in L3_ECMP.  No reuse / no fragmentation handling.  Enough
 * for OSPF/BGP multipath in a single small network.
 */
static int l3_ecmp_next_slot = 0;     /* next free L3_ECMP slot to allocate from */
static int l3_ecmp_next_group_id = 1; /* group IDs start at 1; 0 reserved */

/*
 * l3_ecmp_group_create(intf_ids, count) — write `count` consecutive L3_ECMP
 * slots with the given NEXT_HOP_INDEX values, and one L3_ECMP_COUNT entry.
 * Returns the group's base slot index, which is what L3_DEFIP entries put in
 * their ECMP_PTR field.  Returns -1 on error.
 *
 * NB: intf_ids here are CHIP-SIDE next-hop indices (i.e. the value 4 for
 * "INTF 100004").  Caller is responsible for allocating those via the
 * existing l3_egr_intf_program / ING_L3_NEXT_HOP writes.
 */
int l3_ecmp_group_create(const int *intf_ids, int count)
{
    int base = l3_ecmp_next_slot;
    int group_id = l3_ecmp_next_group_id++;
    int i, rv;

    if (count <= 0 || count > 64) {
        syslog(LOG_ERR, "ECMP group_create: invalid count=%d", count);
        return -1;
    }
    if (base + count > L3_ECMPm_MAX + 1) {
        syslog(LOG_ERR, "ECMP table full (base=%d count=%d)", base, count);
        return -1;
    }

    /* Write each L3_ECMP slot. */
    for (i = 0; i < count; i++) {
        L3_ECMPm_t entry;
        L3_ECMPm_CLR(entry);
        L3_ECMPm_NEXT_HOP_INDEXf_SET(entry, intf_ids[i]);
        rv = WRITE_L3_ECMPm(edged.unit, base + i, entry);
        if (rv < 0) {
            syslog(LOG_WARNING, "ECMP slot %d write failed: %d", base + i, rv);
            return -1;
        }
    }

    /* Write the L3_ECMP_COUNT (group) entry.  The chip indexes this table by
     * the L3_DEFIP ECMP_PTR0 field (= group_id here), reads BASE_PTR + COUNT,
     * then selects L3_ECMP[BASE_PTR + (hash % COUNT)].  Encoding verified on a
     * live Cumulus 56846: BASE_PTR_0[21:10], COUNT_0[9:0], COUNT = #members
     * (project_cumulus_route_storage_decoded / L3_NEXTHOP_FORMAT). */
    {
        L3_ECMP_COUNTm_t cnt;
        L3_ECMP_COUNTm_CLR(cnt);
        L3_ECMP_COUNTm_BASE_PTR_0f_SET(cnt, base);
        L3_ECMP_COUNTm_COUNT_0f_SET(cnt, count);
        rv = WRITE_L3_ECMP_COUNTm(edged.unit, group_id, cnt);
        if (rv < 0) {
            syslog(LOG_WARNING, "L3_ECMP_COUNT[%d] write failed: %d",
                   group_id, rv);
            return -1;
        }
    }

    syslog(LOG_INFO, "ECMP group %d: base=%d count=%d members=[%d%s%d%s]",
           group_id, base, count,
           intf_ids[0], count > 1 ? "," : "",
           count > 1 ? intf_ids[1] : 0,
           count > 2 ? ",..." : "");

    l3_ecmp_next_slot += count;
    return group_id;  /* ECMP_PTR0 value for L3_DEFIP (index into L3_ECMP_COUNT) */
}

/*
 * l3_route_add_paths() — program a transit IPv4 prefix route into L3_DEFIP.
 *
 *   dst_host   : destination prefix, host byte order (MSB = first octet)
 *   prefix_len : 0..32
 *   gw_host[]  : nexthop gateway IPs (host byte order), each already resolved
 *                (l3_host_add ran on its ARP) so it has a chip next-hop
 *   ngw        : number of nexthops; 1 = single path, >1 = ECMP
 *
 * For ngw==1 the DEFIP entry points straight at the next-hop. For ngw>1 we
 * build an L3_ECMP group and point the DEFIP entry at it (ECMP0=1). The chip
 * then hashes each flow across the members → load-balance across swp ports.
 */
int l3_route_add_paths(uint32_t dst_host, int prefix_len,
                       const uint32_t *gw_host, int ngw)
{
    static int defip_transit_slot = 1536;   /* transit band: above /32 (2560),
                                               below the /0 catch-all (8000) */
    int nh_idx[64];
    int n = 0, rv;

    if (ngw < 1) return -1;
    if (ngw > 64) ngw = 64;

    /* Resolve each gateway IP to its chip next-hop (programmed by l3_host_add
     * when the kernel ARP'd it). Skip unresolved ones. */
    for (int i = 0; i < ngw; i++) {
        int nh = l3_neigh_nh_lookup(gw_host[i]);
        if (nh < 0) {
            syslog(LOG_WARNING,
                   "route %u.%u.%u.%u/%d: gw %u.%u.%u.%u has no chip next-hop "
                   "(not ARP-resolved yet?) — skipping this path",
                   (dst_host>>24)&0xff,(dst_host>>16)&0xff,(dst_host>>8)&0xff,dst_host&0xff,
                   prefix_len,
                   (gw_host[i]>>24)&0xff,(gw_host[i]>>16)&0xff,
                   (gw_host[i]>>8)&0xff,gw_host[i]&0xff);
            continue;
        }
        nh_idx[n++] = nh;
    }
    if (n == 0) {
        syslog(LOG_WARNING, "route %u.%u.%u.%u/%d: no resolved next-hops, not programmed",
               (dst_host>>24)&0xff,(dst_host>>16)&0xff,(dst_host>>8)&0xff,dst_host&0xff,
               prefix_len);
        return -1;
    }

    uint32_t mask = (prefix_len == 0) ? 0 :
                    (prefix_len >= 32) ? 0xffffffffu :
                    (0xffffffffu << (32 - prefix_len));

    int slot = defip_transit_slot++;
    L3_DEFIPm_t d;
    L3_DEFIPm_CLR(d);
    L3_DEFIPm_VALID0f_SET(d, 1);
    L3_DEFIPm_MODE0f_SET(d, 0);
    L3_DEFIPm_MODE_MASK0f_SET(d, 1);
    L3_DEFIPm_IP_ADDR0f_SET(d, dst_host & mask);
    L3_DEFIPm_IP_ADDR_MASK0f_SET(d, mask);
    L3_DEFIPm_VRF_ID_0f_SET(d, 0);
    L3_DEFIPm_VRF_ID_MASK0f_SET(d, 0x3ff);

    if (n == 1) {
        L3_DEFIPm_ECMP0f_SET(d, 0);
        L3_DEFIPm_NEXT_HOP_INDEX0f_SET(d, nh_idx[0]);
        rv = WRITE_L3_DEFIPm(edged.unit, slot, d);
        syslog(LOG_INFO,
               "route L3_DEFIP[%d] %u.%u.%u.%u/%d -> nh_idx=%d wr=%d",
               slot,(dst_host>>24)&0xff,(dst_host>>16)&0xff,(dst_host>>8)&0xff,
               dst_host&0xff,prefix_len,nh_idx[0],rv);
    } else {
        int grp = l3_ecmp_group_create(nh_idx, n);
        if (grp < 0) return -1;
        L3_DEFIPm_ECMP0f_SET(d, 1);
        L3_DEFIPm_ECMP_PTR0f_SET(d, grp);
        rv = WRITE_L3_DEFIPm(edged.unit, slot, d);
        syslog(LOG_INFO,
               "route L3_DEFIP[%d] %u.%u.%u.%u/%d -> ECMP grp=%d (%d paths) wr=%d",
               slot,(dst_host>>24)&0xff,(dst_host>>16)&0xff,(dst_host>>8)&0xff,
               dst_host&0xff,prefix_len,grp,n,rv);
    }
    return rv < 0 ? -1 : 0;
}
