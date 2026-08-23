/*
 * edged.c - EdgeNOS datapath daemon for the QEMU/KVM x86_64 virtual switch (edged-vswitch)
 *
 * Same shape as the Arista 7150's board daemon: drive the "ASIC" through the
 * struct asic_ops seam (asic/vswitch/vswitch.c — an L2 learning switch over the
 * pge* netdevs) and bridge the CPU port to a TAP netdev (cpu0) so the control plane
 * sees a normal Linux interface. The loop below is the 7150 loop; only the backend
 * symbol differs — which is the point: one daemon, any backend.
 *
 *   Usage: edged-vswitch [ifname]     (default cpu0)     SIGUSR1: dump ports + MAC table
 *
 * Copyright (C) 2024-2026 EdgeNOS Contributors.
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

#include "vswitch.h"

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t want_dump = 0;
static void on_signal(int sig) { (void)sig; running = 0; }
static void on_usr1(int sig)   { (void)sig; want_dump = 1; }

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
    const struct asic_ops *asic = vswitch_asic_ops();
    int tap_fd, intr_fd;
    uint8_t frame[2048];

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_usr1);

    fprintf(stderr, "edged-vswitch: backend=%s, cpu netdev=%s\n", asic->name, ifname);

    if (asic->init() != 0) {
        fprintf(stderr, "edged-vswitch: ASIC init failed\n");
        return 1;
    }

    tap_fd = tap_open(ifname);
    if (tap_fd < 0) { asic->shutdown(); return 1; }
    /* Non-blocking TAP so the poll loop never stalls on a full/empty queue. */
    fcntl(tap_fd, F_SETFL, fcntl(tap_fd, F_GETFL) | O_NONBLOCK);

    intr_fd = asic->intr_fd();
    fprintf(stderr, "edged-vswitch: up (%s). RX %s.\n", ifname,
            intr_fd >= 0 ? "event-driven" : "polled");

    while (running) {
        struct pollfd pfds[2];
        int n = 0, tap_idx, intr_idx = -1;

        if (want_dump) { want_dump = 0; vswitch_dump(); }

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

        /* Fabric -> host: drain the interrupt fd (eventfd or epoll), then punt. */
        if (intr_idx >= 0 && (pfds[intr_idx].revents & POLLIN)) {
            uint64_t cnt;
            if (read(intr_fd, &cnt, sizeof(cnt)) < 0) { /* drain (no-op for an epoll fd) */ }
        }
        asic->rx_poll(64, punt_to_tap, &tap_fd);
    }

    fprintf(stderr, "edged-vswitch: shutting down\n");
    close(tap_fd);
    asic->shutdown();
    return 0;
}
