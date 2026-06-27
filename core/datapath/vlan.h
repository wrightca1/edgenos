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

/* Create one chip-internal service VLAN per swpN (Cumulus 3301-3352)
 * so CPU TX can direct frames via 802.1Q tag instead of HiGig SOB. */
int vlan_init_resv_per_port(void);
int edged_resv_vid_for_port(int logical_port);

/* Create/destroy VLANs */
int vlan_create(int vid);
int vlan_destroy(int vid);

/* Add/remove port from VLAN */
int vlan_port_add(int vid, int swp, int tagged);
int vlan_port_remove(int vid, int swp);

/* L2 forwarding groups: put the listed swp ports into one isolated L2
 * broadcast domain (VLAN `vid`), pulled off their per-port L3 service VLANs.
 * Members forward directly among themselves; isolated from L3-routed ports. */
int vlan_l2_group_apply(int vid, const int *swps, int n);

/* Read /etc/edged/l2-groups.conf and apply each group. Lines:
 *   <vid> <swpA> <swpB> [swpC ...]      (ports may be "swp10" or "10")
 * Returns the number of groups applied. Missing file = 0 (not an error). */
int vlan_load_l2_groups(const char *path);

#endif /* __VLAN_H__ */
