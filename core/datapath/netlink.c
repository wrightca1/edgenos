/*
 * netlink.c - Netlink listener for route, neighbor, and link events
 *
 * Subscribes to RTNLGRP_LINK, RTNLGRP_IPV4_ROUTE, RTNLGRP_IPV6_ROUTE,
 * RTNLGRP_NEIGH and translates kernel events to ASIC programming.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/neighbour.h>
#include <net/if.h>

/* NDA_RTA / NDA_PAYLOAD may not be defined in older headers */
#ifndef NDA_RTA
#define NDA_RTA(r) \
    ((struct rtattr *)(((char *)(r)) + NLMSG_ALIGN(sizeof(struct ndmsg))))
#endif
#ifndef NDA_PAYLOAD
#define NDA_PAYLOAD(n) \
    NLMSG_PAYLOAD((n), sizeof(struct ndmsg))
#endif

#include "edged.h"
#include "netlink.h"
#include "portmap.h"
#include "l2.h"
#include "l3.h"

/* BMD headers for port mode control */
#include <bmd/bmd.h>

#define NETLINK_BUF_SIZE  16384

int netlink_init(void)
{
    struct sockaddr_nl sa;
    int fd;

    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK, NETLINK_ROUTE);
    if (fd < 0) {
        syslog(LOG_ERR, "Cannot create netlink socket: %s", strerror(errno));
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = RTMGRP_LINK |
                   RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE |
                   RTMGRP_NEIGH |
                   RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        syslog(LOG_ERR, "Cannot bind netlink socket: %s", strerror(errno));
        close(fd);
        return -1;
    }

    edged.netlink_fd = fd;
    syslog(LOG_INFO, "Netlink listener initialized");

    /*
     * Dump existing IPv4 addresses (RTM_GETADDR + NLM_F_DUMP).  The kernel
     * only sends RTM_NEWADDR on a *change*, so an swpN IP that already
     * exists when edged starts (e.g. configured before edged, or surviving
     * an edged restart) is never seen -> its L3 local-host CPU-punt is never
     * programmed -> IP traffic to the switch's own IP (ping replies) is not
     * punted to the CPU.  The kernel answers this dump with RTM_NEWADDR
     * messages that netlink_poll() handles exactly like live events.
     */
    {
        struct {
            struct nlmsghdr nlh;
            struct ifaddrmsg ifa;
        } req;
        memset(&req, 0, sizeof(req));
        req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
        req.nlh.nlmsg_type = RTM_GETADDR;
        req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
        req.nlh.nlmsg_seq = 1;
        req.ifa.ifa_family = AF_INET;
        if (send(fd, &req, req.nlh.nlmsg_len, 0) < 0) {
            syslog(LOG_WARNING, "Netlink: RTM_GETADDR dump request failed: %s",
                   strerror(errno));
        } else {
            syslog(LOG_INFO, "Netlink: requested IPv4 address dump");
        }
    }

    /*
     * Also dump existing NEIGHBORS then ROUTES.  Same reasoning as the address
     * dump: a neighbor (e.g. the OSPF gateway) that resolved, or a route that
     * was installed, *before* edged finished its ~12s ASIC init is never seen as
     * a live event — so its chip next-hop / L3_DEFIP entry is never programmed,
     * and every transit/ECMP route fails next-hop resolution.  The kernel
     * serializes dumps per socket, so neighbors are fully processed (chip
     * next-hops populated) before the route dump arrives and resolves against
     * them.  Order: NEIGH (seq 2) then ROUTE (seq 3).
     */
    {
        struct { struct nlmsghdr nlh; struct rtgenmsg gen; } req;
        int kinds[2] = { RTM_GETNEIGH, RTM_GETROUTE };
        for (int k = 0; k < 2; k++) {
            memset(&req, 0, sizeof(req));
            req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg));
            req.nlh.nlmsg_type = kinds[k];
            req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
            req.nlh.nlmsg_seq = 2 + k;
            req.gen.rtgen_family = AF_INET;
            if (send(fd, &req, req.nlh.nlmsg_len, 0) < 0) {
                syslog(LOG_WARNING, "Netlink: dump type=%d request failed: %s",
                       kinds[k], strerror(errno));
            } else {
                syslog(LOG_INFO, "Netlink: requested %s dump",
                       kinds[k] == RTM_GETNEIGH ? "neighbor" : "route");
            }
        }
    }

    return 0;
}

static void handle_link(struct nlmsghdr *nlh)
{
    struct ifinfomsg *ifi = NLMSG_DATA(nlh);
    char ifname[IFNAMSIZ] = {};
    struct rtattr *rta;
    int rtl;

    rta = IFLA_RTA(ifi);
    rtl = IFLA_PAYLOAD(nlh);

    while (RTA_OK(rta, rtl)) {
        if (rta->rta_type == IFLA_IFNAME) {
            strncpy(ifname, RTA_DATA(rta), IFNAMSIZ - 1);
        }
        rta = RTA_NEXT(rta, rtl);
    }

    /* Only handle our swpN interfaces */
    if (strncmp(ifname, "swp", 3) != 0)
        return;

    int swp = atoi(ifname + 3);
    if (swp < 1 || swp > EDGED_MAX_PORTS)
        return;

    if (nlh->nlmsg_type == RTM_NEWLINK) {
        int up = (ifi->ifi_flags & IFF_UP) ? 1 : 0;
        int was_up = edged.ports[swp - 1].enabled;

        if (up != was_up) {
            int port = edged.ports[swp - 1].physical_lane;
            syslog(LOG_INFO, "Link %s %s (port %d)", ifname,
                   up ? "UP" : "DOWN", port);

            if (port > 0) {
                if (up) {
                    /*
                     * Enable port: set mode to current speed.
                     * bmd_port_mode_set() configures XLPORT, XMAC,
                     * PHY speed, and clears EPC_LINK_BMAP (link poll
                     * will re-enable when PHY link comes up).
                     */
                    bmd_port_mode_t mode;
                    if (edged.ports[swp - 1].speed >= 40000)
                        mode = bmdPortMode40000fd;
                    else
                        mode = bmdPortMode10000fd;
                    bmd_port_mode_set(edged.unit, port, mode, 0);
                } else {
                    /*
                     * Disable port: set mode to disabled.
                     * This disables MAC RX/TX and clears EPC_LINK_BMAP.
                     */
                    bmd_port_mode_set(edged.unit, port,
                                      bmdPortModeDisabled, 0);
                }
            }
            edged.ports[swp - 1].enabled = up;
        }
    }
}

static void handle_route(struct nlmsghdr *nlh)
{
    struct rtmsg *rtm = NLMSG_DATA(nlh);
    struct rtattr *rta;
    int rtl;
    uint32_t dst = 0, gw = 0;
    int oif = 0;
    uint32_t gws[64];      /* gateways, network byte order */
    int ngw = 0;
    uint8_t dst6[16] = {0};
    uint8_t gws6[64][16];  /* v6 gateways, network byte order */
    int is6;

    if (rtm->rtm_family != AF_INET && rtm->rtm_family != AF_INET6)
        return;
    is6 = (rtm->rtm_family == AF_INET6);

    /* Skip non-main table routes */
    if (rtm->rtm_table != RT_TABLE_MAIN)
        return;

    rta = RTM_RTA(rtm);
    rtl = RTM_PAYLOAD(nlh);

    while (RTA_OK(rta, rtl)) {
        switch (rta->rta_type) {
        case RTA_DST:
            if (is6) memcpy(dst6, RTA_DATA(rta), 16);
            else     memcpy(&dst, RTA_DATA(rta), 4);
            break;
        case RTA_GATEWAY:
            if (is6) {
                if (ngw < 64) memcpy(gws6[ngw++], RTA_DATA(rta), 16);
            } else {
                memcpy(&gw, RTA_DATA(rta), 4);
                if (ngw < 64) gws[ngw++] = gw;
            }
            break;
        case RTA_OIF:
            oif = *(int *)RTA_DATA(rta);
            break;
        case RTA_MULTIPATH: {
            /* ECMP route: a sequence of struct rtnexthop, each with its own
             * nested RTA_GATEWAY. Collect every gateway. */
            struct rtnexthop *rtnh = RTA_DATA(rta);
            int len = RTA_PAYLOAD(rta);
            while (RTNH_OK(rtnh, len)) {
                struct rtattr *nha = RTNH_DATA(rtnh);
                int nhalen = rtnh->rtnh_len - sizeof(*rtnh);
                while (RTA_OK(nha, nhalen)) {
                    if (nha->rta_type == RTA_GATEWAY && ngw < 64) {
                        if (is6) memcpy(gws6[ngw++], RTA_DATA(nha), 16);
                        else     memcpy(&gws[ngw++], RTA_DATA(nha), 4);
                    }
                    nha = RTA_NEXT(nha, nhalen);
                }
                rtnh = RTNH_NEXT(rtnh);
            }
            break;
        }
        }
        rta = RTA_NEXT(rta, rtl);
    }

    if (nlh->nlmsg_type == RTM_NEWROUTE) {
        if (is6) {
            if (ngw > 0)
                l3_route_add_paths_v6(dst6, rtm->rtm_dst_len, gws6, ngw);
            /* else: v6 connected route — no chip transit entry needed */
        } else if (ngw > 0) {
            /* Program transit/ECMP into the chip (host byte order). */
            uint32_t dst_host = ntohl(dst);
            uint32_t gw_host[64];
            for (int i = 0; i < ngw; i++) gw_host[i] = ntohl(gws[i]);
            l3_route_add_paths(dst_host, rtm->rtm_dst_len, gw_host, ngw);
        } else {
            /* connected / no-gateway route — legacy path (stub). */
            l3_route_add(rtm->rtm_family, &dst, rtm->rtm_dst_len, &gw, oif);
        }
    } else if (nlh->nlmsg_type == RTM_DELROUTE) {
        if (is6)
            l3_route_del_v6(dst6, rtm->rtm_dst_len);
        else
            l3_route_del(rtm->rtm_family, &dst, rtm->rtm_dst_len);
    }
}

static void handle_neigh(struct nlmsghdr *nlh)
{
    struct ndmsg *ndm = NLMSG_DATA(nlh);
    struct rtattr *rta;
    int rtl;
    void *dst = NULL;
    uint8_t *lladdr = NULL;

    rta = NDA_RTA(ndm);
    rtl = NDA_PAYLOAD(nlh);

    while (RTA_OK(rta, rtl)) {
        switch (rta->rta_type) {
        case NDA_DST:
            dst = RTA_DATA(rta);
            break;
        case NDA_LLADDR:
            lladdr = RTA_DATA(rta);
            break;
        }
        rta = RTA_NEXT(rta, rtl);
    }

    if (!dst)
        return;

    if (nlh->nlmsg_type == RTM_NEWNEIGH && lladdr) {
        /* ARP/ND entry learned */
        if (ndm->ndm_state & (NUD_REACHABLE | NUD_PERMANENT | NUD_STALE)) {
            l2_mac_add(lladdr, ndm->ndm_ifindex);
            if (ndm->ndm_family == AF_INET || ndm->ndm_family == AF_INET6)
                l3_host_add(ndm->ndm_family, dst, lladdr, ndm->ndm_ifindex);
        }
    } else if (nlh->nlmsg_type == RTM_DELNEIGH) {
        if (lladdr)
            l2_mac_del(lladdr, ndm->ndm_ifindex);
        if (dst)
            l3_host_del(ndm->ndm_family, dst);
    }
}

void netlink_poll(void)
{
    char buf[NETLINK_BUF_SIZE];
    ssize_t len;
    struct nlmsghdr *nlh;

    if (edged.netlink_fd <= 0)
        return;

    len = recv(edged.netlink_fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (len <= 0)
        return;

    for (nlh = (struct nlmsghdr *)buf;
         NLMSG_OK(nlh, (unsigned int)len);
         nlh = NLMSG_NEXT(nlh, len)) {

        switch (nlh->nlmsg_type) {
        case RTM_NEWLINK:
        case RTM_DELLINK:
            handle_link(nlh);
            break;

        case RTM_NEWROUTE:
        case RTM_DELROUTE:
            handle_route(nlh);
            break;

        case RTM_NEWNEIGH:
        case RTM_DELNEIGH:
            handle_neigh(nlh);
            break;

        case RTM_NEWADDR: {
            struct ifaddrmsg *ifa = NLMSG_DATA(nlh);
            char ifname[IFNAMSIZ] = "";
            if_indextoname(ifa->ifa_index, ifname);
            syslog(LOG_INFO,
                   "RTM_NEWADDR: family=%d ifidx=%d ifname=%s prefix=%d",
                   ifa->ifa_family, ifa->ifa_index, ifname, ifa->ifa_prefixlen);
            if (ifa->ifa_family != AF_INET) break;
            if (strncmp(ifname, "swp", 3) != 0) {
                syslog(LOG_INFO, "RTM_NEWADDR: %s not swp*, skip", ifname);
                break;
            }
            int logical_port = atoi(ifname + 3);
            if (logical_port < 1 || logical_port > EDGED_MAX_PORTS) {
                syslog(LOG_WARNING,
                       "RTM_NEWADDR: %s logical_port=%d out of range",
                       ifname, logical_port);
                break;
            }

            struct rtattr *rta = IFA_RTA(ifa);
            int rta_len = IFA_PAYLOAD(nlh);
            uint32_t ip = 0;
            for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
                if (rta->rta_type == IFA_LOCAL || rta->rta_type == IFA_ADDRESS) {
                    ip = ntohl(*(uint32_t *)RTA_DATA(rta));
                    break;
                }
            }
            syslog(LOG_INFO, "RTM_NEWADDR: %s ip=0x%08x logical_port=%d",
                   ifname, ip, logical_port);
            if (ip)
                l3_local_host_add(ip, logical_port);
            break;
        }

        default:
            syslog(LOG_INFO, "NL: unhandled nlmsg_type=%d", nlh->nlmsg_type);
            break;
        }
    }
}

void netlink_redump_routes(void)
{
    /* Periodically re-request a neighbor + route dump on the live socket. A
     * transit/ECMP route whose gateway resolved *after* the route was first seen
     * is skipped on first sight ("no resolved next-hops"); this re-dump reprograms
     * it once the gateway's chip next-hop exists. l3_route_add_paths() is
     * idempotent (skips prefixes already in L3_DEFIP), so re-dumping is cheap and
     * non-destructive. Mirrors the 4610 bcmd periodic FIB re-dump. */
    if (edged.netlink_fd <= 0)
        return;
    struct { struct nlmsghdr nlh; struct rtgenmsg gen; } req;
    int kinds[2] = { RTM_GETNEIGH, RTM_GETROUTE };
    for (int k = 0; k < 2; k++) {
        memset(&req, 0, sizeof(req));
        req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg));
        req.nlh.nlmsg_type = kinds[k];
        req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
        req.nlh.nlmsg_seq = 10 + k;
        req.gen.rtgen_family = AF_INET;
        if (send(edged.netlink_fd, &req, req.nlh.nlmsg_len, 0) < 0)
            syslog(LOG_WARNING, "Netlink: periodic redump type=%d failed: %s",
                   kinds[k], strerror(errno));
    }
}

void netlink_cleanup(void)
{
    if (edged.netlink_fd > 0) {
        close(edged.netlink_fd);
        edged.netlink_fd = 0;
    }
}
