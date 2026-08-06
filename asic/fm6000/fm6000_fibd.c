/*
 * fm6000_fibd.c - mirror the Linux kernel FIB into the FM6000 hardware FIB.
 *
 * This is the last link in the chain. OSPF already works end to end:
 *
 *   ASIC -> punt -> TAP -> kernel -> ospfd -> zebra -> kernel FIB -> [ fibd ] -> ASIC
 *                                                                      ^^^^
 * zebra installs learned routes into the kernel; fibd watches the kernel table and
 * programs the ASIC so the traffic is forwarded in silicon rather than punted.
 * Any daemon that installs to the kernel (OSPF, BGP, static) is covered.
 *
 * Structures (recovered by tracing EOS; see docs/ROUTING-FIB.md):
 *   prefix array   0x33bxxx  sorted, ONE WORD per prefix, shadow copy 0x400 below
 *   action array   0x337xxx  two words per entry (fields NOT decoded)
 *   adjacency      NEXTHOP 0x160000 + 10*idx
 *   commit         0x33c09e <- 0, 0x33c09f <- key, 0x3f0000 <- 2
 *
 * *** LIMITATION -- read this before trusting it ***
 * The action-array fields are not decoded, so fibd cannot create a *new*
 * prefix->nexthop binding. It REUSES existing slots: it overwrites the prefix
 * word of slots that the boot-time configuration already pointed at the egress
 * we want, which changes WHICH destination uses that adjacency. That is enough
 * to program OSPF-learned routes that share a next hop (the common case on a
 * two-port box), and it is exactly how the hardware-routing test was validated.
 * It is NOT a general FIB: it cannot add a route to a next hop the table does
 * not already contain, and it does not manage the sort order.
 *
 * usage: fm6000_fibd [-i secs] [-s first:last] [-g gateway] [-n] [-v]
 *   -i  poll interval (default 5)
 *   -s  slot range to manage (default 240:253)
 *   -g  only program routes via this gateway (default: any via a non-eth0 iface)
 *   -n  dry run -- log what would be programmed, touch nothing
 *   -v  verbose
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <sys/mman.h>

#define PIN_STRAP      0x1C021u
#define FIB_PFX_BASE   0x33bf00u
#define FIB_PFX_SHADOW 0x400u
#define FIB_COMMIT_A   0x33c09eu
#define FIB_COMMIT_B   0x33c09fu
#define FIB_COMMIT_GO  0x3f0000u
#define MAX_ROUTES     64

struct route { uint32_t dst, mask, gw; char ifname[16]; };

static volatile uint32_t *M;
static int slot_first = 240, slot_last = 253, dry, verbose, interval = 5;
static uint32_t gw_filter;
static volatile sig_atomic_t stop_flag;
/* what we last wrote to each slot, so we only touch the chip on a real change */
static uint32_t programmed[256];

static void on_signal(int s) { (void)s; stop_flag = 1; }
static uint32_t rd(uint32_t w) { uint32_t v = M[w]; __sync_synchronize(); return v; }
static void     wr(uint32_t w, uint32_t v) { M[w] = v; __sync_synchronize(); usleep(200); }

static void commit(void)
{
    wr(FIB_COMMIT_A, 0x00000000u);
    wr(FIB_COMMIT_B, 0x00000f1eu);
    wr(FIB_COMMIT_GO, 0x00000002u);
}

static int plen(uint32_t mask)      /* netmask -> prefix length */
{
    int n = 0;
    while (mask & 0x80000000u) { n++; mask <<= 1; }
    return n;
}

static const char *ip4(uint32_t a, char *buf)
{
    sprintf(buf, "%u.%u.%u.%u", (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);
    return buf;
}

/* /proc/net/route gives dest/mask/gateway as little-endian hex. */
static int read_kernel_routes(struct route *out, int max)
{
    FILE *f = fopen("/proc/net/route", "r");
    char line[256];
    int n = 0;

    if (!f) { perror("open /proc/net/route"); return -1; }
    if (!fgets(line, sizeof line, f)) { fclose(f); return 0; }   /* header */
    while (fgets(line, sizeof line, f) && n < max) {
        char ifn[16];
        unsigned long d, g, m;
        int flags, refcnt, use, metric;

        if (sscanf(line, "%15s %lx %lx %x %d %d %d %lx",
                   ifn, &d, &g, &flags, &refcnt, &use, &metric, &m) != 8)
            continue;
        if (!strcmp(ifn, "lo") || !strcmp(ifn, "eth0"))          /* mgmt/loopback */
            continue;
        if (g == 0)                                              /* connected, not routed */
            continue;
        /* little-endian in the file -> host order */
        out[n].dst  = ((d & 0xff) << 24) | ((d & 0xff00) << 8) | ((d >> 8) & 0xff00) | (d >> 24);
        out[n].mask = ((m & 0xff) << 24) | ((m & 0xff00) << 8) | ((m >> 8) & 0xff00) | (m >> 24);
        out[n].gw   = ((g & 0xff) << 24) | ((g & 0xff00) << 8) | ((g >> 8) & 0xff00) | (g >> 24);
        snprintf(out[n].ifname, sizeof out[n].ifname, "%s", ifn);
        if (gw_filter && out[n].gw != gw_filter)
            continue;
        n++;
    }
    fclose(f);
    return n;
}

static void sync_once(void)
{
    struct route rt[MAX_ROUTES];
    int n = read_kernel_routes(rt, MAX_ROUTES), i, slot, changed = 0;
    char b1[20], b2[20], b3[20];      /* one buffer per %s: ip4() writes in place
                                       * and argument evaluation order is
                                       * unspecified, so sharing one garbles the
                                       * output. */

    if (n < 0) return;

    for (i = 0, slot = slot_first; i < n && slot <= slot_last; i++, slot++) {
        uint32_t a = FIB_PFX_BASE + slot;
        if (programmed[slot] == rt[i].dst) continue;             /* already there */
        if (verbose || dry)
            printf("  slot %d @0x%06x: %s -> %s/%d via %s dev %s%s\n", slot, a,
                   ip4(rd(a), b1), ip4(rt[i].dst, b2), plen(rt[i].mask),
                   ip4(rt[i].gw, b3), rt[i].ifname, dry ? "   [dry-run]" : "");
        if (!dry) {
            wr(a, rt[i].dst);
            wr(a - FIB_PFX_SHADOW, rt[i].dst);                   /* keep the shadow in step */
            programmed[slot] = rt[i].dst;
        }
        changed++;
    }
    if (changed && !dry) {
        commit();
        printf("fibd: programmed %d route(s), %d kernel route(s) eligible\n", changed, n);
        fflush(stdout);
    } else if (changed && dry) {
        printf("fibd: would program %d route(s)\n", changed);
    }
    if (n > slot_last - slot_first + 1)
        printf("fibd: WARNING %d eligible routes but only %d slots (%d:%d) -- extra routes NOT "
               "programmed\n", n, slot_last - slot_first + 1, slot_first, slot_last);
}

int main(int argc, char **argv)
{
    const char *bdf = "0000:02:00.0";
    char path[256];
    int fd, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) sscanf(argv[++i], "%d:%d", &slot_first, &slot_last);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) {
            unsigned a, b, c, d;
            if (sscanf(argv[++i], "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
                gw_filter = (a << 24) | (b << 16) | (c << 8) | d;
        }
        else if (!strcmp(argv[i], "-n")) dry = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else bdf = argv[i];
    }

    snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
    fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) { perror("open resource0"); return 1; }
    M = mmap(NULL, 32u * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (M == MAP_FAILED) { perror("mmap"); return 1; }
    if (rd(PIN_STRAP) != 0x208u) { fprintf(stderr, "chip not alive\n"); return 1; }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    printf("fibd: mirroring the kernel FIB into slots %d:%d every %ds%s\n",
           slot_first, slot_last, interval, dry ? " (DRY RUN)" : "");
    fflush(stdout);

    while (!stop_flag) {
        sync_once();
        sleep(interval);
    }
    printf("fibd: stopping\n");
    return 0;
}
