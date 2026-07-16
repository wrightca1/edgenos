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
#include <stdlib.h>
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

/* Remove a host entry from the hashed L3_ENTRY table (chip finds the bucket from
 * the key, same as insert/lookup). Returns 0 on delete or not-found, <0 on error. */
static int l3_v4_schan_delete(int unit, L3_ENTRY_IPV4_UNICASTm_t *key)
{
    schan_msg_t schan_msg;
    int ipipe_blk = cdk_xgs_block_number(unit, BLKTYPE_IPIPE, 0);
    int rv, type;

    if (ipipe_blk < 0)
        return -1;

    SCHAN_MSG_CLEAR(&schan_msg);
    SCMH_OPCODE_SET(schan_msg.gencmd.header, TABLE_DELETE_CMD_MSG);
    SCMH_SRCBLK_SET(schan_msg.gencmd.header, CDK_XGS_CMIC_BLOCK(unit));
    SCMH_DSTBLK_SET(schan_msg.gencmd.header, ipipe_blk);
    SCMH_DATALEN_SET(schan_msg.gencmd.header, 12);
    schan_msg.gencmd.address = L3_ENTRY_IPV4_UNICASTm;
    CDK_MEMCPY(schan_msg.gencmd.data, &key->_l3_entry_ipv4_unicast, 12);

    rv = cdk_xgs_schan_op(unit, &schan_msg, 5, 2);
    if (rv < 0)
        return rv;
    type = SCGR_TYPE_GET(schan_msg.genresp.response);
    if (type == SCGR_TYPE_DELETED || type == SCGR_TYPE_NOT_FOUND)
        return 0;
    return -3;
}

/*
 * IPv6 host entry SCHAN insert/delete. L3_ENTRY_IPV6_UNICAST is a *double-wide*
 * hashed entry (6 words / 24 bytes, vs 3 words for v4), so the SCHAN op carries
 * 24 bytes of key+data. Same machinery as the v4 helpers above.
 */
static int l3_v6_schan_insert(int unit, L3_ENTRY_IPV6_UNICASTm_t *e)
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
    SCMH_DATALEN_SET(schan_msg.gencmd.header, 24);   /* 6 words */
    schan_msg.gencmd.address = L3_ENTRY_IPV6_UNICASTm;
    CDK_MEMCPY(schan_msg.gencmd.data, e->v, 24);

    /* writes = 1 header + 1 address + 6 data = 8; reads = 2 */
    rv = cdk_xgs_schan_op(unit, &schan_msg, 8, 2);
    if (rv < 0)
        return rv;
    type = SCGR_TYPE_GET(schan_msg.genresp.response);
    if (type == SCGR_TYPE_INSERTED || type == SCGR_TYPE_REPLACED)
        return SCGR_INDEX_GET(schan_msg.genresp.response);
    if (type == SCGR_TYPE_FULL)
        return -2;
    return -3;
}

static int l3_v6_schan_delete(int unit, L3_ENTRY_IPV6_UNICASTm_t *key)
{
    schan_msg_t schan_msg;
    int ipipe_blk = cdk_xgs_block_number(unit, BLKTYPE_IPIPE, 0);
    int rv, type;

    if (ipipe_blk < 0)
        return -1;

    SCHAN_MSG_CLEAR(&schan_msg);
    SCMH_OPCODE_SET(schan_msg.gencmd.header, TABLE_DELETE_CMD_MSG);
    SCMH_SRCBLK_SET(schan_msg.gencmd.header, CDK_XGS_CMIC_BLOCK(unit));
    SCMH_DSTBLK_SET(schan_msg.gencmd.header, ipipe_blk);
    SCMH_DATALEN_SET(schan_msg.gencmd.header, 24);
    schan_msg.gencmd.address = L3_ENTRY_IPV6_UNICASTm;
    CDK_MEMCPY(schan_msg.gencmd.data, key->v, 24);

    rv = cdk_xgs_schan_op(unit, &schan_msg, 8, 2);
    if (rv < 0)
        return rv;
    type = SCGR_TYPE_GET(schan_msg.genresp.response);
    if (type == SCGR_TYPE_DELETED || type == SCGR_TYPE_NOT_FOUND)
        return 0;
    return -3;
}

static int l3_v6_schan_lookup(int unit, L3_ENTRY_IPV6_UNICASTm_t *key,
                              L3_ENTRY_IPV6_UNICASTm_t *out)
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
    SCMH_DATALEN_SET(schan_msg.gencmd.header, 24);
    schan_msg.gencmd.address = L3_ENTRY_IPV6_UNICASTm;
    CDK_MEMCPY(schan_msg.gencmd.data, key->v, 24);

    rv = cdk_xgs_schan_op(unit, &schan_msg, 8, 8);
    if (rv < 0)
        return rv;
    type = SCGR_TYPE_GET(schan_msg.genresp.response);
    if (type == SCGR_TYPE_NOT_FOUND)
        return -2;
    if (out)
        CDK_MEMCPY(out->v, schan_msg.genresp.data, 24);
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

    /* NOTE: enabling the L2/L3/MPLS AUX_HASH_CONTROL + L2_LEARN/L2_BULK (all REGDIFF
     * gaps vs Cumulus, soc_init sets them) was TESTED here and (a) did NOT flip transit
     * to HW forwarding (the datapath still missed the L3_ENTRY) and (b) BROKE the OSPF
     * multicast->CPU punt (hellos stopped reaching ospfd). So these soc_init gaps must
     * be applied as a COHERENT SET with the rest (egress/ING/MMU config), not piecemeal.
     * See REGDIFF_edged_vs_cumulus.txt (61 gaps + 81 diffs) + project_init_all_insight. */

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

/*
 * Transit-route DEFIP band. Routes added by l3_route_add_paths() land at
 * incrementing slots starting at defip_transit_base; l3_route_del() scans
 * [defip_transit_base, defip_transit_slot) to find and invalidate one.
 * File-scope so both add and del see the same allocator.
 */
static const int defip_transit_base = 1536;  /* first transit DEFIP slot */
static int defip_transit_slot = 1536;        /* high-water mark of the transit band */

/* ---------------------------------------------------------------------------
 * L3 route lifecycle bookkeeping.
 *
 * Without this, every allocator (DEFIP slot, ECMP group, ECMP member slots) only
 * ever grows and is reclaimed solely on edged restart — so route churn (flaps,
 * OSPF reconvergence) leaks chip-table entries until they exhaust. Here we keep a
 * per-prefix record + free-lists so del() frees what add() allocated, ECMP groups
 * are de-duplicated + refcounted (identical member-sets share one group, so the
 * count is O(unique-member-sets), not O(routes)), and a path signature lets the
 * periodic re-dump skip unchanged routes and correctly re-program changed ones.
 * ------------------------------------------------------------------------- */
#define L3_ROUTE_MAX     8192
#define L3_ECMP_GRP_MAX  1024

struct l3_rt {                 /* one per programmed transit prefix */
    uint32_t target, mask;
    int      slot;             /* L3_DEFIP slot */
    int      is_ecmp;          /* 1 => ref is an ECMP group_id; 0 => a next-hop idx */
    int      ref;              /* ECMP group_id or single next-hop index */
    uint64_t sig;              /* path-set signature (for skip-unchanged) */
    int      valid;
};
static struct l3_rt route_tab[L3_ROUTE_MAX];

/* Released transit DEFIP slots, reused before growing the band. */
static int defip_free[L3_ROUTE_MAX];
static int defip_free_n;

static int defip_slot_alloc(void)
{
    if (defip_free_n > 0) return defip_free[--defip_free_n];
    return defip_transit_slot++;
}
static void defip_slot_free(int slot)
{
    if (defip_free_n < L3_ROUTE_MAX) defip_free[defip_free_n++] = slot;
}

/* ---------------------------------------------------------------------------
 * dst-IP ACL denies via a kernel BLACKHOLE ROUTE.
 *
 * edged SOFTWARE-forwards L3 transit: the chip does L2 + punt-to-CPU, and the
 * Linux kernel FIB does the routing (punt -> route -> inject back out). Verified
 * 2026-07-15 with a real transit peer (host on swp5 -> box -> Nexus on swp1): the
 * forwarded packets ride the CPU path, so chip-level filters (FP *and* L3
 * DST_DISCARD) never see them — but a kernel `blackhole` route in the FIB drops
 * them cleanly (host->Nexus went 6 -> 0 forwarded pkts). netfilter/iptables is not
 * an option (this kernel has no ip_tables module). The blackhole route is simple,
 * robust, matches edged's actual forwarding model, and touches nothing on the chip,
 * so it cannot disturb the datapath or the control plane. Managed via `ip route`.
 * Scope: L3-routed/transit + locally-destined IPv4, global across ingress ports.
 *
 * UPDATE 2026-07-16 (commit 1d0c582 armed the chip L3 lookup): the datapath now
 * HARDWARE-forwards L3 transit (chip consults the programmed L3_ENTRY hash), so
 * such traffic no longer rides the CPU path — the kernel blackhole alone would
 * MISS it. For a /32 deny we therefore also program a chip L3_ENTRY with
 * IPV4UC_DST_DISCARD=1: the L3 lookup hits it and drops the routed packet in
 * silicon (verified via the swp-ingress rdbgc drop counter climbing). We KEEP
 * the kernel blackhole too — belt-and-suspenders for any flow still software-
 * forwarded (e.g. a dst with no armed chip entry / prefix denies below /32).
 * ------------------------------------------------------------------------- */
#define L3_DENY_MAX 128
static struct { uint32_t ip, mask; int valid, chip, defip, slot; } l3_deny_tab[L3_DENY_MAX];

/* Add (add=1) / remove (add=0) a /32 L3_DEFIP DST_DISCARD entry at an explicit
 * TCAM slot. The datapath's L3 lookup uses the L3_DEFIP TCAM (NOT the L3_ENTRY
 * host hash table — verified 2026-07-16: host-table discard entries read back
 * VALID=1 DST_DISCARD=1 but HIT stays 0 and traffic still forwards) — AND only
 * certain bands are consulted: the local-host /32s in the ~2560 band ARE honored
 * (l3_local_host_add), while the transit band (1536) is NOT. So ACL /32 discards
 * live in the SAME consulted 2560 band (base 2580, clear of the local hosts at
 * 2560+). A /32 beats the connected /29 by LPM, so the chip drops. */
#define ACL_DEFIP_DISCARD_BASE 2580
#define ACL_DROP_NH_IDX 4000    /* reserved ING_L3_NEXT_HOP index for ACL drops */
static int acl_drop_nh_ready = 0;

/* One-time: program a DROP next-hop the ACL /32 entries can point at, in case
 * the datapath honors NEXT_HOP_INDEX0.DROP but not the DST_DISCARD0 bit. */
static void acl_ensure_drop_nh(void)
{
    ING_L3_NEXT_HOPm_t ing;
    if (acl_drop_nh_ready)
        return;
    ING_L3_NEXT_HOPm_CLR(ing);
    ING_L3_NEXT_HOPm_DROPf_SET(ing, 1);
    ING_L3_NEXT_HOPm_ENTRY_TYPEf_SET(ing, 0);
    if (WRITE_ING_L3_NEXT_HOPm(edged.unit, ACL_DROP_NH_IDX, ing) >= 0)
        acl_drop_nh_ready = 1;
}

static int l3_v4_defip_discard(uint32_t ip, uint32_t mask, int add, int slot)
{
    L3_DEFIPm_t d;
    L3_DEFIPm_CLR(d);
    if (add) {
        acl_ensure_drop_nh();
        L3_DEFIPm_VALID0f_SET(d, 1);
        L3_DEFIPm_MODE0f_SET(d, 0);
        L3_DEFIPm_MODE_MASK0f_SET(d, 1);
        L3_DEFIPm_IP_ADDR0f_SET(d, ip);
        L3_DEFIPm_IP_ADDR_MASK0f_SET(d, mask);
        L3_DEFIPm_VRF_ID_0f_SET(d, 0);
        L3_DEFIPm_VRF_ID_MASK0f_SET(d, 0x3ff);
        /* Belt-and-suspenders: both a DROP next-hop AND the discard bit, so the
         * entry drops if the /32 is matched at all (which mechanism the datapath
         * honors is what we're testing). */
        L3_DEFIPm_ECMP0f_SET(d, 0);
        L3_DEFIPm_NEXT_HOP_INDEX0f_SET(d, ACL_DROP_NH_IDX);
        L3_DEFIPm_DST_DISCARD0f_SET(d, 1);
    }
    /* remove: CLR'd d -> VALID0=0 invalidates the slot */
    return WRITE_L3_DEFIPm(edged.unit, slot, d);
}

static void l3_deny_cidr(uint32_t ip, uint32_t mask, char *out, size_t n)
{
    int plen = 0; uint32_t m = mask;
    while (m & 0x80000000u) { plen++; m <<= 1; }
    snprintf(out, n, "%u.%u.%u.%u/%d",
             (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff, plen);
}

/* Add (add=1) / remove (add=0) a chip L3_ENTRY DST_DISCARD for a /32 dst.
 * Mirrors l3_host_add's key build (KEY_TYPE=0, V6=0, VRF=0) but sets
 * IPV4UC_DST_DISCARD instead of a next-hop, so an armed L3 lookup drops the
 * packet. Returns the schan index (>=0) on insert, 0 on delete, <0 on error. */
static int l3_v4_discard_entry(uint32_t ip, int add)
{
    L3_ENTRY_IPV4_UNICASTm_t e;
    L3_ENTRY_IPV4_UNICASTm_CLR(e);
    L3_ENTRY_IPV4_UNICASTm_KEY_TYPEf_SET(e, 0);
    L3_ENTRY_IPV4_UNICASTm_V6f_SET(e, 0);
    L3_ENTRY_IPV4_UNICASTm_IPV4UC_IP_ADDRf_SET(e, ip);
    L3_ENTRY_IPV4_UNICASTm_IPV4UC_VRF_IDf_SET(e, 0);
    L3_ENTRY_IPV4_UNICASTm_VALIDf_SET(e, 1);
    if (add) {
        L3_ENTRY_IPV4_UNICASTm_IPV4UC_DST_DISCARDf_SET(e, 1);
        return l3_v4_schan_insert(edged.unit, &e);
    }
    return l3_v4_schan_delete(edged.unit, &e);
}

/* Is this /32 dst currently ACL-denied with a chip discard entry? Lets the
 * neighbor->chip sync (l3_host_add) HONOR a deny instead of racing it: when a
 * denied IP is also a resolved neighbor, l3_host_add would otherwise re-insert
 * its forward entry and silently un-deny the IP. ip is host byte order. */
static int l3_v4_is_denied(uint32_t ip)
{
    int i;
    for (i = 0; i < L3_DENY_MAX; i++)
        if (l3_deny_tab[i].valid && l3_deny_tab[i].chip &&
            l3_deny_tab[i].mask == 0xffffffff && l3_deny_tab[i].ip == ip)
            return 1;
    return 0;
}

int l3_v4_deny_add(uint32_t ipv4_addr, uint32_t mask)
{
    int i;
    char cidr[24], cmd[64];
    if (mask == 0) {                           /* refuse "deny any" -> a blackhole default */
        syslog(LOG_WARNING, "ACL: refusing dst=any deny (would blackhole all routing)");
        return -1;
    }
    for (i = 0; i < L3_DENY_MAX; i++)          /* dedup */
        if (l3_deny_tab[i].valid && l3_deny_tab[i].ip == ipv4_addr &&
            l3_deny_tab[i].mask == mask)
            return 0;
    for (i = 0; i < L3_DENY_MAX; i++) if (!l3_deny_tab[i].valid) break;
    if (i == L3_DENY_MAX) { syslog(LOG_WARNING, "ACL: L3 deny table full (%d)", L3_DENY_MAX); return -1; }

    l3_deny_cidr(ipv4_addr, mask, cidr, sizeof(cidr));
    snprintf(cmd, sizeof(cmd), "ip route replace blackhole %s 2>/dev/null", cidr);
    if (system(cmd) != 0) {
        syslog(LOG_WARNING, "ACL: blackhole route add failed for %s", cidr);
        return -1;
    }
    l3_deny_tab[i].ip = ipv4_addr; l3_deny_tab[i].mask = mask; l3_deny_tab[i].valid = 1;
    l3_deny_tab[i].chip = 0; l3_deny_tab[i].defip = 0; l3_deny_tab[i].slot = -1;

    /* /32: also drop in the chip (the datapath HW-forwards these, bypassing the
     * kernel blackhole). Prefix (<32) denies stay kernel-only for now. */
    if (mask == 0xffffffff) {
        int sl = ACL_DEFIP_DISCARD_BASE + i;   /* consulted /32 band, per-entry slot */
        /* Primary in-chip drop: L3_DEFIP /32 DST_DISCARD (the table+band the
         * datapath actually consults). */
        if (l3_v4_defip_discard(ipv4_addr, mask, 1, sl) >= 0) {
            l3_deny_tab[i].defip = 1; l3_deny_tab[i].slot = sl;
            syslog(LOG_INFO, "ACL: dst-IP deny %s -> L3_DEFIP[%d] DST_DISCARD + kernel blackhole", cidr, sl);
        } else {
            syslog(LOG_WARNING, "ACL: L3_DEFIP DST_DISCARD failed for %s; kernel blackhole only", cidr);
        }
        /* Secondary: L3_ENTRY host discard (harmless; covers the host table). */
        if (l3_v4_discard_entry(ipv4_addr, 1) >= 0)
            l3_deny_tab[i].chip = 1;
    } else {
        syslog(LOG_INFO, "ACL: dst-IP deny %s -> kernel blackhole route (prefix, chip DST_DISCARD skipped)", cidr);
    }
    return 0;
}

int l3_v4_deny_del(uint32_t ipv4_addr, uint32_t mask)
{
    int i;
    char cidr[24], cmd[64];
    for (i = 0; i < L3_DENY_MAX; i++)
        if (l3_deny_tab[i].valid && l3_deny_tab[i].ip == ipv4_addr &&
            l3_deny_tab[i].mask == mask)
            break;
    if (i == L3_DENY_MAX) return 0;
    if (l3_deny_tab[i].defip)
        (void)l3_v4_defip_discard(ipv4_addr, mask, 0, l3_deny_tab[i].slot);
    if (l3_deny_tab[i].chip)
        (void)l3_v4_discard_entry(ipv4_addr, 0);   /* remove chip DST_DISCARD */
    l3_deny_cidr(ipv4_addr, mask, cidr, sizeof(cidr));
    snprintf(cmd, sizeof(cmd), "ip route del blackhole %s 2>/dev/null", cidr);
    (void)system(cmd);
    l3_deny_tab[i].valid = 0;
    l3_deny_tab[i].chip = 0;
    l3_deny_tab[i].defip = 0;
    return 0;
}

void l3_v4_deny_reset(void)
{
    int i;
    for (i = 0; i < L3_DENY_MAX; i++)
        if (l3_deny_tab[i].valid)
            (void)l3_v4_deny_del(l3_deny_tab[i].ip, l3_deny_tab[i].mask);
}

/* Full register diff vs working-Cumulus (SIGUSR1). Reveals any lookup-engine-arming
 * register edged has as a persistent VALUE gap. cmp_regs[] auto-gen'd from Cumulus. */
void l3_regdiff_diag(void)
{
    #include "generated/cmp_regs.h"
    FILE *lf = fopen("/tmp/edged-acl.log", "a");
    int k, gaps = 0, diffs = 0;
    if (!lf) return;
    for (k = 0; k < CMP_REGS_N; k++) {
        uint32_t ours = 0xdeadbeef;
        cdk_xgs_reg32_read(edged.unit, cmp_regs[k].addr, &ours);
        if (ours == cmp_regs[k].cval) continue;
        fprintf(lf, "REGDIFF %-34s cumulus=0x%08x ours=0x%08x %s\n",
                cmp_regs[k].name, cmp_regs[k].cval, ours, ours == 0 ? "GAP" : "DIFF");
        if (ours == 0) gaps++; else diffs++;
    }
    fprintf(lf, "REGDIFF summary: %d gaps, %d diffs of %d regs\n", gaps, diffs, CMP_REGS_N);
    fclose(lf);
}

/* HW-L3-forwarding diagnostic (SIGUSR1 -> /tmp/edged-acl.log). Pinpoints why transit
 * punts instead of HW-forwarding: the egress gate (EPC_LINK_BMAP / EGR_ENABLE) and
 * whether a known neighbor (Nexus 10.101.101.2) is a real HW-forward L3 entry. */
void l3_fwd_diag(void)
{
    FILE *lf = fopen("/tmp/edged-acl.log", "a");
    int i;
    if (!lf) return;

    /* ACL DST_DISCARD readback: for each chip-programmed deny, look up the
     * L3_ENTRY and report VALID/DST_DISCARD/HIT. The HW sets HIT when the
     * datapath L3 lookup MATCHES the entry, so HIT=1 after sending traffic to
     * the denied dst directly proves the chip is consulting (and thus dropping
     * on) the discard entry — the definitive functional check for the in-chip
     * ACL, independent of any punt/reply/counter observability. */
    for (i = 0; i < L3_DENY_MAX; i++) {
        L3_ENTRY_IPV4_UNICASTm_t key, got;
        uint32_t ip = l3_deny_tab[i].ip;
        int idx;
        if (!l3_deny_tab[i].valid || !l3_deny_tab[i].chip) continue;
        L3_ENTRY_IPV4_UNICASTm_CLR(key);
        L3_ENTRY_IPV4_UNICASTm_KEY_TYPEf_SET(key, 0);
        L3_ENTRY_IPV4_UNICASTm_V6f_SET(key, 0);
        L3_ENTRY_IPV4_UNICASTm_IPV4UC_IP_ADDRf_SET(key, ip);
        L3_ENTRY_IPV4_UNICASTm_IPV4UC_VRF_IDf_SET(key, 0);
        L3_ENTRY_IPV4_UNICASTm_VALIDf_SET(key, 1);
        idx = l3_v4_schan_lookup(edged.unit, &key, &got);
        if (idx < 0)
            fprintf(lf, "ACL-DENY %u.%u.%u.%u/32: chip lookup NOT FOUND (rv %d)\n",
                    (ip>>24)&0xff,(ip>>16)&0xff,(ip>>8)&0xff,ip&0xff, idx);
        else
            fprintf(lf, "ACL-DENY %u.%u.%u.%u/32: idx=%d VALID=%u DST_DISCARD=%u HIT=%u\n",
                    (ip>>24)&0xff,(ip>>16)&0xff,(ip>>8)&0xff,ip&0xff, idx,
                    L3_ENTRY_IPV4_UNICASTm_VALIDf_GET(got),
                    L3_ENTRY_IPV4_UNICASTm_IPV4UC_DST_DISCARDf_GET(got),
                    L3_ENTRY_IPV4_UNICASTm_HITf_GET(got));
    }

    {   /* EPC_LINK_BMAP — which ports the egress pipeline lets frames out of */
        EPC_LINK_BMAPm_t b; EPC_LINK_BMAPm_CLR(b);
        if (cdk_xgs_mem_read(edged.unit, EPC_LINK_BMAPm, 0, b.v, 3) >= 0)
            fprintf(lf, "L3FWD EPC_LINK_BMAP W0=0x%08x W1=0x%08x W2=0x%08x\n",
                    EPC_LINK_BMAPm_PORT_BITMAP_W0f_GET(b),
                    EPC_LINK_BMAPm_PORT_BITMAP_W1f_GET(b),
                    EPC_LINK_BMAPm_PORT_BITMAP_W2f_GET(b));
    }
    for (i = 0; i < EDGED_MAX_PORTS; i++) {   /* EGR_ENABLE for swp1/swp2/swp5 */
        int lp = edged.ports[i].logical_port, phys, pe = -1;
        EGR_ENABLEm_t e;
        if (!edged.ports[i].valid || (lp != 1 && lp != 2 && lp != 5)) continue;
        phys = edged.ports[i].physical_lane;
        EGR_ENABLEm_CLR(e);
        if (cdk_xgs_mem_read(edged.unit, EGR_ENABLEm, phys, e.v, 1) >= 0)
            pe = EGR_ENABLEm_PRT_ENABLEf_GET(e);
        fprintf(lf, "L3FWD EGR_ENABLE swp%d phys=%d PRT_ENABLE=%d\n", lp, phys, pe);
    }
    {   /* Nexus 10.101.101.2 (0x0a656502): HW-forward L3 entry, or missing/CPU? */
        L3_ENTRY_IPV4_UNICASTm_t key, got;
        L3_ENTRY_IPV4_UNICASTm_CLR(key);
        L3_ENTRY_IPV4_UNICASTm_KEY_TYPEf_SET(key, 0);
        L3_ENTRY_IPV4_UNICASTm_V6f_SET(key, 0);
        L3_ENTRY_IPV4_UNICASTm_IPV4UC_IP_ADDRf_SET(key, 0x0a656502);
        L3_ENTRY_IPV4_UNICASTm_IPV4UC_VRF_IDf_SET(key, 0);
        int lk = l3_v4_schan_lookup(edged.unit, &key, &got);
        if (lk >= 0) {
            int nh = L3_ENTRY_IPV4_UNICASTm_IPV4UC_NEXT_HOP_INDEXf_GET(got);
            int pn = -1, cpu = -1;
            ING_L3_NEXT_HOPm_t ing; ING_L3_NEXT_HOPm_CLR(ing);
            if (cdk_xgs_mem_read(edged.unit, ING_L3_NEXT_HOPm, nh, ing.v, 4) >= 0) {
                pn  = ING_L3_NEXT_HOPm_PORT_NUMf_GET(ing);
                cpu = ING_L3_NEXT_HOPm_COPY_TO_CPUf_GET(ing);
            }
            fprintf(lf, "L3FWD Nexus 10.101.101.2 L3_ENTRY FOUND idx=%d NHI=%d "
                    "-> ING_L3_NEXT_HOP PORT_NUM=%d COPY_TO_CPU=%d (want PORT_NUM=phys,CPU=0)\n",
                    lk, nh, pn, cpu);
        } else {
            fprintf(lf, "L3FWD Nexus 10.101.101.2 L3_ENTRY NOT FOUND (lk=%d) => L3 miss => punt\n", lk);
        }
    }
    {   /* HW-fwd DEFIP (neighbor band 4096+) vs local-host /32 band (2560+) that WORKS */
        int slots[5] = { 2606, 2560, 2561, 2562, 8000 }, s;
        for (s = 0; s < 5; s++) {
            L3_DEFIPm_t d; L3_DEFIPm_CLR(d);
            if (cdk_xgs_mem_read(edged.unit, L3_DEFIPm, slots[s], d.v, 15) >= 0)
                fprintf(lf, "L3FWD DEFIP[%d] V0=%d IP0=0x%08x MASK0=0x%08x MODE0=%d NHI0=%d\n",
                        slots[s], L3_DEFIPm_VALID0f_GET(d), L3_DEFIPm_IP_ADDR0f_GET(d),
                        L3_DEFIPm_IP_ADDR_MASK0f_GET(d), L3_DEFIPm_MODE0f_GET(d),
                        L3_DEFIPm_NEXT_HOP_INDEX0f_GET(d));
        }
    }
    for (i = 0; i < EDGED_MAX_PORTS; i++) {   /* swp1/swp5 ingress PORT_VID (MY_STATION key) */
        int lp = edged.ports[i].logical_port;
        PORT_TABm_t pt;
        if (!edged.ports[i].valid || (lp != 1 && lp != 5)) continue;
        PORT_TABm_CLR(pt);
        if (cdk_xgs_mem_read(edged.unit, PORT_TABm, lp, pt.v, 10) >= 0)
            fprintf(lf, "L3FWD swp%d PORT_VID=%d V4L3=%d\n", lp,
                    PORT_TABm_PORT_VIDf_GET(pt), PORT_TABm_V4L3_ENABLEf_GET(pt));
    }
    {   /* MY_STATION entries: which {MAC,VID} L3-terminate (routed) */
        int m;
        for (m = 0; m <= MY_STATION_TCAMm_MAX && m < 20; m++) {
            MY_STATION_TCAMm_t my; uint32_t mf[2] = {0, 0};
            MY_STATION_TCAMm_CLR(my);
            if (cdk_xgs_mem_read(edged.unit, MY_STATION_TCAMm, m, my.v, 6) < 0) continue;
            if (!MY_STATION_TCAMm_VALIDf_GET(my)) continue;
            MY_STATION_TCAMm_MAC_ADDRf_GET(my, mf);
            fprintf(lf, "L3FWD MY_STATION[%d] MAC=%04x%08x VID=%d\n",
                    m, mf[1] & 0xffff, mf[0], MY_STATION_TCAMm_VLAN_IDf_GET(my));
        }
    }
    fclose(lf);
    l3_regdiff_diag();   /* full register diff vs Cumulus */
}

static struct l3_rt *route_find(uint32_t target, uint32_t mask)
{
    for (int i = 0; i < L3_ROUTE_MAX; i++)
        if (route_tab[i].valid &&
            route_tab[i].target == target && route_tab[i].mask == mask)
            return &route_tab[i];
    return NULL;
}
static struct l3_rt *route_alloc(void)
{
    for (int i = 0; i < L3_ROUTE_MAX; i++)
        if (!route_tab[i].valid) return &route_tab[i];
    return NULL;
}

/* Order-independent signature over a (sorted) next-hop set. */
static uint64_t path_sig(const int *nh, int n)
{
    uint64_t s = (uint64_t)n * 1000003ull;
    for (int i = 0; i < n; i++)
        s ^= ((uint64_t)(nh[i] + 1) * 2654435761ull) + (s << 6) + (s >> 2);
    return s;
}

/* ECMP group dedup + refcount — defined after l3_ecmp_group_create(). */
static int  ecmp_group_ref(const int *nh, int n, uint64_t sig);
static void ecmp_group_unref(int group_id);

int l3_route_del(int family, const void *dst, int prefix_len)
{
    char dst_str[INET6_ADDRSTRLEN] = "?";
    inet_ntop(family, dst, dst_str, sizeof(dst_str));

    syslog(LOG_INFO, "L3 route del: %s/%d", dst_str, prefix_len);

    /* Only IPv4 transit prefixes are programmed into L3_DEFIP here. */
    if (family != AF_INET)
        return 0;

    uint32_t dst_host = ntohl(*(const uint32_t *)dst);
    uint32_t mask = (prefix_len == 0)  ? 0 :
                    (prefix_len >= 32) ? 0xffffffffu :
                    (0xffffffffu << (32 - prefix_len));
    uint32_t target = dst_host & mask;

    /* Look the prefix up in the route table, invalidate its DEFIP slot, and return
     * the slot + any ECMP group to their free-lists so they get reused. */
    struct l3_rt *r = route_find(target, mask);
    if (!r) {
        syslog(LOG_INFO, "L3 route del: %s/%d not in transit DEFIP", dst_str, prefix_len);
        return 0;
    }
    L3_DEFIPm_t d;
    L3_DEFIPm_CLR(d);
    int rv = WRITE_L3_DEFIPm(edged.unit, r->slot, d);
    defip_slot_free(r->slot);
    if (r->is_ecmp)
        ecmp_group_unref(r->ref);
    syslog(LOG_INFO, "route L3_DEFIP[%d] %s/%d removed (wr=%d), slot freed%s",
           r->slot, dst_str, prefix_len, rv,
           r->is_ecmp ? ", ECMP group dereffed" : "");
    r->valid = 0;
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
static int next_hop_idx = 1;   /* high-water; index 0 reserved as 'invalid' */

/* Released next-hop indices, reused before growing. */
static int nh_free[512];
static int nh_free_n;
static int next_hop_alloc(void)
{
    if (nh_free_n > 0) return nh_free[--nh_free_n];
    return next_hop_idx++;
}
static void next_hop_release(int idx)
{
    if (nh_free_n < 512) nh_free[nh_free_n++] = idx;
}

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

static void l3_neigh_nh_remove(uint32_t ip_host)
{
    for (int i = 0; i < L3_NEIGH_MAX; i++)
        if (l3_neigh_nh[i].valid && l3_neigh_nh[i].ip == ip_host) {
            l3_neigh_nh[i].valid = 0;
            return;
        }
}

/* IPv6 gateway/host -> chip next-hop map (mirrors the v4 one, 16-byte key). */
static struct { uint8_t ip[16]; int nh_idx; int valid; } l3_neigh_nh6[L3_NEIGH_MAX];

static int l3_neigh6_lookup(const uint8_t *ip16)
{
    for (int i = 0; i < L3_NEIGH_MAX; i++)
        if (l3_neigh_nh6[i].valid && memcmp(l3_neigh_nh6[i].ip, ip16, 16) == 0)
            return l3_neigh_nh6[i].nh_idx;
    return -1;
}
static void l3_neigh6_record(const uint8_t *ip16, int nh_idx)
{
    int free_slot = -1;
    for (int i = 0; i < L3_NEIGH_MAX; i++) {
        if (l3_neigh_nh6[i].valid && memcmp(l3_neigh_nh6[i].ip, ip16, 16) == 0) {
            l3_neigh_nh6[i].nh_idx = nh_idx; return;
        }
        if (free_slot < 0 && !l3_neigh_nh6[i].valid) free_slot = i;
    }
    if (free_slot >= 0) {
        memcpy(l3_neigh_nh6[free_slot].ip, ip16, 16);
        l3_neigh_nh6[free_slot].nh_idx = nh_idx;
        l3_neigh_nh6[free_slot].valid = 1;
    }
}
static void l3_neigh6_remove(const uint8_t *ip16)
{
    for (int i = 0; i < L3_NEIGH_MAX; i++)
        if (l3_neigh_nh6[i].valid && memcmp(l3_neigh_nh6[i].ip, ip16, 16) == 0) {
            l3_neigh_nh6[i].valid = 0; return;
        }
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
    if (logical_port == 1) {   /* catch-all L3_DEFIP: match-any routed IPv4 -> CPU (kernel software-forwards) */
        L3_DEFIPm_t d;
        L3_DEFIPm_CLR(d);
        L3_DEFIPm_VALID0f_SET(d, 1);
        L3_DEFIPm_MODE0f_SET(d, 0);
        L3_DEFIPm_MODE_MASK0f_SET(d, 0);
        L3_DEFIPm_IP_ADDR0f_SET(d, 0);
        L3_DEFIPm_IP_ADDR_MASK0f_SET(d, 0);
        L3_DEFIPm_VRF_ID_0f_SET(d, 0);
        L3_DEFIPm_VRF_ID_MASK0f_SET(d, 0);
        L3_DEFIPm_NEXT_HOP_INDEX0f_SET(d, nh_idx);
        rv = WRITE_L3_DEFIPm(edged.unit, 8000, d);
        syslog(LOG_INFO, "L3_DEFIP[8000] catch-all -> nh_idx=%d (CPU) wr=%d", nh_idx, rv);
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

    int is_v6 = (family == AF_INET6);   /* v4 and v6 share the next-hop setup below;
                                           only the nh-map + L3_ENTRY insert differ. */

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

    /* 2) Reuse this gateway's existing chip next-hop if already resolved (an ARP
     *    refresh re-fires l3_host_add), else allocate one. Reusing keeps the
     *    gateway's nh_idx stable and stops re-resolution leaking next-hop indices. */
    {
        int existing;
        if (is_v6) {
            existing = l3_neigh6_lookup((const uint8_t *)addr);
        } else {
            const uint8_t *ipb2 = (const uint8_t *)addr;
            uint32_t ip_h = ((uint32_t)ipb2[0] << 24) | ((uint32_t)ipb2[1] << 16)
                          | ((uint32_t)ipb2[2] <<  8) |  (uint32_t)ipb2[3];
            existing = l3_neigh_nh_lookup(ip_h);
        }
        nh_idx = (existing >= 0) ? existing : next_hop_alloc();
    }

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

    /* 3) L3 host entry: map host IP -> our nh_idx (v4 or v6 hash table). */
    if (is_v6) {
        const uint8_t *a = (const uint8_t *)addr;       /* 16 bytes, network order */
        L3_ENTRY_IPV6_UNICASTm_t h6;
        /* Each 64-bit half as {low-word, high-word}; byte a[0] is most significant. */
        uint32_t upr[2], lwr[2];
        upr[1] = ((uint32_t)a[0]<<24)|((uint32_t)a[1]<<16)|((uint32_t)a[2]<<8)|a[3];
        upr[0] = ((uint32_t)a[4]<<24)|((uint32_t)a[5]<<16)|((uint32_t)a[6]<<8)|a[7];
        lwr[1] = ((uint32_t)a[8]<<24)|((uint32_t)a[9]<<16)|((uint32_t)a[10]<<8)|a[11];
        lwr[0] = ((uint32_t)a[12]<<24)|((uint32_t)a[13]<<16)|((uint32_t)a[14]<<8)|a[15];

        L3_ENTRY_IPV6_UNICASTm_CLR(h6);
        L3_ENTRY_IPV6_UNICASTm_KEY_TYPE_0f_SET(h6, 2);   /* IPv6 unicast host */
        L3_ENTRY_IPV6_UNICASTm_KEY_TYPE_1f_SET(h6, 2);
        L3_ENTRY_IPV6_UNICASTm_V6_0f_SET(h6, 1);
        L3_ENTRY_IPV6_UNICASTm_V6_1f_SET(h6, 1);
        L3_ENTRY_IPV6_UNICASTm_IPV6UC_IP_ADDR_UPR_64f_SET(h6, upr);
        L3_ENTRY_IPV6_UNICASTm_IPV6UC_IP_ADDR_LWR_64f_SET(h6, lwr);
        L3_ENTRY_IPV6_UNICASTm_IPV6UC_NEXT_HOP_INDEXf_SET(h6, nh_idx);
        L3_ENTRY_IPV6_UNICASTm_VALID_0f_SET(h6, 1);
        L3_ENTRY_IPV6_UNICASTm_VALID_1f_SET(h6, 1);

        int idx = l3_v6_schan_insert(edged.unit, &h6);
        if (idx < 0) {
            syslog(LOG_WARNING, "L3_ENTRY_IPV6 schan_insert failed: %d", idx);
            return -1;
        }
        /* Read it back to confirm the key encoding hashes/looks up. */
        L3_ENTRY_IPV6_UNICASTm_t key;
        L3_ENTRY_IPV6_UNICASTm_CLR(key);
        L3_ENTRY_IPV6_UNICASTm_KEY_TYPE_0f_SET(key, 2);
        L3_ENTRY_IPV6_UNICASTm_KEY_TYPE_1f_SET(key, 2);
        L3_ENTRY_IPV6_UNICASTm_V6_0f_SET(key, 1);
        L3_ENTRY_IPV6_UNICASTm_V6_1f_SET(key, 1);
        L3_ENTRY_IPV6_UNICASTm_IPV6UC_IP_ADDR_UPR_64f_SET(key, upr);
        L3_ENTRY_IPV6_UNICASTm_IPV6UC_IP_ADDR_LWR_64f_SET(key, lwr);
        int lk = l3_v6_schan_lookup(edged.unit, &key, NULL);
        syslog(LOG_INFO, "L3_ENTRY_IPV6: %s -> nh_idx=%d insert=%d lookup=%s",
               addr_str, nh_idx, idx, lk >= 0 ? "FOUND" : "NOT-FOUND(encoding?)");
        l3_neigh6_record(a, nh_idx);
    } else {
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

        /* ACL deny wins over forwarding: if this neighbor's IP is dst-IP denied,
         * program its host entry as DST_DISCARD so the neighbor sync can't race
         * the ACL and un-deny it (fixes the deny-clobber observed 2026-07-16). */
        if (l3_v4_is_denied(ip_be)) {
            L3_ENTRY_IPV4_UNICASTm_IPV4UC_DST_DISCARDf_SET(hst, 1);
            syslog(LOG_INFO,
                   "L3 host %u.%u.%u.%u ACL-denied -> DST_DISCARD (not forward)",
                   (ip_be >> 24) & 0xff, (ip_be >> 16) & 0xff,
                   (ip_be >> 8) & 0xff, ip_be & 0xff);
        }

        int idx = l3_v4_schan_insert(edged.unit, &hst);
        if (idx < 0) {
            syslog(LOG_WARNING,
                   "L3_ENTRY_IPV4_UNICAST schan_insert failed: %d", idx);
            return -1;
        }
        syslog(LOG_DEBUG, "L3 host schan_insert placed at idx=%d", idx);
        l3_neigh_nh_record(ip_be, nh_idx);
        /* NOTE: a HW-forward attempt (mirror this neighbor into a /32 L3_DEFIP with
         * the real egress next-hop) was tried and did NOT flip transit to HW — the
         * datapath's L3 DEFIP lookup does not match programmed entries (V4L3DSTMISS
         * fires regardless), the same un-armed-lookup-engine wall as the FP. So
         * transit stays software-forwarded (kernel FIB) and ACLs use blackhole
         * routes; HW L3 forwarding needs the soc_init lookup-engine arming OpenMDK
         * skips. See project_acl_l3_dst_discard_pivot / project_init_all_insight. */
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

    if (family == AF_INET6) {
        const uint8_t *a = (const uint8_t *)addr;
        int nh = l3_neigh6_lookup(a);
        if (nh < 0) {
            syslog(LOG_DEBUG, "L3 host del: %s not tracked", addr_str);
            return 0;
        }
        uint32_t upr[2], lwr[2];
        upr[1] = ((uint32_t)a[0]<<24)|((uint32_t)a[1]<<16)|((uint32_t)a[2]<<8)|a[3];
        upr[0] = ((uint32_t)a[4]<<24)|((uint32_t)a[5]<<16)|((uint32_t)a[6]<<8)|a[7];
        lwr[1] = ((uint32_t)a[8]<<24)|((uint32_t)a[9]<<16)|((uint32_t)a[10]<<8)|a[11];
        lwr[0] = ((uint32_t)a[12]<<24)|((uint32_t)a[13]<<16)|((uint32_t)a[14]<<8)|a[15];
        L3_ENTRY_IPV6_UNICASTm_t k;
        L3_ENTRY_IPV6_UNICASTm_CLR(k);
        L3_ENTRY_IPV6_UNICASTm_KEY_TYPE_0f_SET(k, 2);
        L3_ENTRY_IPV6_UNICASTm_KEY_TYPE_1f_SET(k, 2);
        L3_ENTRY_IPV6_UNICASTm_V6_0f_SET(k, 1);
        L3_ENTRY_IPV6_UNICASTm_V6_1f_SET(k, 1);
        L3_ENTRY_IPV6_UNICASTm_IPV6UC_IP_ADDR_UPR_64f_SET(k, upr);
        L3_ENTRY_IPV6_UNICASTm_IPV6UC_IP_ADDR_LWR_64f_SET(k, lwr);
        (void)l3_v6_schan_delete(edged.unit, &k);

        ING_L3_NEXT_HOPm_t ing; ING_L3_NEXT_HOPm_CLR(ing);
        (void)WRITE_ING_L3_NEXT_HOPm(edged.unit, nh, ing);
        EGR_L3_NEXT_HOPm_t egr; EGR_L3_NEXT_HOPm_CLR(egr);
        (void)WRITE_EGR_L3_NEXT_HOPm(edged.unit, nh, egr);
        next_hop_release(nh);
        l3_neigh6_remove(a);
        syslog(LOG_INFO, "L3 host del (v6): %s nh_idx %d freed", addr_str, nh);
        return 0;
    }

    const uint8_t *ipb = (const uint8_t *)addr;
    uint32_t ip_host = ((uint32_t)ipb[0] << 24) | ((uint32_t)ipb[1] << 16)
                     | ((uint32_t)ipb[2] <<  8) |  (uint32_t)ipb[3];
    int nh = l3_neigh_nh_lookup(ip_host);
    if (nh < 0) {
        syslog(LOG_DEBUG, "L3 host del: %s not tracked", addr_str);
        return 0;
    }

    /* 1) Remove the host /32 from the hashed L3_ENTRY table. */
    {
        L3_ENTRY_IPV4_UNICASTm_t hst;
        L3_ENTRY_IPV4_UNICASTm_CLR(hst);
        L3_ENTRY_IPV4_UNICASTm_KEY_TYPEf_SET(hst, 0);
        L3_ENTRY_IPV4_UNICASTm_V6f_SET(hst, 0);
        L3_ENTRY_IPV4_UNICASTm_IPV4UC_IP_ADDRf_SET(hst, ip_host);
        L3_ENTRY_IPV4_UNICASTm_IPV4UC_VRF_IDf_SET(hst, 0);
        (void)l3_v4_schan_delete(edged.unit, &hst);
    }

    /* 2) Invalidate the next-hop's chip halves and return its index to the
     *    free-list. A transit route via this gateway is withdrawn by the kernel
     *    when the neighbor goes away (so it's already out of L3_DEFIP), and the
     *    15s re-dump reprograms anything still in flight — so reusing the freed
     *    index is safe in practice. */
    {
        ING_L3_NEXT_HOPm_t ing; ING_L3_NEXT_HOPm_CLR(ing);
        (void)WRITE_ING_L3_NEXT_HOPm(edged.unit, nh, ing);
        EGR_L3_NEXT_HOPm_t egr; EGR_L3_NEXT_HOPm_CLR(egr);
        (void)WRITE_EGR_L3_NEXT_HOPm(edged.unit, nh, egr);
    }
    next_hop_release(nh);
    l3_neigh_nh_remove(ip_host);

    syslog(LOG_INFO, "L3 host del: %s nh_idx %d freed", addr_str, nh);
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
static int l3_ecmp_next_slot = 0;     /* high-water of L3_ECMP member slots */
static int l3_ecmp_next_group_id = 1; /* high-water of group IDs (0 reserved) */

/* Live ECMP groups — dedup + refcount so identical member-sets share one group
 * (so the live group count is O(unique-member-sets), not O(routes)). */
struct ecmp_grp { uint64_t sig; int group_id; int base; int count; int ref; int valid; };
static struct ecmp_grp ecmp_tab[L3_ECMP_GRP_MAX];

/* Free-lists: released member-slot blocks (reused by exact count) + group IDs. */
static struct { int base, count; } ecmp_blk_free[L3_ECMP_GRP_MAX];
static int ecmp_blk_free_n;
static int ecmp_gid_free[L3_ECMP_GRP_MAX];
static int ecmp_gid_free_n;

static int ecmp_slots_alloc(int count)
{
    for (int i = 0; i < ecmp_blk_free_n; i++)
        if (ecmp_blk_free[i].count == count) {           /* exact-fit reuse */
            int b = ecmp_blk_free[i].base;
            ecmp_blk_free[i] = ecmp_blk_free[--ecmp_blk_free_n];
            return b;
        }
    if (l3_ecmp_next_slot + count > L3_ECMPm_MAX + 1) return -1;
    int b = l3_ecmp_next_slot; l3_ecmp_next_slot += count; return b;
}
static void ecmp_slots_release(int base, int count)
{
    if (ecmp_blk_free_n < L3_ECMP_GRP_MAX) {
        ecmp_blk_free[ecmp_blk_free_n].base = base;
        ecmp_blk_free[ecmp_blk_free_n].count = count;
        ecmp_blk_free_n++;
    }
}
static int ecmp_gid_alloc(void)
{
    if (ecmp_gid_free_n > 0) return ecmp_gid_free[--ecmp_gid_free_n];
    return l3_ecmp_next_group_id++;
}
static void ecmp_gid_release(int gid)
{
    if (ecmp_gid_free_n < L3_ECMP_GRP_MAX) ecmp_gid_free[ecmp_gid_free_n++] = gid;
}

/*
 * Write a NEW L3_ECMP group: `count` consecutive L3_ECMP member slots + one
 * L3_ECMP_COUNT entry, and record it in ecmp_tab. Slots/group-id come from the
 * free-lists. Returns the group_id (the L3_DEFIP ECMP_PTR0 value), -1 on error.
 * Encoding verified on live Cumulus 56846: BASE_PTR_0[21:10], COUNT_0[9:0].
 */
static int l3_ecmp_group_create(const int *intf_ids, int count, uint64_t sig)
{
    int i, rv;

    if (count <= 0 || count > 64) {
        syslog(LOG_ERR, "ECMP create: invalid count=%d", count);
        return -1;
    }
    int base = ecmp_slots_alloc(count);
    if (base < 0) { syslog(LOG_ERR, "ECMP member table full (count=%d)", count); return -1; }
    int group_id = ecmp_gid_alloc();

    for (i = 0; i < count; i++) {
        L3_ECMPm_t entry;
        L3_ECMPm_CLR(entry);
        L3_ECMPm_NEXT_HOP_INDEXf_SET(entry, intf_ids[i]);
        rv = WRITE_L3_ECMPm(edged.unit, base + i, entry);
        if (rv < 0) {
            syslog(LOG_WARNING, "ECMP slot %d write failed: %d", base + i, rv);
            ecmp_slots_release(base, count); ecmp_gid_release(group_id);
            return -1;
        }
    }
    {
        L3_ECMP_COUNTm_t cnt;
        L3_ECMP_COUNTm_CLR(cnt);
        L3_ECMP_COUNTm_BASE_PTR_0f_SET(cnt, base);
        L3_ECMP_COUNTm_COUNT_0f_SET(cnt, count);
        rv = WRITE_L3_ECMP_COUNTm(edged.unit, group_id, cnt);
        if (rv < 0) {
            syslog(LOG_WARNING, "L3_ECMP_COUNT[%d] write failed: %d", group_id, rv);
            ecmp_slots_release(base, count); ecmp_gid_release(group_id);
            return -1;
        }
    }
    for (i = 0; i < L3_ECMP_GRP_MAX; i++)
        if (!ecmp_tab[i].valid) {
            ecmp_tab[i].sig = sig; ecmp_tab[i].group_id = group_id;
            ecmp_tab[i].base = base; ecmp_tab[i].count = count;
            ecmp_tab[i].ref = 1; ecmp_tab[i].valid = 1;
            break;
        }
    syslog(LOG_INFO, "ECMP group %d: base=%d count=%d (new)", group_id, base, count);
    return group_id;
}

/* Reference an ECMP group for this member-set: share an identical existing group
 * (refcount++), else create a new one. */
static int ecmp_group_ref(const int *nh, int n, uint64_t sig)
{
    for (int i = 0; i < L3_ECMP_GRP_MAX; i++)
        if (ecmp_tab[i].valid && ecmp_tab[i].sig == sig && ecmp_tab[i].count == n) {
            ecmp_tab[i].ref++;
            return ecmp_tab[i].group_id;
        }
    return l3_ecmp_group_create(nh, n, sig);
}

/* Drop a reference; on the last one, free the chip group + slots + id. */
static void ecmp_group_unref(int group_id)
{
    for (int i = 0; i < L3_ECMP_GRP_MAX; i++)
        if (ecmp_tab[i].valid && ecmp_tab[i].group_id == group_id) {
            if (--ecmp_tab[i].ref > 0) return;
            L3_ECMP_COUNTm_t cnt;
            L3_ECMP_COUNTm_CLR(cnt);
            (void)WRITE_L3_ECMP_COUNTm(edged.unit, group_id, cnt);
            ecmp_slots_release(ecmp_tab[i].base, ecmp_tab[i].count);
            ecmp_gid_release(group_id);
            ecmp_tab[i].valid = 0;
            syslog(LOG_INFO, "ECMP group %d freed (refcount 0)", group_id);
            return;
        }
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
    int nh_idx[64];
    int n = 0, rv;

    if (ngw < 1) return -1;
    if (ngw > 64) ngw = 64;

    uint32_t mask = (prefix_len == 0)  ? 0 :
                    (prefix_len >= 32) ? 0xffffffffu :
                    (0xffffffffu << (32 - prefix_len));
    uint32_t target = dst_host & mask;

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

    /* Canonicalize the next-hop set (sort) and signature it, so a re-dump of the
     * same paths in any order is a no-op, and any real change (shape OR members) is
     * detected. The bookkeeping layer then reuses the slot, refcounts the ECMP
     * group, and frees what's no longer referenced — no leak under churn. */
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (nh_idx[b] < nh_idx[a]) { int t = nh_idx[a]; nh_idx[a] = nh_idx[b]; nh_idx[b] = t; }
    uint64_t sig = path_sig(nh_idx, n);

    struct l3_rt *r = route_find(target, mask);
    if (r && r->sig == sig)
        return 0;                           /* already programmed, unchanged */

    int is_ecmp = (n > 1);
    int new_ref = is_ecmp ? ecmp_group_ref(nh_idx, n, sig) : nh_idx[0];
    if (is_ecmp && new_ref < 0) return -1;

    int slot = r ? r->slot : defip_slot_alloc();

    L3_DEFIPm_t d;
    L3_DEFIPm_CLR(d);
    L3_DEFIPm_VALID0f_SET(d, 1);
    L3_DEFIPm_MODE0f_SET(d, 0);
    L3_DEFIPm_MODE_MASK0f_SET(d, 1);
    L3_DEFIPm_IP_ADDR0f_SET(d, target);
    L3_DEFIPm_IP_ADDR_MASK0f_SET(d, mask);
    L3_DEFIPm_VRF_ID_0f_SET(d, 0);
    L3_DEFIPm_VRF_ID_MASK0f_SET(d, 0x3ff);
    if (is_ecmp) {
        L3_DEFIPm_ECMP0f_SET(d, 1);
        L3_DEFIPm_ECMP_PTR0f_SET(d, new_ref);
    } else {
        L3_DEFIPm_ECMP0f_SET(d, 0);
        L3_DEFIPm_NEXT_HOP_INDEX0f_SET(d, new_ref);
    }
    rv = WRITE_L3_DEFIPm(edged.unit, slot, d);

    if (r && r->is_ecmp)                     /* drop the old group reference */
        ecmp_group_unref(r->ref);
    if (!r) {
        r = route_alloc();
        if (!r) { syslog(LOG_ERR, "L3 route table full — %u.%u.%u.%u/%d not tracked",
                         (target>>24)&0xff,(target>>16)&0xff,(target>>8)&0xff,target&0xff,
                         prefix_len); return -1; }
        r->target = target; r->mask = mask; r->slot = slot; r->valid = 1;
    }
    r->is_ecmp = is_ecmp; r->ref = new_ref; r->sig = sig;

    if (is_ecmp)
        syslog(LOG_INFO, "route L3_DEFIP[%d] %u.%u.%u.%u/%d -> ECMP grp=%d (%d paths) wr=%d",
               slot,(dst_host>>24)&0xff,(dst_host>>16)&0xff,(dst_host>>8)&0xff,dst_host&0xff,
               prefix_len, new_ref, n, rv);
    else
        syslog(LOG_INFO, "route L3_DEFIP[%d] %u.%u.%u.%u/%d -> nh_idx=%d wr=%d",
               slot,(dst_host>>24)&0xff,(dst_host>>16)&0xff,(dst_host>>8)&0xff,dst_host&0xff,
               prefix_len, new_ref, rv);
    return rv < 0 ? -1 : 0;
}

/* ===========================================================================
 * IPv6 transit routes -> L3_DEFIP_128 (a dedicated 256-entry table, separate
 * from the v4 L3_DEFIP TCAM). Own slot free-list; ECMP groups + path signatures
 * are shared with v4 (groups just hold next-hop indices).
 * ========================================================================= */
#define L3_ROUTE6_MAX 256

struct l3_rt6 {
    uint8_t  target[16];     /* prefix network (already masked by the kernel) */
    int      prefix_len;
    int      slot;           /* L3_DEFIP_128 index (0..255) */
    int      is_ecmp;
    int      ref;            /* ECMP group_id or single next-hop index */
    uint64_t sig;
    int      valid;
};
static struct l3_rt6 route6_tab[L3_ROUTE6_MAX];

static int defip128_next_slot;          /* high-water of L3_DEFIP_128 */
static int defip128_free[L3_ROUTE6_MAX];
static int defip128_free_n;

static int defip128_alloc(void)
{
    if (defip128_free_n > 0) return defip128_free[--defip128_free_n];
    if (defip128_next_slot > L3_DEFIP_128m_MAX) return -1;
    return defip128_next_slot++;
}
static void defip128_release(int s)
{
    if (defip128_free_n < L3_ROUTE6_MAX) defip128_free[defip128_free_n++] = s;
}
static struct l3_rt6 *route6_find(const uint8_t *t, int plen)
{
    for (int i = 0; i < L3_ROUTE6_MAX; i++)
        if (route6_tab[i].valid && route6_tab[i].prefix_len == plen &&
            memcmp(route6_tab[i].target, t, 16) == 0)
            return &route6_tab[i];
    return NULL;
}
static struct l3_rt6 *route6_alloc(void)
{
    for (int i = 0; i < L3_ROUTE6_MAX; i++)
        if (!route6_tab[i].valid) return &route6_tab[i];
    return NULL;
}

/* 128-bit address -> 4 words (word[3] = most significant 32 bits). */
static void v6_addr_words(const uint8_t *a, uint32_t w[4])
{
    w[3] = ((uint32_t)a[0]<<24)|((uint32_t)a[1]<<16)|((uint32_t)a[2]<<8)|a[3];
    w[2] = ((uint32_t)a[4]<<24)|((uint32_t)a[5]<<16)|((uint32_t)a[6]<<8)|a[7];
    w[1] = ((uint32_t)a[8]<<24)|((uint32_t)a[9]<<16)|((uint32_t)a[10]<<8)|a[11];
    w[0] = ((uint32_t)a[12]<<24)|((uint32_t)a[13]<<16)|((uint32_t)a[14]<<8)|a[15];
}
/* /plen prefix -> 4-word mask (top `plen` bits set; word[3] holds bits 127..96). */
static void v6_mask_words(int plen, uint32_t w[4])
{
    int bits = plen, word;
    for (word = 0; word < 4; word++) w[word] = 0;
    for (word = 3; word >= 0 && bits > 0; word--) {
        int n = bits >= 32 ? 32 : bits;
        w[word] = (n == 32) ? 0xffffffffu : (0xffffffffu << (32 - n));
        bits -= n;
    }
}

int l3_route_add_paths_v6(const uint8_t *dst16, int prefix_len,
                          const uint8_t gw16[][16], int ngw)
{
    int nh_idx[64], n = 0, rv;
    char ds[INET6_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET6, dst16, ds, sizeof(ds));

    if (ngw < 1) return -1;
    if (ngw > 64) ngw = 64;

    for (int i = 0; i < ngw; i++) {
        int nh = l3_neigh6_lookup(gw16[i]);
        if (nh < 0) continue;               /* gateway not resolved yet (re-dump heals) */
        nh_idx[n++] = nh;
    }
    if (n == 0) {
        syslog(LOG_WARNING, "v6 route %s/%d: no resolved next-hops, not programmed",
               ds, prefix_len);
        return -1;
    }

    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (nh_idx[b] < nh_idx[a]) { int t = nh_idx[a]; nh_idx[a] = nh_idx[b]; nh_idx[b] = t; }
    uint64_t sig = path_sig(nh_idx, n);

    struct l3_rt6 *r = route6_find(dst16, prefix_len);
    if (r && r->sig == sig)
        return 0;                           /* unchanged */

    int is_ecmp = (n > 1);
    int new_ref = is_ecmp ? ecmp_group_ref(nh_idx, n, sig) : nh_idx[0];
    if (is_ecmp && new_ref < 0) return -1;

    int slot = r ? r->slot : defip128_alloc();
    if (slot < 0) {
        syslog(LOG_ERR, "v6 route %s/%d: L3_DEFIP_128 full", ds, prefix_len);
        if (is_ecmp) ecmp_group_unref(new_ref);
        return -1;
    }

    uint32_t aw[4], mw[4];
    v6_addr_words(dst16, aw);
    v6_mask_words(prefix_len, mw);

    L3_DEFIP_128m_t d;
    L3_DEFIP_128m_CLR(d);
    L3_DEFIP_128m_VALID_0f_SET(d, 1);
    L3_DEFIP_128m_VALID_1f_SET(d, 1);
    L3_DEFIP_128m_IP_ADDRf_SET(d, aw);
    L3_DEFIP_128m_IP_ADDR_MASKf_SET(d, mw);
    L3_DEFIP_128m_VRF_IDf_SET(d, 0);
    L3_DEFIP_128m_VRF_ID_MASKf_SET(d, 0x3ff);
    if (is_ecmp) {
        L3_DEFIP_128m_ECMPf_SET(d, 1);
        L3_DEFIP_128m_ECMP_PTRf_SET(d, new_ref);
    } else {
        L3_DEFIP_128m_ECMPf_SET(d, 0);
        L3_DEFIP_128m_NEXT_HOP_INDEXf_SET(d, new_ref);
    }
    rv = WRITE_L3_DEFIP_128m(edged.unit, slot, d);

    if (r && r->is_ecmp)
        ecmp_group_unref(r->ref);
    if (!r) {
        r = route6_alloc();
        if (!r) { syslog(LOG_ERR, "v6 route table full — %s/%d", ds, prefix_len); return -1; }
        memcpy(r->target, dst16, 16);
        r->prefix_len = prefix_len; r->slot = slot; r->valid = 1;
    }
    r->is_ecmp = is_ecmp; r->ref = new_ref; r->sig = sig;

    if (is_ecmp)
        syslog(LOG_INFO, "route L3_DEFIP_128[%d] %s/%d -> ECMP grp=%d (%d paths) wr=%d",
               slot, ds, prefix_len, new_ref, n, rv);
    else
        syslog(LOG_INFO, "route L3_DEFIP_128[%d] %s/%d -> nh_idx=%d wr=%d",
               slot, ds, prefix_len, new_ref, rv);
    return rv < 0 ? -1 : 0;
}

int l3_route_del_v6(const uint8_t *dst16, int prefix_len)
{
    char ds[INET6_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET6, dst16, ds, sizeof(ds));

    struct l3_rt6 *r = route6_find(dst16, prefix_len);
    if (!r) {
        syslog(LOG_DEBUG, "v6 route del: %s/%d not in L3_DEFIP_128", ds, prefix_len);
        return 0;
    }
    L3_DEFIP_128m_t d;
    L3_DEFIP_128m_CLR(d);
    int rv = WRITE_L3_DEFIP_128m(edged.unit, r->slot, d);
    defip128_release(r->slot);
    if (r->is_ecmp)
        ecmp_group_unref(r->ref);
    syslog(LOG_INFO, "route L3_DEFIP_128[%d] %s/%d removed (wr=%d), slot freed%s",
           r->slot, ds, prefix_len, rv, r->is_ecmp ? ", ECMP group dereffed" : "");
    r->valid = 0;
    return 0;
}
