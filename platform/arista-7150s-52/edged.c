/*
 * edged.c - minimal EdgeNOS datapath daemon for the Arista 7150S-52 (FM6000)
 *
 * The Broadcom boards run core/datapath/edged.c (BMD-coupled). The FM6000 shares
 * no SDK, so the 7150 runs this small daemon that drives the chip through the
 * struct asic_ops seam (asic/fm6000/fm6000_edged.c) and bridges the CPU port to
 * a TUN netdev so the mgmt/control stack (FRR, SSH, etc.) sees a normal Linux
 * interface. This is the M2 mgmt-plane bring-up; L2/L3 offload grows later once
 * bcm56846 and fm6000 are unified behind asic_ops.
 *
 *   Usage: edged-7150 [ifname]     (default cpu0)
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
#include <signal.h>
#include <poll.h>
#include <stdint.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>

#include "fm6000_edged.h"

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

/* Open a TUN device in tap mode (L2 frames) named `name`. */
static int tap_open(const char *name)
{
    struct ifreq ifr;
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) { perror("open /dev/net/tun"); return -1; }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        perror("TUNSETIFF");
        close(fd);
        return -1;
    }
    return fd;
}

/* RX callback: write a punted frame out the TAP so Linux/control plane sees it. */
static void punt_to_tap(void *ctx, const void *frame, uint16_t len)
{
    int tap_fd = *(int *)ctx;
    if (write(tap_fd, frame, len) < 0 && errno != EAGAIN)
        perror("tap write");
}

int main(int argc, char **argv)
{
    const char *ifname = (argc > 1) ? argv[1] : "cpu0";
    const struct asic_ops *asic = fm6000_asic_ops();
    int tap_fd, intr_fd;
    uint8_t frame[2048];

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr, "edged-7150: backend=%s, cpu netdev=%s\n", asic->name, ifname);

    if (asic->init() != 0) {
        fprintf(stderr, "edged-7150: ASIC init failed\n");
        return 1;
    }

    tap_fd = tap_open(ifname);
    if (tap_fd < 0) { asic->shutdown(); return 1; }
    /* Non-blocking TAP so the poll loop never stalls on a full/empty queue. */
    fcntl(tap_fd, F_SETFL, fcntl(tap_fd, F_GETFL) | O_NONBLOCK);

    intr_fd = asic->intr_fd();
    fprintf(stderr, "edged-7150: up (%s). RX %s.\n", ifname,
            intr_fd >= 0 ? "MSI-driven" : "polled");

    while (running) {
        struct pollfd pfds[2];
        int n = 0, tap_idx, intr_idx = -1;

        tap_idx = n;
        pfds[n].fd = tap_fd; pfds[n].events = POLLIN; n++;
        if (intr_fd >= 0) {
            intr_idx = n;
            pfds[n].fd = intr_fd; pfds[n].events = POLLIN; n++;
        }

        if (poll(pfds, n, 100) < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        /* Host -> fabric: frames the control plane sent on the TAP. */
        if (pfds[tap_idx].revents & POLLIN) {
            ssize_t len;
            while ((len = read(tap_fd, frame, sizeof(frame))) > 0)
                asic->tx(frame, (uint16_t)len);
        }

        /* Fabric -> host: drain the MSI eventfd, then punt. */
        if (intr_idx >= 0 && (pfds[intr_idx].revents & POLLIN)) {
            uint64_t cnt;
            if (read(intr_fd, &cnt, sizeof(cnt)) < 0) { /* drain */ }
        }
        asic->rx_poll(64, punt_to_tap, &tap_fd);
    }

    fprintf(stderr, "edged-7150: shutting down\n");
    close(tap_fd);
    asic->shutdown();
    return 0;
}
