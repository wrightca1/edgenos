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

#include "edged.h"
#include "vlan.h"
#include "portmap.h"

/* BMD headers */
#include <bmd/bmd.h>

/*
 * Reserved internal VLAN range for hardware L3.
 *
 * Matches Cumulus 2.5 default (/etc/cumulus/switchd.conf:
 *   resv_vlan_range = 3300-3999).
 *
 * EdgeNOS today does host-routed L3 (CPU does the lookup), so we don't
 * actually consume this range yet.  We still reject user creates inside
 * it so that a future hardware-L3 implementation can allocate without
 * colliding with anything an operator configured by hand.
 */
#define EDGED_RESV_VLAN_LO  3300
#define EDGED_RESV_VLAN_HI  3999

static int vid_is_reserved(int vid)
{
    return vid >= EDGED_RESV_VLAN_LO && vid <= EDGED_RESV_VLAN_HI;
}

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
    for (i = 0; i < EDGED_MAX_PORTS; i++) {
        if (!edged.ports[i].valid)
            continue;

        rv = bmd_port_vlan_set(edged.unit,
                               edged.ports[i].physical_lane, 1);
        if (rv < 0) {
            syslog(LOG_WARNING, "VLAN: failed to set PVID on port %d: %d",
                   edged.ports[i].physical_lane, rv);
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
    if (vid_is_reserved(vid)) {
        syslog(LOG_ERR,
               "VLAN create: VID %d is reserved (range %d-%d) for hardware L3 use",
               vid, EDGED_RESV_VLAN_LO, EDGED_RESV_VLAN_HI);
        return -1;
    }

    /*
     * bmd_vlan_create() programs:
     *   VLAN_TAB[vid]: valid=1, port bitmap=empty
     *   EGR_VLAN[vid]: valid=1, untagged bitmap=empty
     *   STG_TAB: default STP group (all ports forwarding)
     */
    rv = bmd_vlan_create(edged.unit, vid);
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

    rv = bmd_vlan_destroy(edged.unit, vid);
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
    uint32_t flags = tagged ? 0 : BMD_VLAN_PORT_F_UNTAGGED;
    rv = bmd_vlan_port_add(edged.unit, vid, logical, flags);
    if (rv < 0) {
        syslog(LOG_WARNING, "VLAN %d port add swp%d failed: %d",
               vid, swp, rv);
        return -1;
    }

    /* If untagged, set PVID to this VLAN */
    if (!tagged) {
        bmd_port_vlan_set(edged.unit, logical, vid);
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

    rv = bmd_vlan_port_remove(edged.unit, vid, logical);
    if (rv < 0) {
        syslog(LOG_WARNING, "VLAN %d port remove swp%d failed: %d",
               vid, swp, rv);
        return -1;
    }

    syslog(LOG_DEBUG, "VLAN %d: removed swp%d", vid, swp);
    return 0;
}
