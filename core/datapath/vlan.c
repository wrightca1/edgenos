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
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "edged.h"
#include "vlan.h"
#include "portmap.h"

/* BMD headers */
#include <bmd/bmd.h>
#include <cdk/chip/bcm56840_a0_defs.h>
#include <cdk/arch/xgs_chip.h>

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

/* Current L2-group VID per swp (0 = default per-port L3 isolation). */
static int port_group_vid[EDGED_MAX_PORTS + 1];

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

    /*
     * Enable IPv4/IPv6 L3 routing in the VLAN_PROFILE the service VLANs use.
     *
     * On Trident each VLAN points (VLAN_TAB.VLAN_PROFILE_PTR) at a
     * VLAN_PROFILE_TAB entry whose IPV4L3_ENABLE/IPV6L3_ENABLE bits gate
     * whether a MY_STATION-terminated IP frame in that VLAN is submitted to
     * the L3 route lookup.  We never set this -> our VLANs used a profile with
     * L3 DISABLED, so terminated IPv4 to our own IP was never looked up and
     * fell through to RIPD4 (root cause of the cold-boot ICMP-to-self drop,
     * 2026-06-04: confirmed the L3 DEFIP HIT bit stayed 0 even for a match-all
     * route).  Enable L3 on all 128 profiles (we don't do per-VLAN L3 policy). */
    {
        int vid0 = edged_resv_vid_for_port(1);
        VLAN_TABm_t vt;
        int ptr = -1;
        if (READ_VLAN_TABm(edged.unit, vid0, &vt) == 0)
            ptr = VLAN_TABm_VLAN_PROFILE_PTRf_GET(vt);

        int prof, done = 0;
        for (prof = 0; prof <= VLAN_PROFILE_TABm_MAX; prof++) {
            VLAN_PROFILE_TABm_t vp;
            if (READ_VLAN_PROFILE_TABm(edged.unit, prof, &vp) != 0)
                continue;
            if (prof == 0 || prof == ptr)
                syslog(LOG_INFO,
                       "VLAN_PROFILE[%d] before: IPV4L3=%d IPV6L3=%d",
                       prof,
                       VLAN_PROFILE_TABm_IPV4L3_ENABLEf_GET(vp),
                       VLAN_PROFILE_TABm_IPV6L3_ENABLEf_GET(vp));
            VLAN_PROFILE_TABm_IPV4L3_ENABLEf_SET(vp, 1);
            VLAN_PROFILE_TABm_IPV6L3_ENABLEf_SET(vp, 1);
            if (WRITE_VLAN_PROFILE_TABm(edged.unit, prof, vp) == 0)
                done++;
        }
        syslog(LOG_INFO,
               "VLAN_PROFILE: IPv4/IPv6 L3 enabled on %d profiles "
               "(service VLAN %d uses profile ptr=%d)", done, vid0, ptr);
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

/*
 * vlan_l2_group_apply — make the listed swp ports one isolated L2 group.
 *
 * The default datapath puts every swpN on its own per-port service VLAN
 * (edged_resv_vid_for_port, members CPU+port) so all inter-port traffic is
 * CPU/L3-routed.  An L2 group instead pulls the chosen ports OFF their
 * service VLANs and onto a single shared VLAN `vid` (untagged, PVID set,
 * STG-1 forwarding).  Members then bridge directly to each other via the
 * chip's L2 table, and are isolated from the L3-routed ports (which remain
 * on their own service VLANs) and from other groups.
 *
 * Uses physical_lane for the bmd_* calls — mirroring the proven path in
 * vlan_init_resv_per_port (NOT logical_port).
 */
int vlan_l2_group_apply(int vid, const int *swps, int n)
{
    int i, rv, added = 0;

    if (vid < 2 || vid > 4094 || vid_is_reserved(vid)) {
        syslog(LOG_ERR, "L2 group: invalid/reserved VID %d "
               "(must be 2-4094, outside %d-%d)",
               vid, EDGED_RESV_VLAN_LO, EDGED_RESV_VLAN_HI);
        return -1;
    }

    rv = bmd_vlan_create(edged.unit, vid);
    if (rv < 0)
        /* tolerate "already exists" — the port-adds below still apply */
        syslog(LOG_INFO, "L2 group %d: vlan_create rv=%d (continuing)", vid, rv);

    for (i = 0; i < n; i++) {
        int swp = swps[i];
        struct port_state *p;
        int rvid;

        if (swp < 1 || swp > EDGED_MAX_PORTS || !edged.ports[swp - 1].valid) {
            syslog(LOG_WARNING, "L2 group %d: swp%d invalid/absent", vid, swp);
            continue;
        }
        p = &edged.ports[swp - 1];
        rvid = edged_resv_vid_for_port(p->logical_port);

        /* Pull the port off its per-port L3 service VLAN and VLAN 1 so it
         * only forwards within the group (rv -8 = NOT_FOUND = already off). */
        (void)bmd_vlan_port_remove(edged.unit, rvid, p->physical_lane);
        (void)bmd_vlan_port_remove(edged.unit, 1, p->physical_lane);

        rv = bmd_vlan_port_add(edged.unit, vid, p->physical_lane,
                               BMD_VLAN_PORT_F_UNTAGGED);
        if (rv < 0) {
            syslog(LOG_WARNING, "L2 group %d: add %s (lane %d) failed: %d",
                   vid, p->ifname, p->physical_lane, rv);
            continue;
        }

        /* Untagged ingress on this port classifies into the group VLAN. */
        rv = bmd_port_vlan_set(edged.unit, p->physical_lane, vid);
        if (rv < 0)
            syslog(LOG_WARNING, "L2 group %d: PVID on %s failed: %d",
                   vid, p->ifname, rv);

        /* New VLAN is in STG 1; ports default to BLOCKING there. */
        rv = bmd_port_stp_set(edged.unit, p->physical_lane,
                              bmdSpanningTreeForwarding);
        if (rv < 0)
            syslog(LOG_WARNING, "L2 group %d: STP FWD on %s failed: %d",
                   vid, p->ifname, rv);

        added++;
        port_group_vid[swp] = vid;          /* track for live reset/re-sync */
        syslog(LOG_INFO, "L2 group %d: + %s (lane %d, moved off service VID %d)",
               vid, p->ifname, p->physical_lane, rvid);
    }

    syslog(LOG_INFO, "L2 group VLAN %d: %d/%d member ports active", vid, added, n);
    return added > 0 ? 0 : -1;
}

/* Restore one swp from its current L2 group back to its per-port L3 service VLAN
 * (the reverse of the per-port move in vlan_l2_group_apply). */
static int vlan_port_restore_l3(int swp)
{
    struct port_state *p;
    int rvid, gvid;

    if (swp < 1 || swp > EDGED_MAX_PORTS || !edged.ports[swp - 1].valid)
        return -1;
    gvid = port_group_vid[swp];
    if (gvid == 0)
        return 0;                           /* already L3-isolated */
    p = &edged.ports[swp - 1];
    rvid = edged_resv_vid_for_port(p->logical_port);
    (void)bmd_vlan_port_remove(edged.unit, gvid, p->physical_lane);
    (void)bmd_vlan_port_add(edged.unit, rvid, p->physical_lane, BMD_VLAN_PORT_F_UNTAGGED);
    (void)bmd_port_vlan_set(edged.unit, p->physical_lane, rvid);
    (void)bmd_port_stp_set(edged.unit, p->physical_lane, bmdSpanningTreeForwarding);
    port_group_vid[swp] = 0;
    syslog(LOG_INFO, "L2 group: %s restored to L3 service VID %d", p->ifname, rvid);
    return 0;
}

/* Tear down every L2 group — restore all grouped ports to per-port L3 isolation. */
void vlan_l2_reset(void)
{
    int swp, n = 0;
    for (swp = 1; swp <= EDGED_MAX_PORTS; swp++)
        if (port_group_vid[swp] && vlan_port_restore_l3(swp) == 0)
            n++;
    if (n)
        syslog(LOG_INFO, "L2 groups: reset %d ports to L3", n);
}

/* Live re-sync: tear down current groups, then re-apply from the config file.
 * Driven by SIGHUP so the CLI / web UI can flip ports without a chip re-init. */
int vlan_l2_resync(const char *path)
{
    vlan_l2_reset();
    return vlan_load_l2_groups(path);
}

/*
 * vlan_load_l2_groups — parse /etc/edged/l2-groups.conf and apply each group.
 * Format (one group per line):  <vid> <swpA> <swpB> [swpC ...]
 * Ports accept "swp10" or "10".  '#' comments and blank lines ignored.
 */
int vlan_load_l2_groups(const char *path)
{
    FILE *f;
    char line[256];
    int groups = 0;

    f = fopen(path, "r");
    if (!f) {
        syslog(LOG_INFO, "L2 groups: no %s (none configured)", path);
        return 0;
    }

    while (fgets(line, sizeof(line), f)) {
        char *s = line, *tok;
        int vid, swps[EDGED_MAX_PORTS], nswp = 0;

        while (*s == ' ' || *s == '\t')
            s++;
        if (*s == '#' || *s == '\n' || *s == '\0')
            continue;

        tok = strtok(s, " \t\r\n");
        if (!tok)
            continue;
        vid = atoi(tok);

        while ((tok = strtok(NULL, " \t\r\n")) != NULL && nswp < EDGED_MAX_PORTS) {
            if (strncmp(tok, "swp", 3) == 0)
                tok += 3;
            int swp = atoi(tok);
            if (swp > 0)
                swps[nswp++] = swp;
        }

        if (vid > 0 && nswp > 0) {
            if (vlan_l2_group_apply(vid, swps, nswp) == 0)
                groups++;
        } else {
            syslog(LOG_WARNING, "L2 groups: skipping malformed line: %s", line);
        }
    }

    fclose(f);
    syslog(LOG_INFO, "L2 groups: applied %d group(s) from %s", groups, path);
    return groups;
}
