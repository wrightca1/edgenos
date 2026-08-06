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

static struct fpdma     fp;
static struct fm6000_dev dev;
static int      tapfd = -1;
static uint16_t TAG[4] = { 0x0100, 0x03ef, 0xff00, 0x0000 };
static volatile sig_atomic_t stop_flag;
static unsigned long n_rx, n_tx, n_rx_drop;

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
            uint16_t n = len - off;
            if (n > MAX_FRAME) n = MAX_FRAME;
            if (write(tapfd, raw + off, n) < 0 && errno != EAGAIN) n_rx_drop++;
            else n_rx++;
            return;
        }
        /* tag still present at +12: splice it out */
        if (off + 22 <= (int)len) {
            uint16_t et2 = (raw[off + 20] << 8) | raw[off + 21];
            if (et2 == 0x0800 || et2 == 0x0806 || et2 == 0x86dd || et2 == 0x8100) {
                uint16_t body = len - off - 20;
                if (body + 12 > MAX_FRAME) { n_rx_drop++; return; }
                memcpy(clean, raw + off, 12);
                memcpy(clean + 12, raw + off + 20, body);
                if (write(tapfd, clean, body + 12) < 0 && errno != EAGAIN) n_rx_drop++;
                else n_rx++;
                return;
            }
        }
    }
    n_rx_drop++;                       /* no recognisable ethertype */
}

/* TAP -> ASIC. Insert the 8-byte F64 tag inline at offset 12 (after SMAC). */
static void inject(const uint8_t *frame, int len)
{
    uint8_t out[MAX_FRAME + F64_LEN];
    int w;

    if (len < 14 || len + F64_LEN > (int)sizeof out) return;
    memcpy(out, frame, 12);                                   /* DMAC + SMAC   */
    for (w = 0; w < 4; w++) {                                 /* F64 tag       */
        out[12 + 2 * w]     = TAG[w] >> 8;
        out[12 + 2 * w + 1] = TAG[w] & 0xff;
    }
    memcpy(out + 12 + F64_LEN, frame + 12, len - 12);         /* ethertype ... */
    if (fpdma_tx(&fp, out, (uint16_t)(len + F64_LEN)) == 0) n_tx++;
}

int main(int argc, char **argv)
{
    const char *ifname = argc > 1 ? argv[1] : "et1";
    unsigned secs = argc > 4 ? (unsigned)strtoul(argv[4], 0, 0) : 0;
    struct fpdma_backing back;
    uint8_t buf[MAX_FRAME];
    time_t t0;

    if (argc > 2) TAG[1] = (uint16_t)strtoul(argv[2], 0, 16);

    struct fpdma_kmod *k = NULL;
    size_t bsz = 0;
    volatile void *bar0;

    if (fpdma_kmod_open(&k) < 0) { fprintf(stderr, "kmod open failed (insmod fm6000dma.ko?)\n"); return 1; }
    bar0 = fpdma_kmod_bar0(k, &bsz);
    fm6000_hw_attach(&dev, bar0, bsz, "0000:02:00.0");
    back = fpdma_kmod_backing(k);
    if (fpdma_init(&fp, &dev, &back, 8, 64) < 0) { fprintf(stderr, "fpdma init failed\n"); return 1; }

    tapfd = tap_open(ifname);
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
        /* ASIC -> kernel.
         * Do NOT use fpdma_rx_poll(): it walks descriptors by ring tail, which
         * races the hardware's fill order and pairs a descriptor's length with
         * the wrong buffer -- the frames then fail the ethertype scan. Scan all
         * descriptors and pair descriptor i with buf_va[i], as fm6000_l3 does. */
        for (unsigned i = 0; i < fp.rx.size; i++) {
            volatile uint8_t *d = fp.rx.desc + i * FM6000_DESC_STRIDE;
            uint16_t rlen;

            if (!(d[0] & FM6000_DESC_DONE)) continue;
            rlen = *(volatile uint16_t *)(d + 2);
            if (rlen) on_punt(NULL, (uint8_t *)fp.rx.buf_va[i], rlen);
            d[0] = FM6000_DESC_RX_READY;                  /* re-arm */
            __sync_synchronize();
            fm6000_dma_write(&dev, FM6000_DMA_COMMAND, FM6000_DMA_CMD_RX_POST);
        }
        fpdma_tx_reclaim(&fp);

        if (secs && (unsigned)(time(NULL) - t0) >= secs) break;
    }

    printf("portd: rx=%lu tx=%lu rx_drop=%lu\n", n_rx, n_tx, n_rx_drop);
    fpdma_shutdown(&fp);
    close(tapfd);
    return 0;
}
