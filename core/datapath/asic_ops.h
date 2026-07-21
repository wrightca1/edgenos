/*
 * asic_ops.h - EdgeNOS ASIC backend seam
 *
 * The datapath daemon was originally written straight against the Broadcom
 * OpenMDK path (bde_open/cdk_init/bmd_* in edged.c + BMD-coupled l2/l3/vlan).
 * The FM6000 (FocalPoint) shares none of that SDK, so this header introduces a
 * thin backend seam: a table of function pointers a board's edged binds to.
 *
 * This is ADDITIVE — it does not change the existing BCM path. The Broadcom
 * boards keep calling their bde/bmd functions directly; the 7150 (asic/fm6000)
 * provides a struct asic_ops and its board main drives the switch through it.
 * A later cleanup can retrofit bcm56846 behind the same seam and unify the two
 * daemons; until then this is the FM6000 integration point.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __ASIC_OPS_H__
#define __ASIC_OPS_H__

#include <stdint.h>

/* RX delivery callback: one punted frame (untagged payload after any CPU tag
 * has been stripped by the backend). */
typedef void (*asic_rx_cb)(void *ctx, const void *frame, uint16_t len);

struct asic_ops {
    const char *name;

    /* Full bring-up to a CPU-punt-capable state (chip init + DMA rings up). */
    int  (*init)(void);

    /* Enable/disable a front-panel port and set its speed (Mb/s). */
    int  (*port_set)(int port, int enable, int speed_mb);

    /* Inject one frame toward the fabric/CPU port. The caller supplies a fully
     * formed L2 frame; the backend adds any ASIC/CPU tag it needs. */
    int  (*tx)(const void *frame, uint16_t len);

    /* Poll for up to `budget` punted frames; invoke cb() for each. Returns the
     * number delivered. */
    int  (*rx_poll)(int budget, asic_rx_cb cb, void *ctx);

    /* An fd that becomes readable on RX/TX completion (MSI eventfd), or -1 if
     * the backend is poll-only. Lets the daemon block instead of spin. */
    int  (*intr_fd)(void);

    /* Tear down (stop DMA, unmap, release the device). */
    void (*shutdown)(void);
};

#endif /* __ASIC_OPS_H__ */
