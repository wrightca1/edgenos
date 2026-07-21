/*
 * fm6000_hw.c - FM6000 PCIe bind + register access (userspace, x86_64)
 *
 * Locates the FM6000 endpoint via sysfs and mmaps its BAR0. This replaces the
 * kernel-BDE register path used on the PPC Broadcom box: x86 MMIO needs no
 * ordering barriers, so a direct mmap of resource0 is correct and simplest.
 * (The packet-DMA engine still needs kernel help — see fpdma.c.)
 *
 * Bind sequence mirrors fpdma's alta_probe (edgenos/FPDMA.md):
 *   enable device -> request BAR0 region -> iomap -> set master -> 32-bit mask.
 * Here, "enable + set master" are done by the kernel when fpdma.ko binds; this
 * userspace side only needs the BAR0 mapping for CSR I/O.
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
#include <dirent.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "fm6000_hw.h"

#define SYSFS_PCI "/sys/bus/pci/devices"

/* Read a small hex sysfs attribute (e.g. "vendor", "device") into *out. */
static int read_hex_attr(const char *slotdir, const char *attr, unsigned *out)
{
    char path[512], buf[32];
    int fd, n;

    snprintf(path, sizeof(path), "%s/%s", slotdir, attr);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return (sscanf(buf, "%x", out) == 1) ? 0 : -1;
}

/* Match Intel 8086:155b or the pre-acquisition Fulcrum 1823:1770. */
static int is_fm6000(unsigned vendor, unsigned device)
{
    return (vendor == FM6000_VENDOR_INTEL   && device == FM6000_DEVICE_ALTA) ||
           (vendor == FM6000_VENDOR_FULCRUM && device == FM6000_DEVICE_FULCRUM);
}

/* Walk /sys/bus/pci/devices; fill dev->pci_slot with the first FM6000 found. */
static int find_fm6000_slot(struct fm6000_dev *dev)
{
    DIR *d = opendir(SYSFS_PCI);
    struct dirent *e;
    int found = -1;

    if (!d) {
        fprintf(stderr, "fm6000: cannot open %s: %s\n", SYSFS_PCI, strerror(errno));
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        char slotdir[512];
        unsigned vendor = 0, device = 0;

        if (e->d_name[0] == '.')
            continue;
        /* A PCI slot name ("dddd:bb:dd.f") is at most 12 chars; anything longer
         * isn't a PCI device dir — skip it (also keeps pci_slot copy bounded). */
        if (strlen(e->d_name) >= sizeof(dev->pci_slot))
            continue;
        snprintf(slotdir, sizeof(slotdir), "%s/%s", SYSFS_PCI, e->d_name);
        if (read_hex_attr(slotdir, "vendor", &vendor) < 0 ||
            read_hex_attr(slotdir, "device", &device) < 0)
            continue;
        if (is_fm6000(vendor, device)) {
            memcpy(dev->pci_slot, e->d_name, strlen(e->d_name) + 1);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

int fm6000_hw_open(struct fm6000_dev *dev)
{
    char path[512];
    struct stat st;

    memset(dev, 0, sizeof(*dev));
    dev->resource_fd = -1;

    if (find_fm6000_slot(dev) < 0) {
        fprintf(stderr, "fm6000: no FM6000 endpoint (8086:155b) found\n");
        return -1;
    }

    /* BAR0 == resource0. Size comes from the sysfs node's length. */
    snprintf(path, sizeof(path), "%s/%s/resource0", SYSFS_PCI, dev->pci_slot);
    dev->resource_fd = open(path, O_RDWR | O_SYNC);
    if (dev->resource_fd < 0) {
        fprintf(stderr, "fm6000: open %s: %s (need CAP_SYS_RAWIO/root)\n",
                path, strerror(errno));
        return -1;
    }
    if (fstat(dev->resource_fd, &st) < 0) {
        fprintf(stderr, "fm6000: fstat resource0: %s\n", strerror(errno));
        goto err;
    }
    dev->bar0_size = (size_t)st.st_size;

    dev->bar0 = mmap(NULL, dev->bar0_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, dev->resource_fd, 0);
    if (dev->bar0 == MAP_FAILED) {
        fprintf(stderr, "fm6000: mmap BAR0 (%zu bytes): %s\n",
                dev->bar0_size, strerror(errno));
        dev->bar0 = NULL;
        goto err;
    }

    dev->owns_map = 1;
    fprintf(stderr, "fm6000: bound %s, BAR0 %zu KiB @ %p\n",
            dev->pci_slot, dev->bar0_size / 1024, (void *)dev->bar0);
    return 0;

err:
    close(dev->resource_fd);
    dev->resource_fd = -1;
    return -1;
}

void fm6000_hw_attach(struct fm6000_dev *dev, volatile void *bar0,
                      size_t size, const char *slot)
{
    memset(dev, 0, sizeof(*dev));
    dev->resource_fd = -1;
    dev->bar0        = bar0;
    dev->bar0_size   = size;
    dev->owns_map    = 0;             /* VFIO owns the mapping */
    if (slot)
        snprintf(dev->pci_slot, sizeof(dev->pci_slot), "%s", slot);
}

void fm6000_hw_close(struct fm6000_dev *dev)
{
    if (dev->owns_map && dev->bar0 && dev->bar0 != MAP_FAILED)
        munmap((void *)dev->bar0, dev->bar0_size);
    if (dev->resource_fd >= 0)
        close(dev->resource_fd);
    dev->bar0 = NULL;
    dev->resource_fd = -1;
}

int fm6000_csr_poll(struct fm6000_dev *dev, uint32_t word_idx,
                    uint32_t mask, uint32_t want, unsigned timeout_us)
{
    unsigned waited = 0;

    for (;;) {
        if ((fm6000_csr_read(dev, word_idx) & mask) == want)
            return 0;
        if (waited >= timeout_us)
            return -1;
        fm6000_delay_us(10);
        waited += 10;
    }
}

void fm6000_delay_us(unsigned usec)
{
    struct timespec ts = {
        .tv_sec  = usec / 1000000u,
        .tv_nsec = (long)(usec % 1000000u) * 1000L,
    };
    nanosleep(&ts, NULL);
}
