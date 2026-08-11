/*
 * fm6000_portd.c - expose FM6000 front-panel ports as Linux TAP interfaces.
 *
 * This is the piece that lets a normal control plane run on this switch. The
 * ASIC ports are not Linux netdevs, so nothing -- ARP, ping, OSPF, BGP -- can
 * use them. portd bridges each ASIC port to a TAP device through the existing
 * CPU punt/inject path:
 *
 *     ASIC RX (punt)  --strip F64 tag-->  write() to TAP  --> kernel stack
 *     kernel stack    --> read() from TAP --insert F64 tag--> fpdma_tx (inject)
 *
 * Once "et1" exists you can `ip addr add ... dev et1`, ping through it, and run
 * a routing daemon on it. Routes the daemon installs then get pushed into the
 * hardware FIB by fm6000_route / a FIB sync agent -- see docs/ROUTING-FIB.md.
 *
 * The F64 tag goes INLINE in the frame at offset 12 (after SMAC), and word 1 is
 * the egress GLORT, which selects the physical port. The GLORT<->port mapping is
 * not stable across configurations -- see docs/GLORT-MAPPING.md.
 *
 * RX framing is unreliable: a frame may sit at offset 0, be preceded by a
 * receive prefix, and may or may not still carry the tag. We locate the real
 * frame by scanning for a plausible ethertype, exactly as fm6000_l3 does.
 *
 * usage: fm6000_portd <ifname> <glort-hex> [mac] [seconds]
 *   e.g. fm6000_portd et1 03ef 44:4c:a8:31:5d:ab 0     (0 = run forever)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <net/if_arp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>

#include "fpdma.h"
#include "fpdma_kmod.h"
#include "fm6000_regs.h"

#define MAX_FRAME 1600
#define F64_LEN   8
/* The ASIC appends the 4-byte Ethernet FCS to punted frames. The kernel expects
 * frames WITHOUT it: leaving it on makes every packet 4 bytes too long, which
 * ospfd parses as an extra (garbage) neighbour entry in a Hello. */
#define FCS_LEN   4

static struct fpdma     fp;
static struct fm6000_dev dev;
static int      tapfd = -1;
static uint16_t TAG[4] = { 0x0100, 0x03ef, 0xff00, 0x0000 };
static volatile sig_atomic_t stop_flag;
static unsigned long n_rx, n_tx, n_rx_drop;
static int dbg;                      /* PORTD_DEBUG=N -> hexdump first N frames */
static int tx_fcs;                   /* PORTD_TXFCS=1 -> append an FCS placeholder */

static void hexdump(const char *tag, const uint8_t *p, int n)
{
    int i, m = n > 64 ? 64 : n;
    printf("  %s len=%d:", tag, n);
    for (i = 0; i < m; i++) printf("%s%02x", (i % 16) ? " " : "\n    ", p[i]);
    printf("\n"); fflush(stdout);
}

static void on_signal(int sig) { (void)sig; stop_flag = 1; }

/* Create a TAP device and bring it up. Returns its fd. */
static int tap_open(const char *name)
{
    struct ifreq ifr;
    int fd = open("/dev/net/tun", O_RDWR);

    if (fd < 0) { perror("open /dev/net/tun"); return -1; }
    memset(&ifr, 0, sizeof ifr);
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) { perror("TUNSETIFF"); close(fd); return -1; }
    return fd;
}

/* ⚠⚠ THE MAC ARGUMENT USED TO BE ACCEPTED AND SILENTLY IGNORED, AND THAT COST A
 * DAY. usage advertised "<ifname> <glort-hex> [mac] [seconds]", argv[3] was never
 * read, and tap_open left the TAP with the random MAC the kernel assigns. Frames
 * then egressed with a source MAC that changes every boot, while the ASIC's punt
 * tables are programmed for the real one -- so the peer answered into a black
 * hole, ARP hung INCOMPLETE, and the box looked like a forwarding-plane failure.
 * It was diagnosed only by dumping the injected bytes (PORTD_DEBUG=4) and seeing
 * SMAC 0a:2f:38:c1:32:6f where 44:4c:a8:31:5d:ab belonged.
 *
 * Setting it here makes the documented argument real. Passing "x" (or anything
 * without a colon) keeps the kernel's random MAC, which is what the old callers
 * did in practice. */
static int tap_set_mac(const char *name, const char *mac)
{
    struct ifreq ifr;
    unsigned m[6];
    int s, i;

    if (!mac || !strchr(mac, ':'))
        return 0;                      /* no MAC requested */
    if (sscanf(mac, "%x:%x:%x:%x:%x:%x",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        fprintf(stderr, "portd: cannot parse MAC '%s'\n", mac);
        return -1;
    }
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return -1; }
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
    ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
    for (i = 0; i < 6; i++)
        ifr.ifr_hwaddr.sa_data[i] = (char)m[i];
    if (ioctl(s, SIOCSIFHWADDR, &ifr) < 0) {
        perror("SIOCSIFHWADDR"); close(s); return -1;
    }
    close(s);
    printf("portd: %s MAC set to %02x:%02x:%02x:%02x:%02x:%02x\n",
           name, m[0], m[1], m[2], m[3], m[4], m[5]);
    return 0;
}

/* ASIC -> TAP. The frame may be offset by a receive prefix and may still carry
 * the 8-byte F64 tag at offset 12; strip it so the kernel sees clean Ethernet. */
static void on_punt(void *ctx, const void *data, uint16_t len)
{
    const uint8_t *raw = data;
    uint8_t clean[MAX_FRAME];
    int off;

    (void)ctx;
    if (len < 14 || len > MAX_FRAME) { n_rx_drop++; return; }

    for (off = 0; off + 14 <= (int)len && off < 40; off++) {
        uint16_t et = (raw[off + 12] << 8) | raw[off + 13];
        if (et == 0x0800 || et == 0x0806 || et == 0x86dd || et == 0x8100) {
            int n = (int)len - off - FCS_LEN;            /* drop the trailing FCS */
            if (n < 14) { n_rx_drop++; return; }
            if (n > MAX_FRAME) n = MAX_FRAME;
            if (dbg > 0) { dbg--; hexdump("TAP<-ASIC (no tag)", raw + off, n); }
            if (write(tapfd, raw + off, n) < 0 && errno != EAGAIN) n_rx_drop++;
            else n_rx++;
            return;
        }
        /* tag still present at +12: splice it out */
        if (off + 22 <= (int)len) {
            uint16_t et2 = (raw[off + 20] << 8) | raw[off + 21];
            if (et2 == 0x0800 || et2 == 0x0806 || et2 == 0x86dd || et2 == 0x8100) {
                int body = (int)len - off - 20 - FCS_LEN;  /* ditto */
                if (body < 0 || body + 12 > MAX_FRAME) { n_rx_drop++; return; }
                memcpy(clean, raw + off, 12);
                memcpy(clean + 12, raw + off + 20, body);
                if (dbg > 0) { dbg--;
                    hexdump("ASIC raw", raw + off, (int)len - off);
                    hexdump("TAP<-ASIC (tag spliced)", clean, body + 12); }
                if (write(tapfd, clean, body + 12) < 0 && errno != EAGAIN) n_rx_drop++;
                else n_rx++;
                return;
            }
        }
    }
    n_rx_drop++;                       /* no recognisable ethertype */
}

/* Drain the ASIC RX ring into the TAP.
 * Scan ALL descriptors and pair descriptor i with buf_va[i]; fpdma_rx_poll()
 * walks by ring tail, which races the hardware fill order and hands the callback
 * a length from one descriptor with another's buffer. */
/* Hand a descriptor back to hardware.
 *
 * ⚠ The WHOLE descriptor has to be rewritten, not just the status byte.
 * Hardware overwrites LEN with the RECEIVED length when it fills the slot --
 * that is where rlen is read from below. Recycling with `d[0] = RX_READY`
 * alone leaves LEN set to the last frame's size, so the slot is offered back
 * to hardware as a buffer of that size. It shrinks with every short frame
 * until the ring can no longer accept anything and RX stops dead, silently,
 * with no error counter moving.
 *
 * Measured before the fix: RX delivered 157 packets and then went to zero
 * permanently -- OSPF hellos stopped, the adjacency dropped 35 routes -> 2,
 * ping went to 100%, while TX kept working. It presents as an ASIC/replay
 * defect and is neither; it reproduced identically on the stock replay.
 *
 * Order matters: length and address first, barrier, status last -- the status
 * byte is the handoff, so it must not become visible before the fields it
 * describes. (fpdma.c's desc_write does the same; it is static, hence the
 * open-coded copy here.)
 */
static void rx_recycle(volatile uint8_t *d, unsigned i)
{
    *(volatile uint16_t *)(d + FM6000_DESC_LEN)     = (uint16_t)fp.rx.buf_len;
    *(volatile uint32_t *)(d + FM6000_DESC_ADDR_LO) =
        (uint32_t)(fp.rx.buf_dma[i] & 0xFFFFFFFFu);
    *(volatile uint32_t *)(d + FM6000_DESC_ADDR_HI) =
        (uint32_t)(fp.rx.buf_dma[i] >> 32);
    __sync_synchronize();
    *(volatile uint8_t *)(d + FM6000_DESC_STATUS)   = FM6000_DESC_RX_READY;
}

static void rx_drain(void)
{
    int refilled = 0;

    for (unsigned i = 0; i < fp.rx.size; i++) {
        volatile uint8_t *d = fp.rx.desc + i * FM6000_DESC_STRIDE;
        uint16_t rlen;

        if (!(d[FM6000_DESC_STATUS] & FM6000_DESC_DONE)) continue;
        rlen = *(volatile uint16_t *)(d + FM6000_DESC_LEN);
        if (rlen) on_punt(NULL, (uint8_t *)fp.rx.buf_va[i], rlen);
        rx_recycle(d, i);
        refilled++;
    }
    /* One RX_POST for the batch rather than one per descriptor: this register
     * is shared with the TX path, and the fewer writes to it the better. */
    if (refilled)
        fm6000_dma_write(&dev, FM6000_DMA_COMMAND, FM6000_DMA_CMD_RX_POST);
}

/* TAP -> ASIC. Insert the 8-byte F64 tag inline at offset 12 (after SMAC). */
static void inject(const uint8_t *frame, int len)
{
    uint8_t out[MAX_FRAME + F64_LEN];
    int w, tlen;

    if (len < 14 || len + F64_LEN > (int)sizeof out) return;
    memcpy(out, frame, 12);                                   /* DMAC + SMAC   */
    for (w = 0; w < 4; w++) {                                 /* F64 tag       */
        out[12 + 2 * w]     = TAG[w] >> 8;
        out[12 + 2 * w + 1] = TAG[w] & 0xff;
    }
    memcpy(out + 12 + F64_LEN, frame + 12, len - 12);         /* ethertype ... */
    if (dbg > 0) { dbg--;
        hexdump("TAP->portd (from kernel)", frame, len);
        hexdump("portd->ASIC (tag inserted)", out, len + F64_LEN); }
    tlen = len + F64_LEN;
    /* The ASIC appends a 4-byte FCS to frames it punts to us. Symmetrically it
     * may EXPECT a 4-byte FCS placeholder on inject, which it then recomputes.
     * If so, omitting it makes the DMA consume the last 4 bytes of real payload.
     * PORTD_TXFCS=1 appends the placeholder so we can test that directly. */
    if (tx_fcs) {
        memset(out + tlen, 0, FCS_LEN);
        tlen += FCS_LEN;
    }
    if (tlen < 72) tlen = 72;                                 /* min frame     */

    /* The TX engine will NOT pick work up on its own: fpdma_tx only queues a
     * descriptor. Issuing TX_START on an empty ring leaves the processor Idle
     * and TX_POST does not wake it, so every frame needs
     *     TX_STOP (resets the descriptor index) -> fill READY -> TX_START.
     * This is the sequence fm6000_l3/fm6000_txinline use and it is why bare
     * fpdma_tx() silently sends nothing.
     *
     * Cost: ~10 ms per frame, so this caps out around 100 pps. That is fine for
     * ARP and OSPF hellos -- it is NOT a data path. */
    /* TX must be ATOMIC with respect to the DMA command register: rx_drain()
     * writes RX_POST to the SAME register (FM6000_DMA_COMMAND), and issuing it
     * inside the TX_STOP..TX_START window corrupts the transmit -- frames are
     * queued and completed but never reach the wire. Drain RX before and after,
     * never during. */
    rx_drain();
    fm6000_dma_write(&dev, FM6000_DMA_COMMAND, FM6000_DMA_CMD_TX_STOP);
    usleep(1500);
    fp.tx.head = 0; fp.tx.tail = 0;
    memset((void *)fp.tx.desc, 0, fp.tx.size * FM6000_DESC_STRIDE);
    if (fpdma_tx(&fp, out, (uint16_t)tlen) == 0) n_tx++;
    fm6000_dma_write(&dev, FM6000_DMA_COMMAND, FM6000_DMA_CMD_TX);
    usleep(8000);
    rx_drain();
}

int main(int argc, char **argv)
{
    const char *ifname = argc > 1 ? argv[1] : "et1";
    unsigned secs = argc > 4 ? (unsigned)strtoul(argv[4], 0, 0) : 0;
    struct fpdma_backing back;
    uint8_t buf[MAX_FRAME];
    time_t t0;

    if (argc > 2) TAG[1] = (uint16_t)strtoul(argv[2], 0, 16);
    const char *want_mac = argc > 3 ? argv[3] : NULL;
    { const char *d = getenv("PORTD_DEBUG"); if (d) dbg = atoi(d); }
    { const char *d = getenv("PORTD_TXFCS"); if (d) tx_fcs = atoi(d); }
    printf("portd: tx_fcs=%d\n", tx_fcs);

    struct fpdma_kmod *k = NULL;
    size_t bsz = 0;
    volatile void *bar0;

    if (fpdma_kmod_open(&k) < 0) { fprintf(stderr, "kmod open failed (insmod fm6000dma.ko?)\n"); return 1; }
    bar0 = fpdma_kmod_bar0(k, &bsz);
    fm6000_hw_attach(&dev, bar0, bsz, "0000:02:00.0");
    back = fpdma_kmod_backing(k);
    if (fpdma_init(&fp, &dev, &back, 8, 64) < 0) { fprintf(stderr, "fpdma init failed\n"); return 1; }

    tapfd = tap_open(ifname);
    if (tapfd >= 0 && tap_set_mac(ifname, want_mac) < 0)
        fprintf(stderr, "portd: WARNING continuing with the kernel's random MAC;"
                        " the ASIC punt tables expect a specific one\n");
    if (tapfd < 0) return 1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    printf("portd: %s <-> ASIC, egress GLORT 0x%04x\n", ifname, TAG[1]);
    printf("  bring it up with:  ip link set %s up; ip addr add <a.b.c.d/len> dev %s\n",
           ifname, ifname);
    fflush(stdout);

    t0 = time(NULL);
    while (!stop_flag) {
        struct pollfd pfd = { .fd = tapfd, .events = POLLIN };

        /* kernel -> ASIC */
        if (poll(&pfd, 1, 1) > 0 && (pfd.revents & POLLIN)) {
            int n = read(tapfd, buf, sizeof buf);
            if (n > 0) inject(buf, n);
        }
        rx_drain();                                       /* ASIC -> kernel */
        fpdma_tx_reclaim(&fp);

        if (secs && (unsigned)(time(NULL) - t0) >= secs) break;
    }

    printf("portd: rx=%lu tx=%lu rx_drop=%lu\n", n_rx, n_tx, n_rx_drop);
    fpdma_shutdown(&fp);
    close(tapfd);
    return 0;
}
