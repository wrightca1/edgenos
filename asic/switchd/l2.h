/*
 * l2.h - L2 forwarding (MAC table management)
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __L2_H__
#define __L2_H__

#include <stdint.h>

int l2_init(void);
int l2_mac_add(const uint8_t *mac, int ifindex);
int l2_mac_del(const uint8_t *mac, int ifindex);

#endif /* __L2_H__ */
