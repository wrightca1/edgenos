/*
 * l2.c - L2 forwarding table management
 *
 * Programs MAC addresses into the ASIC L2 table via BMD.
 * Handles MAC learning events from netlink neighbor notifications.
 *
 * ASIC L2 table details (from RE docs):
 *   Table ID: 0x1547 (L2_USER_ENTRY)
 *   Entry size: 36 bytes (0x24)
 *   Layout: MAC at offset 0x08, VLAN at 0x14, port at 0x2c
 *   Access: via S-Channel WRITE_MEMORY at soc_schan_op (0x108623e4)
 *
 * BMD L2 API:
 *   bmd_port_mac_addr_add(unit, port, vlan, mac) - static MAC entry
 *   bmd_cpu_mac_addr_add(unit, vlan, mac) - CPU-destined MAC
 *   Hardware MAC learning is enabled by bmd_switching_init()
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <net/if.h>

#include "switchd.h"
#include "l2.h"
#include "portmap.h"

/* BMD headers */
#include <bmd/bmd.h>

/* Default VLAN */
#define DEFAULT_VLAN 1

/*
 * Read base MAC address from eth0 (management port).
 * Cumulus assigns MACs as: eth0 = base, swp1 = base+1, swp2 = base+2, etc.
 * Base MAC for this switch: 80:a2:35:81:ca:ae (captured from Cumulus).
 * Falls back to hardcoded MAC if sysfs read fails.
 */
static int read_mgmt_mac(bmd_mac_addr_t *mac)
{
    FILE *f;
    unsigned int m[6];

    f = fopen("/sys/class/net/eth0/address", "r");
    if (f) {
        if (fscanf(f, "%x:%x:%x:%x:%x:%x",
                   &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
            int i;
            for (i = 0; i < 6; i++)
                mac->b[i] = (uint8_t)m[i];
            fclose(f);
            return 0;
        }
        fclose(f);
    }

    /* Fallback: hardcoded base MAC from Cumulus capture */
    mac->b[0] = 0x80; mac->b[1] = 0xa2; mac->b[2] = 0x35;
    mac->b[3] = 0x81; mac->b[4] = 0xca; mac->b[5] = 0xae;
    return 0;
}

int l2_init(void)
{
    bmd_mac_addr_t mgmt_mac;
    int rv;

    /*
     * bmd_switching_init() already configured:
     * - L2 table reset
     * - Default VLAN (1) with all ports as members
     * - Hardware MAC learning enabled (PORT_TABm CML_FLAGS=8)
     * - L2 aging timer active
     */

    /*
     * Add CPU MAC address to L2 table so packets destined to the
     * switch management address get forwarded to CPU (via RX DMA)
     * instead of being flooded to all ports.
     *
     * Without this, ARP replies, DHCP responses, and any traffic
     * addressed to the switch's own MAC will flood or be dropped.
     */
    read_mgmt_mac(&mgmt_mac);

    rv = bmd_cpu_mac_addr_add(switchd.unit, DEFAULT_VLAN, &mgmt_mac);
    if (rv < 0) {
        syslog(LOG_WARNING, "L2: failed to add CPU MAC: rv=%d", rv);
    } else {
        syslog(LOG_INFO, "L2: CPU MAC %02x:%02x:%02x:%02x:%02x:%02x added to VLAN %d",
               mgmt_mac.b[0], mgmt_mac.b[1], mgmt_mac.b[2],
               mgmt_mac.b[3], mgmt_mac.b[4], mgmt_mac.b[5],
               DEFAULT_VLAN);
    }

    syslog(LOG_INFO, "L2 forwarding initialized");
    return 0;
}

int l2_mac_add(const uint8_t *mac, int ifindex)
{
    char ifname[IFNAMSIZ];
    int swp, logical, rv;

    if (!if_indextoname(ifindex, ifname))
        return -1;

    if (strncmp(ifname, "swp", 3) != 0)
        return 0;  /* Not our interface */

    swp = atoi(ifname + 3);
    if (swp < 1 || swp > SWITCHD_MAX_PORTS)
        return -1;

    /* Use CDK physical port for BMD calls */
    logical = switchd.ports[swp - 1].physical_lane;
    if (logical <= 0)
        return -1;

    syslog(LOG_DEBUG, "L2 add: %02x:%02x:%02x:%02x:%02x:%02x on port %d (swp%d)",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], logical, swp);

    /*
     * Program static MAC entry into ASIC L2 table.
     * bmd_port_mac_addr_add() internally:
     *   1. Builds L2_USER_ENTRY in 36-byte buffer
     *   2. Sets MAC, VLAN, port, STATIC bit
     *   3. Writes via S-Channel WRITE_MEMORY to table 0x1547
     *
     * From OpenMDK bcm56840_a0_bmd_port_mac_addr_add.c:
     *   L2Xm_MAC_ADDRf_SET() - sets MAC in entry
     *   L2Xm_VLAN_IDf_SET() - sets VLAN
     *   L2Xm_PORT_NUMf_SET() - sets destination port
     *   L2Xm_STATIC_BITf_SET() - marks as static (won't age)
     *   L2Xm_VALIDf_SET() - marks entry valid
     *   WRITE_L2Xm() - writes to ASIC via S-Channel
     */
    bmd_mac_addr_t bmac;
    memcpy(bmac.b, mac, 6);
    rv = bmd_port_mac_addr_add(switchd.unit, logical, DEFAULT_VLAN, &bmac);
    if (rv < 0) {
        syslog(LOG_WARNING, "L2 add failed: %02x:%02x:%02x:%02x:%02x:%02x "
               "port %d: rv=%d",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], logical, rv);
        return -1;
    }

    return 0;
}

int l2_mac_del(const uint8_t *mac, int ifindex)
{
    char ifname[IFNAMSIZ];
    int swp, logical, rv;

    if (!if_indextoname(ifindex, ifname))
        return -1;

    if (strncmp(ifname, "swp", 3) != 0)
        return 0;

    swp = atoi(ifname + 3);
    if (swp < 1 || swp > SWITCHD_MAX_PORTS)
        return -1;

    /* Use CDK physical port for BMD calls */
    logical = switchd.ports[swp - 1].physical_lane;
    if (logical <= 0)
        return -1;

    syslog(LOG_DEBUG, "L2 del: %02x:%02x:%02x:%02x:%02x:%02x on port %d (swp%d)",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], logical, swp);

    /*
     * Remove MAC entry from ASIC L2 table.
     * bmd_port_mac_addr_remove() invalidates the entry.
     */
    bmd_mac_addr_t bmac;
    memcpy(bmac.b, mac, 6);
    rv = bmd_port_mac_addr_remove(switchd.unit, logical, DEFAULT_VLAN, &bmac);
    if (rv < 0) {
        syslog(LOG_DEBUG, "L2 del failed (may not exist): rv=%d", rv);
    }

    return 0;
}
