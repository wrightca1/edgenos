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

#include "switchd.h"
#include "netlink.h"
#include "portmap.h"
#include "l2.h"
#include "l3.h"

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

    switchd.netlink_fd = fd;
    syslog(LOG_INFO, "Netlink listener initialized");
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
    if (swp < 1 || swp > SWITCHD_MAX_PORTS)
        return;

    if (nlh->nlmsg_type == RTM_NEWLINK) {
        int up = (ifi->ifi_flags & IFF_UP) ? 1 : 0;
        syslog(LOG_INFO, "Link %s %s", ifname, up ? "UP" : "DOWN");

        /* TODO: Enable/disable port on ASIC
         * bmd_port_mode_set(unit, logical_port, up ? enabled : disabled);
         */
        switchd.ports[swp - 1].enabled = up;
    }
}

static void handle_route(struct nlmsghdr *nlh)
{
    struct rtmsg *rtm = NLMSG_DATA(nlh);
    struct rtattr *rta;
    int rtl;
    uint32_t dst = 0, gw = 0;
    int oif = 0;

    if (rtm->rtm_family != AF_INET && rtm->rtm_family != AF_INET6)
        return;

    /* Skip non-main table routes */
    if (rtm->rtm_table != RT_TABLE_MAIN)
        return;

    rta = RTM_RTA(rtm);
    rtl = RTM_PAYLOAD(nlh);

    while (RTA_OK(rta, rtl)) {
        switch (rta->rta_type) {
        case RTA_DST:
            if (rtm->rtm_family == AF_INET)
                memcpy(&dst, RTA_DATA(rta), 4);
            break;
        case RTA_GATEWAY:
            if (rtm->rtm_family == AF_INET)
                memcpy(&gw, RTA_DATA(rta), 4);
            break;
        case RTA_OIF:
            oif = *(int *)RTA_DATA(rta);
            break;
        }
        rta = RTA_NEXT(rta, rtl);
    }

    if (nlh->nlmsg_type == RTM_NEWROUTE) {
        syslog(LOG_DEBUG, "Route add: dst=%08x/%d gw=%08x oif=%d",
               dst, rtm->rtm_dst_len, gw, oif);
        l3_route_add(rtm->rtm_family, &dst, rtm->rtm_dst_len, &gw, oif);
    } else if (nlh->nlmsg_type == RTM_DELROUTE) {
        syslog(LOG_DEBUG, "Route del: dst=%08x/%d", dst, rtm->rtm_dst_len);
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

    if (switchd.netlink_fd <= 0)
        return;

    len = recv(switchd.netlink_fd, buf, sizeof(buf), MSG_DONTWAIT);
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

        default:
            break;
        }
    }
}

void netlink_cleanup(void)
{
    if (switchd.netlink_fd > 0) {
        close(switchd.netlink_fd);
        switchd.netlink_fd = 0;
    }
}
