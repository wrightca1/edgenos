/*
 * packet_io.c - Packet I/O between kernel TUN interfaces and ASIC
 *
 * Creates swpN TUN interfaces for each port. Handles:
 * - TX: reads from TUN fd, maps swpN->xeM, sends via bmd_tx()
 * - RX: polls bmd_rx_poll(), maps xeM->swpN, writes to TUN fd
 *
 * Packet path (from Cumulus RE captures):
 *   TX: kernel -> TUN read -> bmd_tx() -> DMA -> ASIC -> wire
 *   RX: wire -> ASIC -> DMA -> bmd_rx_poll() -> TUN write -> kernel
 *
 * DMA memory:
 *   Allocated via bmd_dma_alloc_coherent() which returns both a
 *   virtual address (for CPU access) and a bus/physical address
 *   (for ASIC DMA). On AS5610-52X, the DMA pool is at phys
 *   0x04000000 (32MB), mapped by the BDE kernel module.
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

/* BMD headers */
#include <bmd/bmd.h>
#include <bmd/bmd_dma.h>

#define TUN_DEV     "/dev/net/tun"
#define MAX_PKT_SIZE 9216  /* Jumbo frame support */

/*
 * Number of RX buffers to keep in the DMA ring.
 * Cumulus uses 64 RX DCBs with 2044-byte buffers.
 * We use a smaller count for simplicity.
 */
#define NUM_RX_BUFS  16
#define RX_BUF_SIZE  2048

static int max_tun_fd = -1;
static fd_set tun_fds;

/* RX DMA buffers submitted to the ASIC */
static bmd_pkt_t rx_pkts[NUM_RX_BUFS];
static int rx_initialized;

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

/*
 * Submit RX buffers to the ASIC DMA engine.
 *
 * Each buffer is allocated from DMA-coherent memory and submitted
 * via bmd_rx_start(). The ASIC will DMA received packets into these
 * buffers. We poll for completed buffers via bmd_rx_poll().
 */
static int rx_dma_init(void)
{
    int i;
    int submitted = 0;

    memset(rx_pkts, 0, sizeof(rx_pkts));

    for (i = 0; i < NUM_RX_BUFS; i++) {
        dma_addr_t baddr;

        rx_pkts[i].data = bmd_dma_alloc_coherent(switchd.unit,
                                                  RX_BUF_SIZE, &baddr);
        if (!rx_pkts[i].data) {
            syslog(LOG_ERR, "Failed to allocate RX DMA buffer %d", i);
            break;
        }

        rx_pkts[i].size = RX_BUF_SIZE;
        rx_pkts[i].baddr = baddr;
        rx_pkts[i].port = -1;  /* Will be filled by ASIC on RX */

        int rv = bmd_rx_start(switchd.unit, &rx_pkts[i]);
        if (rv < 0) {
            syslog(LOG_ERR, "bmd_rx_start failed for buffer %d: %d", i, rv);
            bmd_dma_free_coherent(switchd.unit, RX_BUF_SIZE,
                                  rx_pkts[i].data, baddr);
            rx_pkts[i].data = NULL;
            break;
        }

        submitted++;
    }

    syslog(LOG_INFO, "Submitted %d RX DMA buffers (%d bytes each)",
           submitted, RX_BUF_SIZE);
    rx_initialized = (submitted > 0);
    return submitted > 0 ? 0 : -1;
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

    /* Initialize RX DMA buffers */
    if (rx_dma_init() < 0) {
        syslog(LOG_WARNING, "RX DMA init failed - RX path disabled");
    }

    return count > 0 ? 0 : -1;
}

/*
 * TX path: kernel -> ASIC
 *
 * Read a packet from the TUN fd and send it to the ASIC via bmd_tx().
 * The BMD TX path (from OpenMDK bcm56840_a0_bmd_tx.c):
 *   1. Allocate TX DCB from DMA pool
 *   2. Set up module header (SOBMH) with destination port
 *   3. Set DCB address to packet buffer physical address
 *   4. Start DMA via bmd_xgs_dma_tx_start()
 *   5. Poll for completion via bmd_xgs_dma_tx_poll()
 *   6. Free DCB
 */
static void handle_tun_tx(int port_idx)
{
    struct port_state *port = &switchd.ports[port_idx];
    static uint8_t tx_buf[MAX_PKT_SIZE] __attribute__((aligned(64)));
    ssize_t len;
    bmd_pkt_t pkt;
    dma_addr_t baddr;
    void *dma_buf;
    int rv;

    len = read(port->tun_fd, tx_buf, sizeof(tx_buf));
    if (len <= 0)
        return;

    /* Allocate DMA-coherent buffer for the packet data */
    dma_buf = bmd_dma_alloc_coherent(switchd.unit, len, &baddr);
    if (!dma_buf) {
        syslog(LOG_DEBUG, "TX: DMA alloc failed for %s (%zd bytes)",
               port->ifname, len);
        return;
    }

    /* Copy packet data into DMA buffer */
    memcpy(dma_buf, tx_buf, len);

    /* Set up BMD packet structure */
    memset(&pkt, 0, sizeof(pkt));
    pkt.port = port->logical_port;
    pkt.data = dma_buf;
    pkt.size = len;
    pkt.baddr = baddr;
    pkt.flags = BMD_PKT_F_UNTAGGED;

    /* Send to ASIC */
    rv = bmd_tx(switchd.unit, &pkt);
    if (rv < 0) {
        syslog(LOG_DEBUG, "TX: bmd_tx failed on %s: %d", port->ifname, rv);
    } else {
        port->tx_packets++;
        port->tx_bytes += len;
    }

    /* Free DMA buffer */
    bmd_dma_free_coherent(switchd.unit, len, dma_buf, baddr);
}

/*
 * RX path: ASIC -> kernel
 *
 * Poll for completed RX DMA descriptors. When a packet arrives:
 *   1. bmd_rx_poll() returns the completed bmd_pkt_t
 *   2. pkt->port contains the ingress physical port
 *   3. Map physical port to swpN via portmap
 *   4. Write packet data to the corresponding TUN fd
 *   5. Re-submit the RX buffer to the DMA ring
 *
 * From Cumulus RE (CAPTURE_RESULTS §9):
 *   Thread 10792 handles RX: write(tun_fd, packet, len)
 *   Thread 10793 handles TX: read(tun_fd, buf, 16384)
 *   TUN fd mapping: fd = 19 + swp_number
 */
static void handle_asic_rx(void)
{
    bmd_pkt_t *pkt = NULL;
    int rv;

    if (!rx_initialized)
        return;

    /* Poll for a completed RX packet */
    rv = bmd_rx_poll(switchd.unit, &pkt);
    if (rv < 0 || !pkt)
        return;

    /* Map ASIC ingress port to swpN */
    int swp = portmap_logical_to_swp(pkt->port);
    if (swp < 1 || swp > SWITCHD_MAX_PORTS) {
        syslog(LOG_DEBUG, "RX: unknown ingress port %d", pkt->port);
        goto resubmit;
    }

    int port_idx = swp - 1;
    struct port_state *port = &switchd.ports[port_idx];

    if (port->tun_fd <= 0) {
        syslog(LOG_DEBUG, "RX: no TUN fd for %s", port->ifname);
        goto resubmit;
    }

    /* Write packet to TUN fd (delivers to kernel network stack) */
    ssize_t written = write(port->tun_fd, pkt->data, pkt->size);
    if (written > 0) {
        port->rx_packets++;
        port->rx_bytes += written;
    }

resubmit:
    /* Re-submit buffer to RX DMA ring */
    pkt->port = -1;
    pkt->size = RX_BUF_SIZE;
    rv = bmd_rx_start(switchd.unit, pkt);
    if (rv < 0) {
        syslog(LOG_WARNING, "RX: failed to resubmit buffer: %d", rv);
    }
}

void packet_io_rx_poll(void)
{
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    fd_set read_fds;
    int i;

    /* Check TUN interfaces for TX packets (kernel -> ASIC) */
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

    /* Poll ASIC for RX packets (ASIC -> kernel) */
    handle_asic_rx();
}

void packet_io_cleanup(void)
{
    int i;

    /* Stop RX DMA */
    if (rx_initialized) {
        bmd_rx_stop(switchd.unit);
        rx_initialized = 0;
    }

    /* Free RX DMA buffers */
    for (i = 0; i < NUM_RX_BUFS; i++) {
        if (rx_pkts[i].data) {
            bmd_dma_free_coherent(switchd.unit, RX_BUF_SIZE,
                                  rx_pkts[i].data, rx_pkts[i].baddr);
            rx_pkts[i].data = NULL;
        }
    }

    /* Close TUN fds */
    for (i = 0; i < SWITCHD_MAX_PORTS; i++) {
        if (switchd.ports[i].tun_fd > 0) {
            close(switchd.ports[i].tun_fd);
            switchd.ports[i].tun_fd = 0;
        }
    }

    syslog(LOG_INFO, "Packet I/O cleaned up");
}
