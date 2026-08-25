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
 * hardware FIB by fm6000_route / a FIB sync agent -- see docs/EDGENOS-7150.md (was ROUTING-FIB).
 *
 * The F64 tag goes INLINE in the frame at offset 12 (after SMAC), and word 1 is
 * the egress GLORT, which selects the physical port. The GLORT<->port mapping is
 * not stable across configurations -- see docs/EDGENOS-7150.md (was GLORT-MAPPING).
 *
 * RX framing is unreliable: a frame may sit at offset 0, be preceded by a
 * receive prefix, and may or may not still carry the tag. We locate the real
 * frame by scanning for a plausible ethertype, exactly as fm6000_l3 does.
 *
 * usage: fm6000_portd <ifname> <glort-hex> [mac] [seconds]
 *   e.g. fm6000_portd et1 03ef 44:4c:a8:31:5d:ab 0     (0 = run forever)
 *
 * or, for several ports in ONE process:
 *        fm6000_portd <if>:<glort>[:<mac>] ... [-t seconds]
 *   e.g. fm6000_portd et1:03ef:44:4c:a8:31:5d:ab et3:03ed -t 0
 *
 * It has to be one process. Every port shares the single punt/inject DMA ring,
 * so two portd instances would each take descriptors off that ring and each
 * would see only part of the traffic -- indistinguishable from a dataplane
 * fault, and the reason edgenos-up.sh refuses to start a second one.
 *
 * ⚠ RX demux is the unfinished half. Injection is exact -- each port stamps its
 * own egress GLORT in tag w1 -- but deciding which TAP a *punted* frame belongs
 * to needs the source GLORT out of that frame's tag, and which halfword carries
 * it is not confirmed (see src_word). Frames with no tag, or a tag naming no
 * configured port, go to the first port, which is what this program did when it
 * only had one. So multi-port TX is trustworthy today; multi-port RX needs one
 * capture with PORTD_DEBUG=N on a box with two live ports to confirm.
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

#define MAX_PORTS 8

/* One TAP per ASIC port, all in ONE process. They share the single punt/inject
 * DMA ring, so a second portd is not an option: both would pull descriptors off
 * the same ring and each would see a fraction of the traffic, which looks
 * exactly like a dataplane fault. edgenos-up.sh refuses to start a second one. */
struct port {
    char     name[16];
    int      fd;
    uint16_t tag[4];              /* w1 = this port's egress GLORT */
};
static struct port ports[MAX_PORTS];
static int nports;

/* Which halfword of a punted frame's F64 tag holds the SOURCE glort.
 *
 * ⚠ THIS WAS 2, AND 2 IS WRONG. The old comment reasoned that since inject
 * builds w1 = DGLORT and w2 = 0xff00, w2 "reads like the CPU's own SGLORT", so
 * w2 was the natural candidate. That is an argument from the TX layout about
 * the RX layout, and it does not hold.
 *
 * It is WORD 1, and this was decoded and written down before this file guessed:
 * docs/EDGENOS-7150.md (was PORT3-BRINGUP), "The punt frame format, decoded", from an fm6000_rxdump
 * capture with no portd competing --
 *
 *     33 33 00 00 00 05 | 80 a2 35 81 ca b4 | 07 01  03 ef  00 01  ff ff | 86 dd
 *     DMAC                SMAC                F64 tag: 4 x 16-bit at offset 12
 *
 * "Tag word 1 is the GLORT: source on RX, destination on TX. Every punted frame
 * from Et1 carries 0x03ef, which is precisely PARSER_INIT_FIELDS[40] >> 16."
 * The same slot, opposite direction -- which is why w1 is right and w2 is not.
 *
 * The symptom of the wrong value, measured 2026-08-15 with both ports up: every
 * punted frame falls through to port 0, so et1 rx climbs while et2 rx stays 0
 * even though et2 transmits fine and the peer has learned our MAC. The
 * fallback hides it -- a wrong src_word and a tag with no source field at all
 * look identical from the outside. */
static int src_word = 1;
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

/* Pick the TAP a punted frame belongs to, from the source glort in its F64 tag.
 * Falls back to port 0 -- see the src_word note above. */
static int port_for_tag(const uint8_t *tag)
{
    uint16_t g = (uint16_t)((tag[2 * src_word] << 8) | tag[2 * src_word + 1]);
    int i;

    if (dbg > 0)
        fprintf(stderr, "portd: punt tag %04x %04x %04x %04x -> src w%d = %04x\n",
                (tag[0] << 8) | tag[1], (tag[2] << 8) | tag[3],
                (tag[4] << 8) | tag[5], (tag[6] << 8) | tag[7], src_word, g);
    for (i = 0; i < nports; i++)
        if (ports[i].tag[1] == g) return i;
    return 0;
}

/* ASIC -> TAP. The frame may be offset by a receive prefix and may still carry
 * the 8-byte F64 tag at offset 12; strip it so the kernel sees clean Ethernet. */
static void on_punt(void *ctx, const void *data, uint16_t len)
{
    const uint8_t *raw = data;
    uint8_t clean[MAX_FRAME];
    int off;
    int tapfd = ports[0].fd;      /* untagged frames carry no source: port 0 */

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
                tapfd = ports[port_for_tag(raw + off + 12)].fd;
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

/* TAP -> ASIC. Insert the 8-byte F64 tag inline at offset 12 (after SMAC).
 * The tag is the PORT's, not a global: w1 selects which physical port the frame
 * egresses, so this is what makes one process able to drive several ports. */
static void inject(const struct port *p, const uint8_t *frame, int len)
{
    uint8_t out[MAX_FRAME + F64_LEN];
    int w, tlen;

    if (len < 14 || len + F64_LEN > (int)sizeof out) return;
    memcpy(out, frame, 12);                                   /* DMAC + SMAC   */
    for (w = 0; w < 4; w++) {                                 /* F64 tag       */
        out[12 + 2 * w]     = p->tag[w] >> 8;
        out[12 + 2 * w + 1] = p->tag[w] & 0xff;
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

/* Add one port. mac may be NULL. Returns 0 on success. */
static int add_port(const char *name, const char *glort, const char *mac)
{
    struct port *p;

    if (nports >= MAX_PORTS) { fprintf(stderr, "portd: too many ports\n"); return -1; }
    p = &ports[nports];
    snprintf(p->name, sizeof p->name, "%s", name);
    p->tag[0] = 0x0100;
    p->tag[1] = (uint16_t)strtoul(glort, 0, 16);
    p->tag[2] = 0xff00;
    p->tag[3] = 0x0000;

    p->fd = tap_open(p->name);
    if (p->fd < 0) return -1;
    if (tap_set_mac(p->name, mac) < 0)
        fprintf(stderr, "portd: WARNING %s continuing with the kernel's random MAC;"
                        " the ASIC punt tables expect a specific one\n", p->name);
    nports++;
    return 0;
}

/* "et3:03ed" or "et3:03ed:44:4c:a8:31:5d:ab" -- the MAC keeps its own colons,
 * so split on the FIRST two only. */
static int add_port_spec(char *spec)
{
    char *glort, *mac;

    glort = strchr(spec, ':');
    if (!glort) { fprintf(stderr, "portd: bad port spec '%s'\n", spec); return -1; }
    *glort++ = '\0';
    mac = strchr(glort, ':');
    if (mac) *mac++ = '\0';
    return add_port(spec, glort, mac);
}

int main(int argc, char **argv)
{
    unsigned secs = 0;
    struct fpdma_backing back;
    uint8_t buf[MAX_FRAME];
    struct pollfd pfd[MAX_PORTS];
    time_t t0;
    int i;

    { const char *d = getenv("PORTD_DEBUG"); if (d) dbg = atoi(d); }
    { const char *d = getenv("PORTD_TXFCS"); if (d) tx_fcs = atoi(d); }
    { const char *d = getenv("PORTD_SRCWORD"); if (d) src_word = atoi(d); }
    if (src_word < 0 || src_word > 3) {
        fprintf(stderr, "portd: PORTD_SRCWORD must be 0..3\n"); return 2;
    }
    printf("portd: tx_fcs=%d\n", tx_fcs);

    struct fpdma_kmod *k = NULL;
    size_t bsz = 0;
    volatile void *bar0;

    if (fpdma_kmod_open(&k) < 0) { fprintf(stderr, "kmod open failed (insmod fm6000dma.ko?)\n"); return 1; }
    bar0 = fpdma_kmod_bar0(k, &bsz);
    fm6000_hw_attach(&dev, bar0, bsz, "0000:02:00.0");
    back = fpdma_kmod_backing(k);
    if (fpdma_init(&fp, &dev, &back, 8, 64) < 0) { fprintf(stderr, "fpdma init failed\n"); return 1; }

    /* Two forms. The legacy one is what edgenos-up.sh has always used and it
     * must keep working verbatim:
     *     fm6000_portd <ifname> <glort-hex> [mac] [seconds]
     * The multi-port one takes any number of port specs:
     *     fm6000_portd et1:03ef:44:4c:a8:31:5d:ab et3:03ed [-t seconds]  */
    if (argc > 1 && strchr(argv[1], ':')) {
        for (i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-t") && i + 1 < argc) { secs = (unsigned)strtoul(argv[++i], 0, 0); continue; }
            if (add_port_spec(argv[i]) < 0) return 1;
        }
    } else {
        if (add_port(argc > 1 ? argv[1] : "et1",
                     argc > 2 ? argv[2] : "03ef",
                     argc > 3 ? argv[3] : NULL) < 0) return 1;
        secs = argc > 4 ? (unsigned)strtoul(argv[4], 0, 0) : 0;
    }
    if (!nports) { fprintf(stderr, "portd: no ports\n"); return 2; }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    for (i = 0; i < nports; i++) {
        printf("portd: %s <-> ASIC, egress GLORT 0x%04x\n", ports[i].name, ports[i].tag[1]);
        printf("  bring it up with:  ip link set %s up; ip addr add <a.b.c.d/len> dev %s\n",
               ports[i].name, ports[i].name);
    }
    if (nports > 1)
        printf("portd: punt demux on tag w%d (PORTD_SRCWORD); unmatched -> %s\n",
               src_word, ports[0].name);
    fflush(stdout);

    t0 = time(NULL);
    while (!stop_flag) {
        for (i = 0; i < nports; i++) {
            pfd[i].fd = ports[i].fd; pfd[i].events = POLLIN; pfd[i].revents = 0;
        }
        /* kernel -> ASIC */
        if (poll(pfd, nports, 1) > 0) {
            for (i = 0; i < nports; i++) {
                if (!(pfd[i].revents & POLLIN)) continue;
                int n = read(ports[i].fd, buf, sizeof buf);
                if (n > 0) inject(&ports[i], buf, n);
            }
        }
        rx_drain();                                       /* ASIC -> kernel */
        fpdma_tx_reclaim(&fp);

        if (secs && (unsigned)(time(NULL) - t0) >= secs) break;
    }

    printf("portd: rx=%lu tx=%lu rx_drop=%lu\n", n_rx, n_tx, n_rx_drop);
    fpdma_shutdown(&fp);
    for (i = 0; i < nports; i++) close(ports[i].fd);
    return 0;
}
