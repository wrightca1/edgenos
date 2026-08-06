/*
 * fm6000_edged.c - FM6000 backend implementing struct asic_ops
 *
 * Binds the FM6000 pieces into the datapath seam:
 *   init      : VFIO open -> hw attach -> fm6000_boot_switch -> fpdma_init
 *   tx        : fpdma_tx  (caller supplies the frame; F64/CPU tag TODO)
 *   rx_poll   : fpdma_rx_poll
 *   intr_fd   : the VFIO MSI eventfd
 *   port_set  : EPL/SerDes port enable  (TODO(live-trace): SerDes tuning values)
 *   shutdown  : reverse teardown
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fm6000_edged.h"
#include "fm6000_boot.h"
#include "fpdma.h"
#include "fpdma_kmod.h"
#include "fpdma_vfio.h"

#define FM6000_DEFAULT_SLOT   "0000:02:00.0"
#define FM6000_DMA_POOL       (8 * 1024 * 1024)
#define FM6000_TX_RING        256
#define FM6000_RX_RING        256

enum backend { BE_NONE, BE_KMOD, BE_VFIO };

/* Single ASIC per box: file-static context. */
static struct {
    enum backend       be;
    struct fpdma_kmod *kmod;
    struct fpdma_vfio *vfio;
    struct fm6000_dev  dev;
    struct fpdma       fp;
    int                up;
} g;

/* Bring up a DMA backend + attach BAR0, filling *back. Prefers the
 * kmod (works without an IOMMU — the 7150's case); falls back to VFIO on
 * IOMMU-capable boxes. */
static int backend_open(struct fpdma_backing *back)
{
    const char *slot = getenv("EDGENOS_FM6000_SLOT");
    volatile void *bar0;
    size_t bar0_size = 0;

    if (!slot || !*slot)
        slot = FM6000_DEFAULT_SLOT;

    if (fpdma_kmod_open(&g.kmod) == 0) {
        g.be  = BE_KMOD;
        bar0  = fpdma_kmod_bar0(g.kmod, &bar0_size);
        *back = fpdma_kmod_backing(g.kmod);
        fprintf(stderr, "fm6000: DMA backend = kmod (/dev/fm6000dma)\n");
    } else if (fpdma_vfio_open(&g.vfio, slot, FM6000_DMA_POOL) == 0) {
        g.be  = BE_VFIO;
        bar0  = fpdma_vfio_bar0(g.vfio, &bar0_size);
        *back = fpdma_vfio_backing(g.vfio);
        fprintf(stderr, "fm6000: DMA backend = vfio (%s)\n", slot);
    } else {
        fprintf(stderr, "fm6000: no DMA backend (load fm6000dma.ko, or bind vfio-pci)\n");
        return -1;
    }
    fm6000_hw_attach(&g.dev, bar0, bar0_size, slot);
    return 0;
}

static void backend_close(void)
{
    if (g.be == BE_KMOD) { fpdma_kmod_close(g.kmod); g.kmod = NULL; }
    else if (g.be == BE_VFIO) { fpdma_vfio_close(g.vfio); g.vfio = NULL; }
    g.be = BE_NONE;
}

static int fm6000_be_init(void)
{
    struct fpdma_backing back;

    if (backend_open(&back) < 0)
        return -1;

    if (fm6000_boot_switch(&g.dev) != 0) {
        fprintf(stderr, "fm6000: bring-up failed\n");
        goto fail;
    }
    if (fpdma_init(&g.fp, &g.dev, &back, FM6000_TX_RING, FM6000_RX_RING) != 0) {
        fprintf(stderr, "fm6000: fpdma_init failed\n");
        goto fail;
    }

    g.up = 1;
    return 0;

fail:
    fm6000_hw_close(&g.dev);
    backend_close();
    return -1;
}

static int fm6000_be_port_set(int port, int enable, int speed_mb)
{
    /* TODO(live-trace): program the EPL/PCS/MAC + SerDes for this port. The
     * register blocks (EPL 0xE0000, SerDes SBus) are mapped; the AN/DFE tuning
     * *values* are runtime-computed and need a live trace (GAPS.md A). */
    fprintf(stderr, "fm6000: port_set(%d, %s, %dMb) - stub (SerDes tuning TBD)\n",
            port, enable ? "up" : "down", speed_mb);
    return 0;
}

static int fm6000_be_tx(const void *frame, uint16_t len)
{
    if (!g.up)
        return -1;
    /* NOTE: the F64/ISL CPU tag (DGLORT/SGLORT/FTYPE/SWPRI/VLAN at L2 offset 12)
     * must be prepended before this call for directed injection; see FPDMA.md
     * fpdma_f64. Untagged frames flood per the default VLAN. */
    return fpdma_tx(&g.fp, frame, len);
}

static int fm6000_be_rx_poll(int budget, asic_rx_cb cb, void *ctx)
{
    if (!g.up)
        return 0;
    fpdma_tx_reclaim(&g.fp);
    return fpdma_rx_poll(&g.fp, budget, cb, ctx);
}

static int fm6000_be_intr_fd(void)
{
    if (g.be == BE_KMOD) return fpdma_kmod_eventfd(g.kmod);
    if (g.be == BE_VFIO) return fpdma_vfio_eventfd(g.vfio);
    return -1;
}

static void fm6000_be_shutdown(void)
{
    if (g.up) {
        fpdma_shutdown(&g.fp);
        g.up = 0;
    }
    if (g.be != BE_NONE) {
        fm6000_hw_close(&g.dev);
        backend_close();
    }
}

static const struct asic_ops fm6000_ops = {
    .name     = "fm6000",
    .init     = fm6000_be_init,
    .port_set = fm6000_be_port_set,
    .tx       = fm6000_be_tx,
    .rx_poll  = fm6000_be_rx_poll,
    .intr_fd  = fm6000_be_intr_fd,
    .shutdown = fm6000_be_shutdown,
};

const struct asic_ops *fm6000_asic_ops(void)
{
    return &fm6000_ops;
}
