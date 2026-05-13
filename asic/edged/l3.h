/*
 * l3.h - L3 routing (FIB programming)
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __L3_H__
#define __L3_H__

#include <stdint.h>

int l3_init(void);
int l3_route_add(int family, const void *dst, int prefix_len,
                 const void *gw, int oif);
int l3_route_del(int family, const void *dst, int prefix_len);
int l3_host_add(int family, const void *addr, const uint8_t *mac, int ifindex);
int l3_host_del(int family, const void *addr);

/*
 * Add a MY_STATION_TCAM entry for one of our front-panel MACs so the
 * chip recognises it as a router endpoint and can L3-terminate IPv4
 * frames addressed to it.  Without this, IPv4 unicast from the wire
 * silently drops at the L3 stage (ARP works because it's L2-only).
 */
int l3_my_station_add(const uint8_t *mac, int vlan);

#endif /* __L3_H__ */
