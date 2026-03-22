/*
 * switchd.h - EdgeNOS Switch Daemon common definitions
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __SWITCHD_H__
#define __SWITCHD_H__

#include <stdint.h>

/* Port limits */
#define SWITCHD_MAX_PORTS    52
#define SWITCHD_SFP_PORTS    48
#define SWITCHD_QSFP_PORTS   4
#define SWITCHD_MAX_VLANS    4096

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

/* Global switchd state */
struct switchd_state {
    struct port_state ports[SWITCHD_MAX_PORTS];
    int num_ports;
    int unit;             /* CDK/BMD unit number */
    int netlink_fd;       /* Netlink socket */
};

extern struct switchd_state switchd;

/* BDE interface */
int bde_open(void);
void bde_close(void);

/* CDK/BMD interface */
int cdk_init(void);
int bmd_init_all(void);
int bmd_switching_init_all(void);

#endif /* __SWITCHD_H__ */
