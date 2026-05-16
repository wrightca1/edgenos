/*
 * edged.h - EdgeNOS Switch Daemon common definitions
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __EDGED_H__
#define __EDGED_H__

#include <stdint.h>

/* Port limits */
#define EDGED_MAX_PORTS    52
#define EDGED_SFP_PORTS    48
#define EDGED_QSFP_PORTS   4
#define EDGED_MAX_VLANS    4096

/* Port types */
#define PORT_TYPE_SFP   0
#define PORT_TYPE_QSFP  1

/* Port state */
struct port_state {
    int valid;
    int enabled;
    int link_up;
    int speed;            /* Mbps: 10000 or 40000 */
    int port_type;        /* PORT_TYPE_SFP or PORT_TYPE_QSFP */
    int logical_port;     /* BCM logical port (1-52) */
    int physical_lane;    /* SerDes lane */
    int tun_fd;           /* TUN interface fd */
    char ifname[16];      /* swpN */
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
};

/* Global edged state */
struct edged_state {
    struct port_state ports[EDGED_MAX_PORTS];
    int num_ports;
    int unit;             /* CDK/BMD unit number */
    int netlink_fd;       /* Netlink socket */
};

extern struct edged_state edged;

/* BDE interface */
int bde_open(void);
void bde_close(void);
void bde_dma_pool_reset(void);
void bde_set_dma_endianness(void);

/* CDK/BMD interface */
int cdk_init(void);
int bmd_init_all(void);
int bmd_switching_init_all(void);

/* Datapath configuration (CPU punt, hash, buffers) */
int datapath_init(void);

/* Replicate Cumulus chip-memory state (EPC_LINK_BMAP, L2_USER_ENTRY,
 * EGR_VLAN, FP_TCAM, FP_POLICY_TABLE) from captured dumps. */
int cumulus_replicate_init(void);

#endif /* __EDGED_H__ */
