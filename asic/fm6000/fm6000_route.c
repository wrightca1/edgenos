/*
 * fm6000_route.c - program the FM6000 hardware FIB from EdgeNOS.
 *
 * IPv4 forwarding on the FM6000 lives in the FFU (the same TCAM used for ACLs);
 * there is no separate LPM/TRIE block. The structures were recovered by tracing
 * EOS installing routes on live silicon -- see docs/EDGENOS-7150.md (was ROUTING-FIB).
 *
 *   prefix array   0x33bxxx   sorted, ONE WORD per prefix (plain u32 network
 *                             address), a second copy 0x400 words below, and it
 *                             grows DOWNWARD.
 *   action array   0x337xxx   two words per entry (fields not yet decoded).
 *   adjacency      NEXTHOP 0x160000 + 10*idx:
 *                    w0 = MAC[5:2]                 (low four bytes)
 *                    w1 = GLORT << 16 | MAC[1:0]   (egress GLORT + top two)
 *   commit         0x33c09e <- 0, 0x33c09f <- key, then 0x3f0000 <- 2
 *
 * Verified against EOS's own tables: 0x160014/15 decode to 80:a2:35:81:ca:b4
 * with GLORT 0x03ef (Et1) and 0x16001e/1f to ...cab5 with GLORT 0x03ee (Et2),
 * which match EOS's ARP table exactly.
 *
 * usage:
 *   fm6000_route [bdf] dump                       show the prefix array
 *   fm6000_route [bdf] setpfx <slot> <a.b.c.d>    overwrite one prefix slot
 *   fm6000_route [bdf] nexthop <idx> <mac> <glort>
 *   fm6000_route [bdf] commit
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

#define PIN_STRAP        0x1C021u
#define FIB_PFX_BASE     0x33bf00u   /* primary prefix array            */
#define FIB_PFX_SHADOW   0x400u      /* second copy lives this far BELOW */
#define FIB_PFX_LAST     0x33bfffu
#define FIB_ACT_BASE     0x337000u
#define NEXTHOP_BASE     0x160000u
#define NEXTHOP_STRIDE   10u
#define FIB_COMMIT_A     0x33c09eu
#define FIB_COMMIT_B     0x33c09fu
#define FIB_COMMIT_GO    0x3f0000u

static volatile uint32_t *M;

static uint32_t rd(uint32_t w) { uint32_t v = M[w]; __sync_synchronize(); return v; }
static void     wr(uint32_t w, uint32_t v) { M[w] = v; __sync_synchronize(); usleep(200); }

static int alive(void)
{
    uint32_t p = rd(PIN_STRAP);
    if (p != 0x208u) { fprintf(stderr, "chip not alive (PIN=0x%08x)\n", p); return 0; }
    return 1;
}

static void dump(void)
{
    uint32_t a;
    int n = 0;

    printf("slot  addr      prefix\n");
    for (a = FIB_PFX_BASE; a <= FIB_PFX_LAST; a++) {
        uint32_t v = rd(a);
        if ((v >> 24) == 0x0a || (v && v != 0xffffffffu)) {
            printf("  %3u  0x%06x  %u.%u.%u.%u\n", a - FIB_PFX_BASE, a,
                   (v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
            n++;
        }
    }
    printf("(%d populated slots)\n", n);
}

/* Write one prefix slot in BOTH copies. The hardware keeps a shadow 0x400 words
 * below the primary; updating only one leaves them inconsistent. */
static void setpfx(uint32_t slot, uint32_t prefix)
{
    uint32_t a = FIB_PFX_BASE + slot;

    if (a > FIB_PFX_LAST) { fprintf(stderr, "slot %u out of range\n", slot); exit(2); }
    printf("slot %u @0x%06x: 0x%08x -> 0x%08x  (shadow @0x%06x)\n",
           slot, a, rd(a), prefix, a - FIB_PFX_SHADOW);
    wr(a, prefix);
    wr(a - FIB_PFX_SHADOW, prefix);
}

static void nexthop(uint32_t idx, const char *mac, uint32_t glort)
{
    unsigned b[6];
    uint32_t a = NEXTHOP_BASE + NEXTHOP_STRIDE * idx;

    if (sscanf(mac, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        fprintf(stderr, "bad MAC '%s' (want aa:bb:cc:dd:ee:ff)\n", mac); exit(2);
    }
    uint32_t w0 = ((uint32_t)b[2] << 24) | (b[3] << 16) | (b[4] << 8) | b[5];
    uint32_t w1 = (glort << 16) | ((uint32_t)b[0] << 8) | b[1];
    printf("nexthop %u @0x%06x: w0=0x%08x w1=0x%08x  (%s via GLORT 0x%04x)\n",
           idx, a, w0, w1, mac, glort);
    wr(a,     w0);
    wr(a + 1, w1);
}

static void commit(void)
{
    printf("commit: 0x%06x<-0 0x%06x<-0x00000f1e 0x%06x<-2\n",
           FIB_COMMIT_A, FIB_COMMIT_B, FIB_COMMIT_GO);
    wr(FIB_COMMIT_A, 0x00000000u);
    wr(FIB_COMMIT_B, 0x00000f1eu);
    wr(FIB_COMMIT_GO, 0x00000002u);
}

int main(int argc, char **argv)
{
    const char *bdf = "0000:02:00.0";
    char path[256];
    int fd, i = 1;

    if (argc > 1 && strchr(argv[1], ':') && strchr(argv[1], '.')) bdf = argv[i++];
    if (i >= argc) {
        fprintf(stderr,
            "usage: %s [bdf] dump | setpfx <slot> <a.b.c.d> | nexthop <idx> <mac> <glort> | commit\n",
            argv[0]);
        return 2;
    }

    snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
    fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) { perror("open resource0"); return 1; }
    M = mmap(NULL, 32u * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (M == MAP_FAILED) { perror("mmap"); return 1; }
    if (!alive()) return 1;

    if (!strcmp(argv[i], "dump")) {
        dump();
    } else if (!strcmp(argv[i], "setpfx") && argc > i + 2) {
        unsigned a, b, c, d;
        if (sscanf(argv[i + 2], "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
            fprintf(stderr, "bad prefix '%s'\n", argv[i + 2]); return 2;
        }
        setpfx((uint32_t)strtoul(argv[i + 1], 0, 0),
               (a << 24) | (b << 16) | (c << 8) | d);
        commit();
    } else if (!strcmp(argv[i], "nexthop") && argc > i + 3) {
        nexthop((uint32_t)strtoul(argv[i + 1], 0, 0), argv[i + 2],
                (uint32_t)strtoul(argv[i + 3], 0, 16));
        commit();
    } else if (!strcmp(argv[i], "commit")) {
        commit();
    } else {
        fprintf(stderr, "unknown command '%s'\n", argv[i]); return 2;
    }
    printf("PIN=0x%08x\n", rd(PIN_STRAP));
    return 0;
}
