/*
 * vswitch.c - EdgeNOS software "ASIC": an L2 learning switch over Linux netdevs,
 * exposed through the core/datapath/asic_ops.h backend seam.
 *
 * There is no silicon in a VM, so the "chip" is this file: the front-panel ports are
 * the physical NICs handed to it (AF_PACKET raw sockets, promiscuous), and the CPU
 * port is the daemon's tx()/rx_poll() pair — the board edged bridges that to a TAP
 * netdev (cpu0) exactly like the Arista 7150 does with its FM6000, so the same
 * daemon loop drives real silicon and this. Forwarding model (M2, deliberately
 * small): one L2 domain, MAC learning with ageing, unknown-unicast/broadcast/
 * multicast flooding, CPU treated as just another port in the MAC table (its MAC is
 * learnt from the frames the control plane injects). No VLANs, no L3 offload yet —
 * those grow behind the same seam, which is the point.
 *
 * Port set: env EDGENOS_VSWITCH_PORTS="pge0 pge1 ..." (space separated), else every
 * netdev named pge<N> (what platform-svc names the NICs in vswitch mode), in order.
 *
 * Copyright (C) 2026 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <arpa/inet.h>
#include <stdarg.h>

#include "vswitch.h"

struct vport {
    char     name[IFNAMSIZ];
    int      ifindex;
    int      fd;                /* AF_PACKET socket, or -1 */
    int      enabled;
    uint8_t  mac[6];
    uint64_t rx, tx, drop;
};

struct mac_entry {
    uint8_t  mac[6];
    uint8_t  valid;
    uint8_t  port;              /* 0..VSW_MAX_PORTS-1, or VSW_CPU_PORT */
    uint32_t ts;                /* last seen, seconds (monotonic) */
};

struct punt {
    uint16_t len;
    uint8_t  frame[VSW_MAX_FRAME];
};

static struct vport      ports[VSW_MAX_PORTS];
static int               nports;
static struct mac_entry  mactab[VSW_MAC_ENTRIES];
static struct punt       punt_ring[VSW_PUNT_RING];
static unsigned          punt_head, punt_tail;       /* head=write, tail=read */
static int               epfd = -1;
static uint64_t          st_cpu_tx, st_cpu_rx, st_flood, st_fwd, st_punt_drop;

static uint32_t now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_sec;
}

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("vswitch: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---------------------------------------------------------------- MAC table */
static unsigned mac_hash(const uint8_t *m)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) { h ^= m[i]; h *= 16777619u; }
    return h & (VSW_MAC_ENTRIES - 1);
}

static int mac_lookup(const uint8_t *m)
{
    unsigned h = mac_hash(m);
    for (int i = 0; i < 8; i++) {                 /* linear probe, 8 deep */
        struct mac_entry *e = &mactab[(h + i) & (VSW_MAC_ENTRIES - 1)];
        if (!e->valid)
            return -1;
        if (!memcmp(e->mac, m, 6)) {
            if (now_s() - e->ts > VSW_MAC_AGE_S) { e->valid = 0; return -1; }
            return e->port;
        }
    }
    return -1;
}

static void mac_learn(const uint8_t *m, int port)
{
    if (m[0] & 1)                                 /* never learn multicast/broadcast SAs */
        return;
    unsigned h = mac_hash(m);
    struct mac_entry *victim = NULL;
    for (int i = 0; i < 8; i++) {
        struct mac_entry *e = &mactab[(h + i) & (VSW_MAC_ENTRIES - 1)];
        if (e->valid && !memcmp(e->mac, m, 6)) {
            if (e->port != port) {
                e->port = port;                   /* station move */
            }
            e->ts = now_s();
            return;
        }
        if (!e->valid) { victim = e; break; }
        if (!victim || e->ts < victim->ts) victim = e;   /* oldest as fallback */
    }
    memcpy(victim->mac, m, 6);
    victim->port = port;
    victim->ts = now_s();
    victim->valid = 1;
}

/* ---------------------------------------------------------------- ports */
static int port_open(struct vport *p)
{
    struct ifreq ifr;
    struct sockaddr_ll sll;
    struct packet_mreq mr;
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) { logmsg("%s: socket: %s", p->name, strerror(errno)); return -1; }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%.15s", p->name);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) { logmsg("%s: no such netdev", p->name); close(fd); return -1; }
    p->ifindex = ifr.ifr_ifindex;
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0)
        memcpy(p->mac, ifr.ifr_hwaddr.sa_data, 6);

    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = p->ifindex;
    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) { logmsg("%s: bind: %s", p->name, strerror(errno)); close(fd); return -1; }

    memset(&mr, 0, sizeof(mr));
    mr.mr_ifindex = p->ifindex;
    mr.mr_type = PACKET_MR_PROMISC;
    setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    p->fd = fd;
    return 0;
}

static int port_link(struct vport *p, int up)
{
    struct ifreq ifr;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%.15s", p->name);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) { close(s); return -1; }
    if (up) ifr.ifr_flags |= IFF_UP | IFF_PROMISC; else ifr.ifr_flags &= ~IFF_UP;
    int rc = ioctl(s, SIOCSIFFLAGS, &ifr);
    close(s);
    return rc;
}

static int cmp_names(const void *a, const void *b)
{
    const char *x = a, *y = b;
    int nx = atoi(x + 3), ny = atoi(y + 3);          /* pge<N> */
    return nx - ny;
}

static int discover_ports(void)
{
    char names[VSW_MAX_PORTS][IFNAMSIZ];
    int n = 0;
    const char *env = getenv("EDGENOS_VSWITCH_PORTS");
    if (env && *env) {
        char *dup = strdup(env), *save = NULL;
        for (char *t = strtok_r(dup, " ,", &save); t && n < VSW_MAX_PORTS; t = strtok_r(NULL, " ,", &save))
            snprintf(names[n++], IFNAMSIZ, "%.15s", t);
        free(dup);
    } else {
        DIR *d = opendir("/sys/class/net");
        struct dirent *de;
        if (!d) return -1;
        while ((de = readdir(d)) && n < VSW_MAX_PORTS)
            if (!strncmp(de->d_name, "pge", 3) && de->d_name[3] >= '0' && de->d_name[3] <= '9')
                snprintf(names[n++], IFNAMSIZ, "%.15s", de->d_name);
        closedir(d);
        qsort(names, n, IFNAMSIZ, cmp_names);
    }
    for (int i = 0; i < n; i++) {
        memset(&ports[i], 0, sizeof(ports[i]));
        snprintf(ports[i].name, IFNAMSIZ, "%.15s", names[i]);
        ports[i].fd = -1;
    }
    nports = n;
    return n;
}

/* ---------------------------------------------------------------- forwarding */
static void port_send(struct vport *p, const void *frame, uint16_t len)
{
    if (!p->enabled || p->fd < 0) { p->drop++; return; }
    if (send(p->fd, frame, len, MSG_DONTWAIT) < 0) p->drop++; else p->tx++;
}

static void punt_to_cpu(const void *frame, uint16_t len)
{
    unsigned next = (punt_head + 1) % VSW_PUNT_RING;
    if (next == punt_tail) { st_punt_drop++; return; }        /* ring full */
    if (len > VSW_MAX_FRAME) len = VSW_MAX_FRAME;
    punt_ring[punt_head].len = len;
    memcpy(punt_ring[punt_head].frame, frame, len);
    punt_head = next;
}

/* one frame entering the fabric from `in` (a port index, or VSW_CPU_PORT) */
static void forward(int in, const uint8_t *frame, uint16_t len)
{
    if (len < 14) return;
    const uint8_t *da = frame, *sa = frame + 6;
    mac_learn(sa, in);
    int out = (da[0] & 1) ? -1 : mac_lookup(da);
    if (out >= 0 && out != in) {
        st_fwd++;
        if (out == VSW_CPU_PORT) punt_to_cpu(frame, len);
        else port_send(&ports[out], frame, len);
        return;
    }
    if (out == in)                                  /* hairpin: drop (same as a real switch) */
        return;
    st_flood++;                                     /* unknown / bcast / mcast: flood */
    for (int i = 0; i < nports; i++)
        if (i != in) port_send(&ports[i], frame, len);
    if (in != VSW_CPU_PORT) punt_to_cpu(frame, len);
}

/* ---------------------------------------------------------------- asic_ops */
static int vsw_init(void)
{
    int n = discover_ports();
    if (n <= 0) { logmsg("no ports (set EDGENOS_VSWITCH_PORTS or name NICs pge<N>)"); return -1; }
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { logmsg("epoll_create: %s", strerror(errno)); return -1; }
    int ok = 0;
    for (int i = 0; i < nports; i++) {
        struct vport *p = &ports[i];
        if (port_open(p) < 0) continue;
        port_link(p, 1);
        p->enabled = 1;
        struct epoll_event ev = { .events = EPOLLIN, .data.u32 = (uint32_t)i };
        epoll_ctl(epfd, EPOLL_CTL_ADD, p->fd, &ev);
        logmsg("port %d = %s (ifindex %d, %02x:%02x:%02x:%02x:%02x:%02x)", i, p->name, p->ifindex,
               p->mac[0], p->mac[1], p->mac[2], p->mac[3], p->mac[4], p->mac[5]);
        ok++;
    }
    memset(mactab, 0, sizeof(mactab));
    punt_head = punt_tail = 0;
    logmsg("up: %d/%d ports, L2 learning switch, CPU = port %d", ok, nports, VSW_CPU_PORT);
    return ok ? 0 : -1;
}

static int vsw_port_set(int port, int enable, int speed_mb)
{
    (void)speed_mb;                                 /* virtio: speed is whatever the host gives */
    if (port < 0 || port >= nports) return -1;
    ports[port].enabled = !!enable;
    return port_link(&ports[port], enable);
}

static int vsw_tx(const void *frame, uint16_t len)
{
    st_cpu_tx++;
    forward(VSW_CPU_PORT, frame, len);
    return 0;
}

static int vsw_rx_poll(int budget, asic_rx_cb cb, void *ctx)
{
    uint8_t buf[VSW_MAX_FRAME];
    struct sockaddr_ll from;
    socklen_t flen;
    int work = 0;

    /* 1. move frames through the fabric (and into the punt ring) */
    for (int i = 0; i < nports && work < budget; i++) {
        struct vport *p = &ports[i];
        if (p->fd < 0) continue;
        for (;;) {
            flen = sizeof(from);
            ssize_t n = recvfrom(p->fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &flen);
            if (n <= 0) break;
            if (from.sll_pkttype == PACKET_OUTGOING) continue;   /* our own TX echoed back */
            p->rx++;
            if (p->enabled) forward(i, buf, (uint16_t)n);
            if (++work >= budget) break;
        }
    }
    /* 2. deliver punted frames to the CPU port */
    int delivered = 0;
    while (punt_tail != punt_head && delivered < budget) {
        struct punt *pk = &punt_ring[punt_tail];
        cb(ctx, pk->frame, pk->len);
        punt_tail = (punt_tail + 1) % VSW_PUNT_RING;
        delivered++;
        st_cpu_rx++;
    }
    return delivered;
}

static int vsw_intr_fd(void)
{
    return epfd;                                    /* readable whenever a port has frames */
}

static void vsw_shutdown(void)
{
    for (int i = 0; i < nports; i++) {
        if (ports[i].fd >= 0) { close(ports[i].fd); ports[i].fd = -1; }
    }
    if (epfd >= 0) { close(epfd); epfd = -1; }
    logmsg("down: cpu tx %llu rx %llu, fwd %llu flood %llu punt-drop %llu",
           (unsigned long long)st_cpu_tx, (unsigned long long)st_cpu_rx,
           (unsigned long long)st_fwd, (unsigned long long)st_flood, (unsigned long long)st_punt_drop);
}

static const struct asic_ops vswitch_ops = {
    .name     = "vswitch",
    .init     = vsw_init,
    .port_set = vsw_port_set,
    .tx       = vsw_tx,
    .rx_poll  = vsw_rx_poll,
    .intr_fd  = vsw_intr_fd,
    .shutdown = vsw_shutdown,
};

const struct asic_ops *vswitch_asic_ops(void) { return &vswitch_ops; }

int vswitch_port_count(void) { return nports; }
const char *vswitch_port_name(int port) { return (port >= 0 && port < nports) ? ports[port].name : NULL; }

void vswitch_dump(void)
{
    fprintf(stderr, "vswitch: %d ports  cpu tx %llu rx %llu  fwd %llu flood %llu punt-drop %llu\n", nports,
            (unsigned long long)st_cpu_tx, (unsigned long long)st_cpu_rx,
            (unsigned long long)st_fwd, (unsigned long long)st_flood, (unsigned long long)st_punt_drop);
    for (int i = 0; i < nports; i++)
        fprintf(stderr, "  port %-2d %-8s %s rx %llu tx %llu drop %llu\n", i, ports[i].name,
                ports[i].enabled ? "up  " : "down", (unsigned long long)ports[i].rx,
                (unsigned long long)ports[i].tx, (unsigned long long)ports[i].drop);
    int shown = 0;
    for (int i = 0; i < VSW_MAC_ENTRIES && shown < 64; i++) {
        struct mac_entry *e = &mactab[i];
        if (!e->valid || now_s() - e->ts > VSW_MAC_AGE_S) continue;
        fprintf(stderr, "  mac %02x:%02x:%02x:%02x:%02x:%02x -> %s (age %us)\n",
                e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                e->port == VSW_CPU_PORT ? "cpu" : ports[e->port].name, now_s() - e->ts);
        shown++;
    }
}
