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

#endif /* __L3_H__ */
