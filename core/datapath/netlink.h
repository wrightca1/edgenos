/*
 * netlink.h - Netlink listener for route/neighbor/link events
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __NETLINK_H__
#define __NETLINK_H__

int netlink_init(void);
void netlink_poll(void);
void netlink_cleanup(void);

#endif /* __NETLINK_H__ */
