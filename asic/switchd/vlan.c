/*
 * vlan.c - VLAN management
 *
 * Sets up default VLAN 1 with all ports as untagged members.
 * Handles VLAN create/destroy and port membership changes.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <syslog.h>

#include "switchd.h"
#include "vlan.h"

int vlan_init_default(void)
{
    syslog(LOG_INFO, "Setting up default VLAN 1 (all ports untagged)");

    /*
     * BMD switching_init already creates VLAN 1 with all ports.
     * This function is for any additional VLAN setup.
     *
     * With BMD:
     *   bmd_vlan_create(unit, 1);
     *   for each port:
     *     bmd_vlan_port_add(unit, 1, port, 0);  // 0 = untagged
     *     bmd_port_vlan_set(unit, port, 1);       // PVID = 1
     */

    return 0;
}
