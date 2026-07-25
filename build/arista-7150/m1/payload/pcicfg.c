/*
 * pcicfg.c - read/write PCI config space via sysfs (M1 has no setpci/python).
 *   pcicfg <BDF> <off>          # read dword  (e.g. pcicfg 0000:00:04.0 0x68)
 *   pcicfg <BDF> <off> <val>    # write dword
 *   pcicfg <BDF> linkctl        # find PCIe cap, print Link Control offset+value+LinkDisable bit
 *   pcicfg <BDF> link           # decode Link Cap/Ctl/Status incl. DLLLA (bit13) + speed/width
 *   pcicfg <BDF> retrain        # set Link Control Retrain-Link (bit5) to kick LTSSM
 *   pcicfg <BDF> hotreset       # secondary-bus (hot) reset of the downstream link, then re-train
 *   pcicfg <BDF> gen1           # pin target link speed to Gen1 (2.5GT/s) + retrain (skip Gen2 EQ)
 * Build like scdreg:  gcc -O2 -o pcicfg pcicfg.c
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

static int cfgopen(const char *bdf, int wr) {
    char path[256];
    snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/config", bdf);
    return open(path, wr ? O_RDWR : O_RDONLY);
}
static uint8_t  rb(int fd, unsigned o){ uint8_t v=0;  pread(fd,&v,1,o); return v; }
static uint16_t rw(int fd, unsigned o){ uint16_t v=0; pread(fd,&v,2,o); return v; }
static uint32_t rl(int fd, unsigned o){ uint32_t v=0; pread(fd,&v,4,o); return v; }

/* walk the capability list, return the PCIe cap (id 0x10) offset, or 0 */
static unsigned find_pcie_cap(int fd) {
    unsigned p = rb(fd, 0x34);
    int guard = 48;
    while (p >= 0x40 && guard--) {
        if (rb(fd, p) == 0x10) return p;
        p = rb(fd, p + 1);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: pcicfg <BDF> <off|linkctl> [val]\n"); return 2; }
    if (!strcmp(argv[2], "linkctl")) {
        int fd = cfgopen(argv[1], 0);
        if (fd < 0) { perror("open"); return 1; }
        unsigned cap = find_pcie_cap(fd);
        if (!cap) { fprintf(stderr, "no PCIe cap\n"); return 1; }
        unsigned lc = cap + 0x10;            /* Link Control */
        uint16_t v = rw(fd, lc);
        printf("%s PCIe cap@0x%02x LinkControl@0x%02x = 0x%04x  LinkDisable(bit4)=%d\n",
               argv[1], cap, lc, v, (v >> 4) & 1);
        close(fd); return 0;
    }
    if (!strcmp(argv[2], "link")) {
        int fd = cfgopen(argv[1], 0);
        if (fd < 0) { perror("open"); return 1; }
        unsigned cap = find_pcie_cap(fd);
        if (!cap) { fprintf(stderr, "no PCIe cap\n"); return 1; }
        uint32_t lcap = rl(fd, cap + 0x0c);   /* Link Capabilities */
        uint16_t lctl = rw(fd, cap + 0x10);   /* Link Control */
        uint16_t lsta = rw(fd, cap + 0x12);   /* Link Status  */
        int dllla   = (lsta >> 13) & 1;       /* Data Link Layer Link Active - THE decisive bit */
        int lt      = (lsta >> 11) & 1;       /* Link Training */
        int cspeed  =  lsta & 0xf;            /* current link speed (1=2.5 2=5 3=8 GT/s) */
        int cwidth  = (lsta >> 4) & 0x3f;     /* negotiated width */
        printf("%s PCIe cap@0x%02x\n", argv[1], cap);
        printf("  LinkCap   @0x%02x = 0x%08x  maxSpeed=%u maxWidth=x%u\n",
               cap + 0x0c, lcap, lcap & 0xf, (lcap >> 4) & 0x3f);
        printf("  LinkCtrl  @0x%02x = 0x%04x    LinkDisable(b4)=%d Retrain(b5)=%d\n",
               cap + 0x10, lctl, (lctl >> 4) & 1, (lctl >> 5) & 1);
        printf("  LinkStat  @0x%02x = 0x%04x    DLLLA(b13)=%d LinkTraining(b11)=%d curSpeed=%d curWidth=x%d\n",
               cap + 0x12, lsta, dllla, lt, cspeed, cwidth);
        printf("  VERDICT: DLLLA=%d -> %s\n", dllla,
               dllla ? "LINK UP (FM6000 driving PCIe; enum gap is PCI-resource: resize bridge window / pci=realloc)"
                     : "LINK DOWN (FM6000 not driving PCIe; chip-boot problem, not enumeration)");
        close(fd); return 0;
    }
    if (!strcmp(argv[2], "retrain")) {
        int fd = cfgopen(argv[1], 1);
        if (fd < 0) { perror("open"); return 1; }
        unsigned cap = find_pcie_cap(fd);
        if (!cap) { fprintf(stderr, "no PCIe cap\n"); return 1; }
        unsigned lc = cap + 0x10;
        uint16_t v = rw(fd, lc), nv = v | (1u << 5);   /* Retrain Link */
        if (pwrite(fd, &nv, 2, lc) != 2) { perror("pwrite"); return 1; }
        printf("%s LinkControl@0x%02x 0x%04x -> 0x%04x (Retrain bit5 set)\n", argv[1], lc, v, nv);
        close(fd); return 0;
    }
    if (!strcmp(argv[2], "hotreset")) {          /* secondary-bus (hot) reset of the downstream link */
        int fd = cfgopen(argv[1], 1);
        if (fd < 0) { perror("open"); return 1; }
        uint16_t bc = rw(fd, 0x3e);              /* Bridge Control (type-1 header) */
        uint16_t on = bc | (1u << 6);            /* Secondary Bus Reset (bit6) */
        if (pwrite(fd, &on, 2, 0x3e) != 2) { perror("pwrite"); return 1; }
        usleep(100000);                          /* hold >=1ms; 100ms generous */
        pwrite(fd, &bc, 2, 0x3e);                /* deassert -> link re-trains */
        usleep(100000);
        printf("%s BridgeCtrl@0x3e 0x%04x -> SBR -> 0x%04x (hot reset done)\n", argv[1], bc, bc);
        close(fd); return 0;
    }
    if (!strcmp(argv[2], "gen1")) {              /* pin target link speed to Gen1 (2.5GT/s) + retrain */
        int fd = cfgopen(argv[1], 1);
        if (fd < 0) { perror("open"); return 1; }
        unsigned cap = find_pcie_cap(fd);
        if (!cap) { fprintf(stderr, "no PCIe cap\n"); return 1; }
        unsigned l2 = cap + 0x30;                /* Link Control 2 */
        uint16_t v = rw(fd, l2), nv = (uint16_t)((v & ~0xf) | 1);
        pwrite(fd, &nv, 2, l2);
        unsigned lc = cap + 0x10;                /* Link Control: kick Retrain */
        uint16_t r = rw(fd, lc), nr = (uint16_t)(r | (1u << 5));
        pwrite(fd, &nr, 2, lc);
        printf("%s LnkCtl2@0x%02x 0x%04x -> 0x%04x (TargetSpeed=Gen1) + retrain\n", argv[1], l2, v, nv);
        close(fd); return 0;
    }
    unsigned long off = strtoul(argv[2], 0, 0);
    int fd = cfgopen(argv[1], argc >= 4);
    if (fd < 0) { perror("open"); return 1; }
    if (argc >= 4) {
        uint32_t v = (uint32_t)strtoul(argv[3], 0, 0);
        if (pwrite(fd, &v, 4, off) != 4) { perror("pwrite"); return 1; }
        printf("%s cfg[0x%lx] <= 0x%08x\n", argv[1], off, v);
    } else {
        printf("%s cfg[0x%lx] = 0x%08x\n", argv[1], off, rl(fd, off));
    }
    close(fd); return 0;
}
