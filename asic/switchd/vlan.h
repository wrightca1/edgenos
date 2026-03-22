/*
 * vlan.h - VLAN management
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __VLAN_H__
#define __VLAN_H__

/* Initialize default VLAN 1 with all ports untagged */
int vlan_init_default(void);

/* Create/destroy VLANs */
int vlan_create(int vid);
int vlan_destroy(int vid);

/* Add/remove port from VLAN */
int vlan_port_add(int vid, int swp, int tagged);
int vlan_port_remove(int vid, int swp);

#endif /* __VLAN_H__ */
