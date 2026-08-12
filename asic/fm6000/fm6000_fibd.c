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
 * Structures. These were originally guessed from the replay's shape; they are
 * now named and decoded -- see docs/ROUTED-PORT-ANATOMY.md.
 *
 *   prefix array   FFU_BST_KEY(engine, block, i)   0x10000*e + 0x400*b + i + 0x308000
 *   action array   FFU_BST_ACTION_ROUTE(e, b, i,w) 0x10000*e + 0x800*b + 2*i + 0x300000
 *   adjacency      NEXTHOP_TABLE[idx]              0x160000 + 2*idx   (WIDTH 2)
 *   root keys      FFU_BST_ROOT_KEYS(e, 15, w)     0x10000*e + 0x0c080 + 0x300000
 *
 * The old note here said "shadow copy 0x400 below" and "adjacency + 10*idx".
 * The first is right about the address and wrong about the meaning: 0x400 words
 * is exactly one block, and the two populated blocks are an ACTIVE/STANDBY pair.
 * FFU_BST_ROOT_KEYS[e][15] holds Partition[43:40], which names the live one.
 * EOS updates by rebuilding the standby in full and then flipping that field --
 * verified by adding a route on a live box and watching partition 14 -> 15 with
 * the live block untouched. The second is simply wrong; the stride is 2 words.
 *
 * Keys are sorted ASCENDING and right-aligned against i = 1023. This is
 * interval-based LPM: boundaries partition the address space and each carries
 * the action for the range it opens (~2.5 boundaries per route -- a connected
 * /24 contributes network, local address and broadcast).
 *
 * FFU_BST_ACTION_ROUTE fields, from the register header:
 *   NextHopBaseIndex[15:0]  NextHopRange[22:16]  NextHopEntryType[23]  LPM[31:24]
 *   TagData[43:32]  TagCmd[45:44]  Route[46]  Precedence[49:47]
 *
 * *** LIMITATION -- read this before trusting it ***
 * The CODE BELOW still does the old thing, and has not yet been rewritten to
 * use any of the above. It REUSES existing slots: it overwrites the prefix
 * word of slots that the boot-time configuration already pointed at the egress
 * we want, which changes WHICH destination uses that adjacency. That is enough
 * to program OSPF-learned routes that share a next hop (the common case on a
 * two-port box), and it is exactly how the hardware-routing test was validated.
 * It is NOT a general FIB: it cannot add a route to a next hop the table does
 * not already contain, and it does not manage the sort order.
 *
 * *** SUPERSEDED -- use fm6000_fibgen | fm6000_bst -p instead ***
 * That real implementation now exists as two tools:
 *
 *     fm6000_fibgen --map et1=0x3ef,et3=0x3f0 --nh-out /tmp/nh | fm6000_bst -p /dev/stdin
 *     fm6000_bst -N /tmp/nh
 *
 * fibgen walks the kernel RIB/neighbours/addresses and emits the boundary list
 * plus its NEXTHOP entries; fm6000_bst -p sorts, right-aligns, writes all 1024
 * slots of the standby and flips Partition. Both are validated against a live
 * EOS FIB (see docs/ROUTED-PORT-ANATOMY.md): the block reproduces byte for byte
 * from shuffled input, and the boundary list matches 46 for 46.
 *
 * This daemon is kept because it is what the hardware-routing test was
 * validated with, and because it polls -- neither new tool does. Wiring the
 * poll loop here to the new pair is the remaining work; do that rather than
 * extending the slot-reuse code below.
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
