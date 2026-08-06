/*
 * fm6000_bringup.c - standalone FM6000 bring-up / punt diagnostic
 *
 * Ties the FM6000 pieces together end to end:
 *   vfio open -> hw attach -> fm6000_boot_switch -> fpdma_init -> punt/inject.
 * Not part of the edged link (guarded); build as a tool to smoke-test bring-up
 * on the box once the device is bound to vfio-pci.
 *
 *   Usage: fm6000_bringup <pci-slot>        e.g. 0000:02:00.0
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <stdint.h>

#include "fm6000_boot.h"
#include "fpdma.h"
#include "fpdma_vfio.h"

static void on_rx(void *ctx, const void *data, uint16_t len)
{
    unsigned long *count = ctx;
    const uint8_t *p = data;
    (*count)++;
    fprintf(stderr, "  RX %u bytes: %02x %02x %02x %02x %02x %02x ...\n",
            len, p[0], p[1], p[2], p[3], p[4], p[5]);
}

int main(int argc, char **argv)
{
    const char *slot = (argc > 1) ? argv[1] : "0000:02:00.0";
    struct fpdma_vfio *vfio = NULL;
    struct fm6000_dev dev;
    struct fpdma fp;
    struct fpdma_backing back;
    volatile void *bar0;
    size_t bar0_size = 0;
    unsigned long rx_count = 0;
    int efd, i;

    fprintf(stderr, "== FM6000 bring-up on %s ==\n", slot);

    /* 1. VFIO: BAR0 + DMA pool + MSI. */
    if (fpdma_vfio_open(&vfio, slot, 8 * 1024 * 1024) < 0) {
        fprintf(stderr, "vfio open failed (bound to vfio-pci? IOMMU on?)\n");
        return 1;
    }
    bar0 = fpdma_vfio_bar0(vfio, &bar0_size);
    fm6000_hw_attach(&dev, bar0, bar0_size, slot);

    /* 2. Chip bring-up (BIST -> microcode -> SPICO -> ports). */
    if (fm6000_boot_switch(&dev) != 0) {
        fprintf(stderr, "bring-up failed\n");
        goto out;
    }

    /* 3. Packet DMA rings. */
    back = fpdma_vfio_backing(vfio);
    if (fpdma_init(&fp, &dev, &back, 256, 256) != 0) {
        fprintf(stderr, "fpdma_init failed\n");
        goto out;
    }

    /* 4. Inject one probe frame on the CPU port (caller-tagged F64 omitted here;
     * this is a DMA-path smoke test). */
    {
        uint8_t frame[64];
        memset(frame, 0, sizeof(frame));
        memset(frame, 0xff, 6);              /* broadcast dmac                   */
        frame[12] = 0x88; frame[13] = 0xb5;  /* local experimental ethertype     */
        if (fpdma_tx(&fp, frame, sizeof(frame)) == 0)
            fprintf(stderr, "TX: injected %zu-byte probe\n", sizeof(frame));
    }

    /* 5. RX poll loop (MSI-driven if available, else timed poll). */
    efd = fpdma_vfio_eventfd(vfio);
    for (i = 0; i < 50; i++) {
        if (efd >= 0) {
            struct pollfd pfd = { .fd = efd, .events = POLLIN };
            if (poll(&pfd, 1, 100) > 0) {
                uint64_t cnt;
                if (read(efd, &cnt, sizeof(cnt)) < 0) { /* drain */ }
            }
        } else {
            usleep(100000);
        }
        fpdma_rx_poll(&fp, 32, on_rx, &rx_count);
        fpdma_tx_reclaim(&fp);
    }
    fprintf(stderr, "done: %lu frames punted\n", rx_count);

    fpdma_shutdown(&fp);
out:
    fm6000_hw_close(&dev);
    fpdma_vfio_close(vfio);
    return 0;
}
