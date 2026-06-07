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

#include "edged.h"
#include "packet_io.h"
#include "portmap.h"
#include "vlan.h"
#include "l3.h"

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

/* TX-path counters (2026-05-31 datapath debug), summarized in edged.c stat poll. */
unsigned g_tx_calls = 0, g_tx_ok = 0, g_tx_fail = 0, g_tx_lastrv = 0;
static fd_set tun_fds;

/* RX DMA buffers submitted to the ASIC */
static bmd_pkt_t rx_pkts[NUM_RX_BUFS];
static int rx_initialized;

/*
 * Read eth0's MAC into base. Cumulus assigns swpN MACs as base+N so
 * the addresses are stable across reboots. Falls back to a captured
 * default if eth0 is unavailable (see l2.c read_mgmt_mac).
 */
static int read_base_mac(uint8_t base[6])
{
    FILE *f;
    unsigned int m[6];

    f = fopen("/sys/class/net/eth0/address", "r");
    if (f) {
        if (fscanf(f, "%x:%x:%x:%x:%x:%x",
                   &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
            int i;
            for (i = 0; i < 6; i++) base[i] = (uint8_t)m[i];
            fclose(f);
            return 0;
        }
        fclose(f);
    }
    /* Captured Cumulus base */
    base[0] = 0x80; base[1] = 0xa2; base[2] = 0x35;
    base[3] = 0x81; base[4] = 0xca; base[5] = 0xae;
    return 0;
}

static int tun_create(const char *name, int port_num)
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

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd >= 0) {
        /*
         * Assign stable per-port MAC: base + port_num (Cumulus scheme).
         * port_num is the front-panel 1..52. eth0 stays at base+0.
         * Only the low byte of base is bumped; if it overflows we wrap
         * into byte 4 — matches what `swpd` does on real Cumulus.
         */
        uint8_t base[6];
        if (port_num >= 1 && port_num <= 52 && read_base_mac(base) == 0) {
            unsigned int low = ((unsigned int)base[5]) + (unsigned int)port_num;
            base[5] = low & 0xff;
            base[4] = (uint8_t)(base[4] + (low >> 8));

            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
            ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
            memcpy(ifr.ifr_hwaddr.sa_data, base, 6);
            if (ioctl(sfd, SIOCSIFHWADDR, &ifr) < 0) {
                syslog(LOG_WARNING,
                       "%s: SIOCSIFHWADDR failed: %s", name, strerror(errno));
            }
        }

        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
        ioctl(sfd, SIOCGIFFLAGS, &ifr);
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ioctl(sfd, SIOCSIFFLAGS, &ifr);

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
 * RX DMA — multi-DCB ring model (matches Cumulus's CMICm setup).
 *
 * `bmd_rx_start` now allocates a 64-DCB ring internally (in
 * bcm56840_a0_bmd_rx.c) on its first call; subsequent calls are no-ops.
 * The chip walks the ring autonomously via CONTINUOUS_DMA + DESC_HALT_ADDR
 * and refills each DCB via RELOAD=1.  `bmd_rx_poll` returns a packet whose
 * lifetime is tied to the ring (don't free it — chip will reuse the slot).
 */
static int rx_dma_init(void)
{
    int rv;

    /* bmd_rx_start ignores its pkt arg now — it manages its own ring.
     * Pass NULL for clarity. */
    rv = bmd_rx_start(edged.unit, NULL);
    if (rv < 0) {
        syslog(LOG_ERR, "bmd_rx_start (ring init) failed: %d", rv);
        return -1;
    }

    syslog(LOG_INFO, "RX DMA initialized (64-DCB ring, CMICm CONTINUOUS_DMA)");
    rx_initialized = 1;
    return 0;
}

int packet_io_init(void)
{
    int i;
    int count = 0;

    FD_ZERO(&tun_fds);

    for (i = 0; i < EDGED_MAX_PORTS; i++) {
        if (!edged.ports[i].valid)
            continue;

        int fd = tun_create(edged.ports[i].ifname,
                            edged.ports[i].logical_port);
        if (fd < 0) {
            syslog(LOG_WARNING, "Failed to create TUN for %s",
                   edged.ports[i].ifname);
            continue;
        }

        edged.ports[i].tun_fd = fd;
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
            for (i = 0; i < EDGED_MAX_PORTS; i++) {
                if (!edged.ports[i].valid || edged.ports[i].tun_fd <= 0)
                    continue;
                struct ifreq ifr;
                memset(&ifr, 0, sizeof(ifr));
                strncpy(ifr.ifr_name, edged.ports[i].ifname, IFNAMSIZ - 1);
                if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
                    bmd_mac_addr_t mac;
                    memcpy(mac.b, ifr.ifr_hwaddr.sa_data, 6);
                    /* Use bmd_port_mac_addr_add on CPU port (0) — this writes
                     * to L2_ENTRY with STATIC bit, which has priority over
                     * hardware-learned dynamic entries. */
                    /* Add the swpN MAC to its own service VID's L2 table,
                     * pointing to CPU port.  This ensures the chip's L2
                     * lookup for unicast frames (Nexus's IPv4 replies)
                     * addressed to this swpN MAC finds CPU port directly,
                     * not just via the flood-to-VLAN-members fallback. */
                    int svid = edged_resv_vid_for_port(
                        edged.ports[i].logical_port);
                    int rv = bmd_port_mac_addr_add(edged.unit, 0, svid, &mac);
                    syslog(LOG_INFO,
                           "L2: %s MAC %02x:%02x:%02x:%02x:%02x:%02x -> CPU "
                           "VID=%d (rv=%d)",
                           edged.ports[i].ifname,
                           mac.b[0], mac.b[1], mac.b[2],
                           mac.b[3], mac.b[4], mac.b[5], svid, rv);
                    /* MY_STATION_TCAM is REQUIRED so chip's L3 pipeline
                     * recognises this MAC as a router endpoint. */
                    l3_my_station_add(mac.b, svid);

                    /* l3_local_host_add for the swpN's IPv4 (so the
                     * chip can deliver received-IPv4-for-our-IP to
                     * CPU instead of dropping) is handled by the
                     * netlink RTM_NEWADDR handler.  Don't duplicate
                     * here. */
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
    struct port_state *port = &edged.ports[port_idx];
    static uint8_t tx_buf[MAX_PKT_SIZE] __attribute__((aligned(64)));
    ssize_t len;
    bmd_pkt_t pkt;
    dma_addr_t baddr;
    void *dma_buf;
    int rv;

    len = read(port->tun_fd, tx_buf, sizeof(tx_buf));
    if (len <= 0)
        return;

    g_tx_calls++;

    /*
     * TX path selection (2026-06-02):
     *
     * DIRECTED injection (link-up ports) — the real path.  We hand the
     * raw frame to bcm56840_a0_bmd_tx with pkt.port = physical_lane; the
     * SOBMH module header (sob[2]=P2L(port)) directs it straight out that
     * physical port, UNTAGGED, bypassing the ingress L2 lookup entirely.
     * This is the correct L2 model (each swpN TUN frame egresses swpN) and
     * it is the path proven to put frames on the wire (swp47 tx_pkts>0).
     * No service-VID tag is needed or wanted here — a tag would make the
     * far end (e.g. swp48 in the loopback) classify into the wrong VID and
     * drop the frame.
     *
     * FLOOD fallback (link-down ports) — keep the Cumulus service-VID
     * scheme (prepend 3300+logical_port, pkt.port=-1) so behaviour is
     * unchanged for ports we can't direct to yet.  These can't egress
     * anyway (link down); the tag keeps the old code path intact.
     *
     * NOTE the service VID is still used for the RX direction (swpN frame
     * -> PVID 33xx -> flood to CPU); only TX changes here.
     */
    int directed = port->link_up;
    if (!directed && len >= 14 && (size_t)(len + 4) <= sizeof(tx_buf)) {
        /* Insert 4 bytes at offset 12 (after dst+src MAC):
         *   [dst 6][src 6][etype 2][payload]
         * -> [dst 6][src 6][0x8100][TCI 2][etype 2][payload]  */
        int vid = edged_resv_vid_for_port(port->logical_port);
        memmove(tx_buf + 16, tx_buf + 12, len - 12);
        tx_buf[12] = 0x81;
        tx_buf[13] = 0x00;
        tx_buf[14] = (vid >> 8) & 0x0f;   /* PCP=0 DEI=0, VID hi nibble */
        tx_buf[15] = vid & 0xff;          /* VID low byte */
        len += 4;
    }

    /* Pad to 64 bytes minimum on the wire. */
    if (len < 64) {
        memset(tx_buf + len, 0, 64 - len);
        len = 64;
    }

    /* FCS slack (2026-06-04): the chip's egress MAC is in CRC-REPLACE mode — it
     * overwrites the LAST 4 BYTES of the frame with the computed FCS rather than
     * appending.  Without slack, every frame loses its last 4 real data bytes:
     * harmless for padded frames (lost bytes are pad) but for an exact-length
     * frame (IP total-length == L2 payload) it truncates 4 bytes of IP data, so
     * the peer receives a clean-FCS frame whose IP packet is 4 bytes short and
     * silently drops it (observed: e1/33 full byte count, 0 CRC, no ICMP reply;
     * pings worked only for payloads that left >=4 pad bytes).  Append 4 dummy
     * bytes so the MAC overwrites those instead of real data.  (Safe under
     * CRC-append mode too: the 4 extra bytes become harmless trailing L2 pad.) */
    memset(tx_buf + len, 0, 4);
    len += 4;

    /* Allocate DMA-coherent buffer for the (padded) packet data */
    dma_buf = bmd_dma_alloc_coherent(edged.unit, len, &baddr);
    if (!dma_buf) {
        syslog(LOG_DEBUG, "TX: DMA alloc failed for %s (%zd bytes)",
               port->ifname, len);
        return;
    }

    /* Copy packet data into DMA buffer */
    memcpy(dma_buf, tx_buf, len);

    /* Set up BMD packet structure.
     *
     * pkt.port = -1 tells bcm56840_a0_bmd_tx to skip the HiGig SOB
     * module header and submit the frame as a single non-chained DCB.
     * The chip's L2 lookup uses the 802.1Q tag we just prepended to
     * pick the egress port (service VID has exactly one untagged
     * member), strips the tag on egress, and puts a clean Ethernet
     * frame on the wire — same shape Cumulus 2.5 produced. */
    memset(&pkt, 0, sizeof(pkt));
    pkt.port = directed ? port->physical_lane : -1;
    pkt.data = dma_buf;
    pkt.size = len;
    pkt.baddr = baddr;
    pkt.flags = 0;

    /* Send to ASIC */
    rv = bmd_tx(edged.unit, &pkt);
    g_tx_lastrv = (unsigned)rv;
    if (rv < 0) {
        g_tx_fail++;
        syslog(LOG_DEBUG, "TX: bmd_tx failed on %s: %d", port->ifname, rv);
    } else {
        g_tx_ok++;
        port->tx_packets++;
        port->tx_bytes += len;
    }

    /* Free DMA buffer */
    bmd_dma_free_coherent(edged.unit, len, dma_buf, baddr);
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
    rv = bmd_rx_poll(edged.unit, &pkt);

    if (rv < 0 || !pkt)
        return;

    /* Map ASIC ingress port (CDK physical port) to swpN */
    int swp = portmap_phys_to_swp(pkt->port);
    if (swp < 1 || swp > EDGED_MAX_PORTS) {
        syslog(LOG_DEBUG, "RX: unknown ingress port %d", pkt->port);
        goto resubmit;
    }

    int port_idx = swp - 1;
    struct port_state *port = &edged.ports[port_idx];

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
    /* With the multi-DCB ring (CONTINUOUS_DMA + RELOAD=1), the chip auto-
     * reuses each DCB after we consume it.  bmd_rx_poll already reset the
     * just-consumed DCB's DONE bit and advanced the read pointer.  Nothing
     * for us to do here. */
    (void)rv;
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
            for (i = 0; i < EDGED_MAX_PORTS; i++) {
                if (!edged.ports[i].valid || edged.ports[i].tun_fd <= 0)
                    continue;
                if (FD_ISSET(edged.ports[i].tun_fd, &read_fds))
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

    /* Stop RX DMA — bmd_rx_stop frees the entire 64-DCB ring + its buffers
     * (managed internally by bcm56840_a0_bmd_rx.c, no per-slot cleanup here). */
    if (rx_initialized) {
        bmd_rx_stop(edged.unit);
        rx_initialized = 0;
    }

    /* Close TUN fds */
    for (i = 0; i < EDGED_MAX_PORTS; i++) {
        if (edged.ports[i].tun_fd > 0) {
            close(edged.ports[i].tun_fd);
            edged.ports[i].tun_fd = 0;
        }
    }

    syslog(LOG_INFO, "Packet I/O cleaned up");
}
