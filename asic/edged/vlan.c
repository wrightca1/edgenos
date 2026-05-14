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

int edged_resv_vid_for_port(int logical_port)
{
    /* Cumulus mapping (from 30_full_dump.txt):
     *   xe0 (swp1) -> 3301
     *   xe1 (swp2) -> 3302
     *   xeN        -> 3301 + N
     * logical_port is 1..52, so vid = 3300 + logical_port. */
    return EDGED_RESV_VLAN_LO + logical_port;
}

/*
 * vlan_init_resv_per_port — re-create Cumulus's per-port "service
 * VLAN" scheme.  For every front-panel port, create a fresh VID
 * (3300+logical_port) with exactly two members: the CPU (tagged)
 * and the front-panel port (untagged on egress).
 *
 * Why: BCM Trident's CPU TX path through HiGig SOB ("send to port X")
 * happens to put a chip-default frame on the wire that the Nexus
 * accepts for ARP (ethertype 0x0806) but not for IPv4 (0x0800).  In
 * the Cumulus baseline (cumulus_baseline_2013/30_full_dump.txt) the
 * CPU instead sends frames tagged with the service VID and lets the
 * chip's normal L2 forwarding do the work — VID has exactly one
 * untagged member port, so the frame can only egress one place, and
 * the chip strips the tag on the way out so the wire frame is the
 * clean tagged-free Ethernet packet the Nexus expects.
 *
 * Cumulus's vlan show (truncated):
 *     vlan 1    ports none, untagged none
 *     vlan 3301 ports cpu,xe0,  untagged xe0
 *     vlan 3302 ports cpu,xe1,  untagged xe1
 *     ...
 *
 * We don't blow away VLAN 1 here — leaving it populated by bmd
 * doesn't hurt, since service VID forwarding takes the path it does
 * regardless.
 */
int vlan_init_resv_per_port(void)
{
    int i, rv;
    int created = 0, port_added = 0, cmic_added = 0;
    const int cmic = 0;

    syslog(LOG_INFO, "VLAN: creating per-port service VIDs (Cumulus 3301+)");

    /* Mirror Cumulus baseline: remove every port from VLAN 1 so it
     * stays empty.  Service VIDs (next loop) carry all CPU<->swpN
     * traffic.  Leaving VLAN 1 populated risks the chip's L2
     * forwarding picking the wrong egress when CMIC injects a
     * tagged frame on a service VID — saw exactly this in
     * cumulus_baseline_2013/30_full_dump.txt:
     *     vlan 1    ports none, untagged none */
    for (i = 0; i < EDGED_MAX_PORTS; i++) {
        struct port_state *p = &edged.ports[i];
        if (!p->valid)
            continue;
        rv = bmd_vlan_port_remove(edged.unit, 1, p->physical_lane);
        if (rv < 0 && rv != -8 /* CDK_E_NOT_FOUND */) {
            syslog(LOG_DEBUG, "VLAN 1: remove port %d rv=%d (ok if absent)",
                   p->physical_lane, rv);
        }
    }
    /* Also remove CPU. */
    (void)bmd_vlan_port_remove(edged.unit, 1, 0);

    for (i = 0; i < EDGED_MAX_PORTS; i++) {
        struct port_state *p = &edged.ports[i];
        if (!p->valid)
            continue;
        int vid = edged_resv_vid_for_port(p->logical_port);

        rv = bmd_vlan_create(edged.unit, vid);
        if (rv < 0) {
            syslog(LOG_WARNING,
                   "resv VLAN: create VID %d for %s failed: %d",
                   vid, p->ifname, rv);
            continue;
        }
        created++;

        /* swpN as untagged member (egress strips tag). */
        rv = bmd_vlan_port_add(edged.unit, vid, p->physical_lane,
                               BMD_VLAN_PORT_F_UNTAGGED);
        if (rv == 0)
            port_added++;
        else
            syslog(LOG_WARNING,
                   "resv VLAN: untagged-add port %d to VID %d failed: %d",
                   p->physical_lane, vid, rv);

        /* CPU as tagged member (so CPU can inject frames into this VID). */
        rv = bmd_vlan_port_add(edged.unit, vid, cmic, 0);
        if (rv == 0)
            cmic_added++;
        else
            syslog(LOG_WARNING,
                   "resv VLAN: tagged-add CMIC to VID %d failed: %d",
                   vid, rv);

        /* Set swpN's PVID to this service VID so incoming UNTAGGED
         * frames from the Nexus get classified into this service
         * VID (whose only members are CPU + this swpN), then punted
         * to CPU.  Cumulus does the same — without this, Nexus's
         * untagged unicast reply gets classified into VID 1 which
         * is now empty -> dropped. */
        rv = bmd_port_vlan_set(edged.unit, p->physical_lane, vid);
        if (rv < 0) {
            syslog(LOG_WARNING,
                   "resv VLAN: set PVID %d on port %d failed: %d",
                   vid, p->physical_lane, rv);
        }
    }

    syslog(LOG_INFO,
           "VLAN: %d service VIDs created, %d port-adds, %d cmic-adds",
           created, port_added, cmic_added);

    /*
     * Set STP state to FORWARDING for CPU + every swpN port in STG 1.
     *
     * `bmd_vlan_create` puts every new VLAN in STG 1 (verified —
     * VLAN_TABm.STG = 1 after creation), but the chip defaults all
     * ports in non-default STGs to BLOCKING.  Without this, frames
     * classified into our service VIDs hit STP_BLOCKING at the
     * bridging stage and get silently dropped (no rx_drops counter
     * fires for STP drops on Trident+).  That's the actual reason
     * our chip MAC RX worked but RX never reached the CPU port.
     *
     * bcm56840_a0_bmd_port_stp_set always targets STG 1 (the value
     * is hardcoded in its READ_STG_TABm call).
     */
    {
        int fwd_count = 0;
        rv = bmd_port_stp_set(edged.unit, 0 /* CMIC */,
                              bmdSpanningTreeForwarding);
        if (rv == 0) {
            fwd_count++;
        } else {
            syslog(LOG_WARNING,
                   "STG1: FORWARDING on CPU port failed: %d", rv);
        }
        for (i = 0; i < EDGED_MAX_PORTS; i++) {
            struct port_state *p = &edged.ports[i];
            if (!p->valid)
                continue;
            rv = bmd_port_stp_set(edged.unit, p->physical_lane,
                                  bmdSpanningTreeForwarding);
            if (rv == 0) {
                fwd_count++;
            } else {
                syslog(LOG_WARNING,
                       "STG1: FORWARDING on %s (port %d) failed: %d",
                       p->ifname, p->physical_lane, rv);
            }
        }
        syslog(LOG_INFO,
               "STG1: FORWARDING set on %d ports (CPU + %d swpN)",
               fwd_count, fwd_count - 1);
    }

    return 0;
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
