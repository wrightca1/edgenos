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
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <linux/if_tun.h>

#include "switchd.h"
#include "packet_io.h"
#include "portmap.h"

/* BMD/CDK headers */
#include <bmd/bmd.h>
#include <bmd/bmd_dma.h>
#include <cdk/chip/bcm56840_a0_defs.h>

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
/*
 * OpenMDK RX DMA is single-buffer: one bmd_rx_start() at a time.
 * After bmd_rx_poll() returns a packet, we must resubmit via
 * bmd_rx_start() before the next packet can be received.
 */
static bmd_pkt_t rx_pkt;
static dma_addr_t rx_baddr;

static int rx_dma_init(void)
{
    int rv;

    memset(&rx_pkt, 0, sizeof(rx_pkt));

    rx_pkt.data = bmd_dma_alloc_coherent(switchd.unit, RX_BUF_SIZE, &rx_baddr);
    if (!rx_pkt.data) {
        syslog(LOG_ERR, "Failed to allocate RX DMA buffer");
        return -1;
    }

    rx_pkt.size = RX_BUF_SIZE;
    rx_pkt.baddr = rx_baddr;
    rx_pkt.port = -1;

    rv = bmd_rx_start(switchd.unit, &rx_pkt);
    if (rv < 0) {
        syslog(LOG_ERR, "bmd_rx_start failed: %d", rv);
        bmd_dma_free_coherent(switchd.unit, RX_BUF_SIZE, rx_pkt.data, rx_baddr);
        rx_pkt.data = NULL;
        return -1;
    }

    syslog(LOG_INFO, "RX DMA initialized (single buffer, %d bytes)", RX_BUF_SIZE);
    rx_initialized = 1;
    return 0;
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

    /* Add each TUN's MAC to L2 table so unicast frames to our
     * interfaces get forwarded to CPU instead of flooded/dropped. */
    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            for (i = 0; i < SWITCHD_MAX_PORTS; i++) {
                if (!switchd.ports[i].valid || switchd.ports[i].tun_fd <= 0)
                    continue;
                struct ifreq ifr;
                memset(&ifr, 0, sizeof(ifr));
                strncpy(ifr.ifr_name, switchd.ports[i].ifname, IFNAMSIZ - 1);
                if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
                    bmd_mac_addr_t mac;
                    memcpy(mac.b, ifr.ifr_hwaddr.sa_data, 6);
                    /*
                     * Program MY_STATION_TCAM so ASIC triggers L3 processing
                     * for packets to our MAC. With V4L3DSTMISS_TOCPU, these
                     * get punted to CPU. VLAN mask=0 matches all VLANs.
                     */
                    {
                        static int tcam_idx = 0;
                        static MY_STATION_TCAMm_t mst;
                        uint32_t mac_field[2];

                        MY_STATION_TCAMm_CLR(mst);

                        /* Set MAC address (key) */
                        mac_field[0] = ((uint32_t)mac.b[2] << 24) | ((uint32_t)mac.b[3] << 16) |
                                       ((uint32_t)mac.b[4] << 8) | mac.b[5];
                        mac_field[1] = ((uint32_t)mac.b[0] << 8) | mac.b[1];
                        MY_STATION_TCAMm_MAC_ADDRf_SET(mst, mac_field);

                        /* Set MAC mask (all 1s = exact match) */
                        mac_field[0] = 0xFFFFFFFF;
                        mac_field[1] = 0xFFFF;
                        MY_STATION_TCAMm_MAC_ADDR_MASKf_SET(mst, mac_field);

                        /* VLAN mask=0 (match any VLAN) */
                        MY_STATION_TCAMm_VLAN_ID_MASKf_SET(mst, 0);

                        /* Enable IPv4 and IPv6 termination */
                        MY_STATION_TCAMm_IPV4_TERMINATION_ALLOWEDf_SET(mst, 1);
                        MY_STATION_TCAMm_IPV6_TERMINATION_ALLOWEDf_SET(mst, 1);

                        /* Valid */
                        MY_STATION_TCAMm_VALIDf_SET(mst, 1);

                        int rv2 = WRITE_MY_STATION_TCAMm(switchd.unit, tcam_idx, mst);
                        if (rv2 == 0)
                            tcam_idx++;
                    }
                    int rv = 0;
                    syslog(LOG_INFO, "L2: %s MAC %02x:%02x:%02x:%02x:%02x:%02x -> CPU (rv=%d)",
                           switchd.ports[i].ifname,
                           mac.b[0], mac.b[1], mac.b[2],
                           mac.b[3], mac.b[4], mac.b[5], rv);
                }
            }
            close(sock);
        }
    }

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

    /* Pad to 64 bytes minimum. The MAC adds 4-byte FCS for 68 on wire.
     * Extra 4 bytes beyond the standard 60-byte minimum compensates for
     * the ASIC stripping a VLAN tag on untagged egress ports. */
    if (len < 64) {
        memset(tx_buf + len, 0, 64 - len);
        len = 64;
    }

    /* Allocate DMA-coherent buffer for the (padded) packet data */
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
    /*
     * pkt.port must be the CDK physical port number (not front-panel).
     * BMD_PORT_VALID checks BMD_PORT_PROPERTIES which is indexed by
     * CDK port. On BCM56840, physical_lane IS the CDK port number.
     */
    pkt.port = port->physical_lane;
    pkt.data = dma_buf;
    pkt.size = len;
    pkt.baddr = baddr;
    pkt.flags = 0;  /* Don't set UNTAGGED — avoids hdr_size/hdr_offset mismatch in DCB scatter-gather */

    /* Debug: dump first 32 bytes of packet */
    if (port->tx_packets < 3) {
        char hex[97];
        int i, n = len < 32 ? len : 32;
        for (i = 0; i < n; i++)
            sprintf(hex + i*3, "%02x ", ((unsigned char*)dma_buf)[i]);
        hex[n*3] = '\0';
        syslog(LOG_INFO, "TX %s: port=%d len=%d baddr=0x%08x: %s",
               port->ifname, pkt.port, len, (unsigned)baddr, hex);
    }

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

    /* Debug: dump first RX packets */
    {
        static int rx_dbg_count = 0;
        if (rx_dbg_count < 20) {
            char hex[97];
            int i, n = pkt->size < 32 ? pkt->size : 32;
            for (i = 0; i < n; i++)
                sprintf(hex + i*3, "%02x ", ((unsigned char*)pkt->data)[i]);
            hex[n*3] = '\0';
            syslog(LOG_INFO, "RX: port=%d size=%d baddr=0x%08x: %s",
                   pkt->port, pkt->size, (unsigned)pkt->baddr, hex);
            rx_dbg_count++;
        }
    }

    /* Map ASIC ingress port (CDK physical port) to swpN */
    int swp = portmap_phys_to_swp(pkt->port);
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

    /* Strip 802.1Q VLAN tag if present (ASIC adds it for CPU-bound frames).
     * VLAN tag is 4 bytes at offset 12: [81 00] [TCI].
     * Remove it so the kernel sees a clean untagged Ethernet frame. */
    uint8_t *frame = pkt->data;
    int frame_len = pkt->size;
    if (frame_len >= 18 && frame[12] == 0x81 && frame[13] == 0x00) {
        memmove(frame + 12, frame + 16, frame_len - 16);
        frame_len -= 4;
    }

    /* Write packet to TUN fd (delivers to kernel network stack) */
    ssize_t written = write(port->tun_fd, frame, frame_len);
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
