/*
 * pcicfg.c - read/write PCI config space via sysfs (M1 has no setpci/python).
 *   pcicfg <BDF> <off>          # read dword  (e.g. pcicfg 0000:00:04.0 0x68)
 *   pcicfg <BDF> <off> <val>    # write dword
 *   pcicfg <BDF> linkctl        # find PCIe cap, print Link Control offset+value+LinkDisable bit
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
