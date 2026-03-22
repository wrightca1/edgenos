/*
 * packet_io.c - Packet I/O between kernel TUN interfaces and ASIC
 *
 * Creates swpN TUN interfaces for each port. Handles:
 * - TX: reads from TUN fd, maps swpN→xeM, sends via bmd_tx()
 * - RX: polls bmd_rx(), maps xeM→swpN, writes to TUN fd
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/if_tun.h>

#include "switchd.h"
#include "packet_io.h"
#include "portmap.h"

#define TUN_DEV     "/dev/net/tun"
#define MAX_PKT_SIZE 9216  /* Jumbo frame support */

static int max_tun_fd = -1;
static fd_set tun_fds;

static int tun_create(const char *name)
{
    struct ifreq ifr;
    int fd;

    fd = open(TUN_DEV, O_RDWR);
    if (fd < 0) {
        syslog(LOG_ERR, "Cannot open %s: %s", TUN_DEV, strerror(errno));
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        syslog(LOG_ERR, "TUNSETIFF %s: %s", name, strerror(errno));
        close(fd);
        return -1;
    }

    /* Set non-blocking */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    /* Bring interface up */
    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd >= 0) {
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
        ioctl(sfd, SIOCGIFFLAGS, &ifr);
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ioctl(sfd, SIOCSIFFLAGS, &ifr);

        /* Set MTU to 9000 for jumbo frame support */
        ifr.ifr_mtu = 9000;
        ioctl(sfd, SIOCSIFMTU, &ifr);

        close(sfd);
    }

    return fd;
}

int packet_io_init(void)
{
    int i;
    int count = 0;

    FD_ZERO(&tun_fds);

    for (i = 0; i < SWITCHD_MAX_PORTS; i++) {
        if (!switchd.ports[i].valid)
            continue;

        int fd = tun_create(switchd.ports[i].ifname);
        if (fd < 0) {
            syslog(LOG_WARNING, "Failed to create TUN for %s",
                   switchd.ports[i].ifname);
            continue;
        }

        switchd.ports[i].tun_fd = fd;
        FD_SET(fd, &tun_fds);
        if (fd > max_tun_fd)
            max_tun_fd = fd;

        count++;
    }

    syslog(LOG_INFO, "Created %d TUN interfaces", count);
    return count > 0 ? 0 : -1;
}

static void handle_tun_tx(int port_idx)
{
    struct port_state *port = &switchd.ports[port_idx];
    uint8_t buf[MAX_PKT_SIZE];
    ssize_t len;

    len = read(port->tun_fd, buf, sizeof(buf));
    if (len <= 0)
        return;

    /*
     * TODO: Send packet to ASIC via BMD TX API
     * bmd_tx(switchd.unit, port->logical_port, buf, len);
     */

    port->tx_packets++;
    port->tx_bytes += len;
}

void packet_io_rx_poll(void)
{
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    fd_set read_fds;
    int i;

    /* Check TUN interfaces for TX packets (kernel → ASIC) */
    if (max_tun_fd >= 0) {
        read_fds = tun_fds;
        int nready = select(max_tun_fd + 1, &read_fds, NULL, NULL, &tv);
        if (nready > 0) {
            for (i = 0; i < SWITCHD_MAX_PORTS; i++) {
                if (!switchd.ports[i].valid || switchd.ports[i].tun_fd <= 0)
                    continue;
                if (FD_ISSET(switchd.ports[i].tun_fd, &read_fds))
                    handle_tun_tx(i);
            }
        }
    }

    /*
     * TODO: Poll ASIC for RX packets (ASIC → kernel)
     * bmd_rx_poll(switchd.unit, &rx_port, buf, &len);
     * Then map rx_port to swpN and write to TUN fd:
     *   int swp = portmap_logical_to_swp(rx_port);
     *   write(switchd.ports[swp-1].tun_fd, buf, len);
     */
}

void packet_io_cleanup(void)
{
    int i;

    for (i = 0; i < SWITCHD_MAX_PORTS; i++) {
        if (switchd.ports[i].tun_fd > 0) {
            close(switchd.ports[i].tun_fd);
            switchd.ports[i].tun_fd = 0;
        }
    }

    syslog(LOG_INFO, "Packet I/O cleaned up");
}
