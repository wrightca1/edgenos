/*
 * vlan.c - VLAN management
 *
 * Sets up default VLAN 1 with all ports as untagged members.
 * Handles VLAN create/destroy and port membership changes.
 *
 * ASIC VLAN table details (from RE docs):
 *   VLAN_TAB: per-VLAN port bitmap, STP group, profile index
 *   EGR_VLAN: egress VLAN table (untagged port bitmap)
 *   bmd_switching_init() creates VLAN 1 with all ports
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <syslog.h>

#include "switchd.h"
#include "vlan.h"
#include "portmap.h"

/* BMD headers */
#include <bmd/bmd.h>

int vlan_init_default(void)
{
    int i, rv;

    syslog(LOG_INFO, "Verifying default VLAN 1 (all ports untagged)");

    /*
     * bmd_switching_init() already creates VLAN 1 with all ports.
     * Verify by setting PVID=1 on each port explicitly.
     *
     * bmd_port_vlan_set() programs the PORT_TABm entry for each port
     * to set the default VLAN ID used for untagged ingress packets.
     */
    for (i = 0; i < SWITCHD_MAX_PORTS; i++) {
        if (!switchd.ports[i].valid)
            continue;

        rv = bmd_port_vlan_set(switchd.unit,
                               switchd.ports[i].logical_port, 1);
        if (rv < 0) {
            syslog(LOG_WARNING, "VLAN: failed to set PVID on port %d: %d",
                   switchd.ports[i].logical_port, rv);
        }
    }

    return 0;
}

int vlan_create(int vid)
{
    int rv;

    if (vid < 1 || vid > 4094) {
        syslog(LOG_ERR, "VLAN create: invalid VID %d", vid);
        return -1;
    }

    /*
     * bmd_vlan_create() programs:
     *   VLAN_TAB[vid]: valid=1, port bitmap=empty
     *   EGR_VLAN[vid]: valid=1, untagged bitmap=empty
     *   STG_TAB: default STP group (all ports forwarding)
     */
    rv = bmd_vlan_create(switchd.unit, vid);
    if (rv < 0) {
        syslog(LOG_ERR, "VLAN create %d failed: %d", vid, rv);
        return -1;
    }

    syslog(LOG_INFO, "VLAN %d created", vid);
    return 0;
}

int vlan_destroy(int vid)
{
    int rv;

    if (vid < 2 || vid > 4094) {
        syslog(LOG_ERR, "VLAN destroy: invalid VID %d", vid);
        return -1;
    }

    rv = bmd_vlan_destroy(switchd.unit, vid);
    if (rv < 0) {
        syslog(LOG_ERR, "VLAN destroy %d failed: %d", vid, rv);
        return -1;
    }

    syslog(LOG_INFO, "VLAN %d destroyed", vid);
    return 0;
}

int vlan_port_add(int vid, int swp, int tagged)
{
    int logical, rv;

    logical = portmap_swp_to_logical(swp);
    if (logical < 0)
        return -1;

    /*
     * bmd_vlan_port_add() updates:
     *   VLAN_TAB[vid]: adds port to port bitmap
     *   EGR_VLAN[vid]: if untagged, adds to untagged bitmap
     *
     * flags: 0 = untagged, BMD_VLAN_PORT_F_TAG = tagged
     */
    uint32_t flags = tagged ? BMD_VLAN_PORT_F_TAG : 0;
    rv = bmd_vlan_port_add(switchd.unit, vid, logical, flags);
    if (rv < 0) {
        syslog(LOG_WARNING, "VLAN %d port add swp%d failed: %d",
               vid, swp, rv);
        return -1;
    }

    /* If untagged, set PVID to this VLAN */
    if (!tagged) {
        bmd_port_vlan_set(switchd.unit, logical, vid);
    }

    syslog(LOG_DEBUG, "VLAN %d: added swp%d (%s)", vid, swp,
           tagged ? "tagged" : "untagged");
    return 0;
}

int vlan_port_remove(int vid, int swp)
{
    int logical, rv;

    logical = portmap_swp_to_logical(swp);
    if (logical < 0)
        return -1;

    rv = bmd_vlan_port_remove(switchd.unit, vid, logical);
    if (rv < 0) {
        syslog(LOG_WARNING, "VLAN %d port remove swp%d failed: %d",
               vid, swp, rv);
        return -1;
    }

    syslog(LOG_DEBUG, "VLAN %d: removed swp%d", vid, swp);
    return 0;
}
