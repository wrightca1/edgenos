/*
 * fm6000_sched.c - FM6000 SSCHED port-service-ring init via the TOKEN-INIT API.
 *
 * The port-0 (PCIe DMA) egress scheduler is uninitialized on a bare-M1 bring-up
 * (SSCHED ring empty), so frames forwarded to port 0 never drain to the DMA RX
 * ring (arista notes phase51/55). The ring MUST be programmed through the HW
 * token-init API + completion polls, NOT by writing SSCHED_TX_NEXT_PORT directly
 * (that is HW ring state; a direct write desyncs the internal linked-list and
 * hangs the fabric — boots 12/13/14). Procedure recovered from the decompiled
 * Alta platform layer + libFocalpointSDK.so (arista phase57):
 *   freelist init (poll _DONE) -> insert one TX + RX token per scheduled port
 *   (poll INIT_COMPLETE) -> the HW builds the ring consistently.
 *
 * Usage:  fm6000_sched [port ...]   (default: just port 0, the CPU/DMA port)
 * Reuses fm6000_hw. Run with the SCD watchdog armed (this touches the scheduler).
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>

#include "fm6000_hw.h"
#include "fm6000_regs.h"

/* Poll a completion bit (word bit0) up to ~100 ms. Returns 0 on set, -1 timeout. */
static int poll_done(struct fm6000_dev *dev, uint32_t word, const char *what)
{
    int i;
    for (i = 0; i < 1000; i++) {
        uint32_t v = fm6000_csr_read(dev, word);
        if (v == 0xFFFFFFFFu) {
            fprintf(stderr, "fm6000_sched: %s reads 0xffffffff (chip wedged)\n", what);
            return -1;
        }
        if (v & 1u)
            return 0;
        fm6000_delay_us(100);
    }
    fprintf(stderr, "fm6000_sched: %s did not complete (timeout)\n", what);
    return -1;
}

/* Init the four scheduler freelists (idempotent; boot-ctrl cmd3 may have done it,
 * but re-running is safe and each has a DONE handshake). */
static int freelist_init(struct fm6000_dev *dev)
{
    static const struct { uint32_t init, done; const char *name; } fl[] = {
        { FM6000_SSCHED_RXQ_FREELIST_INIT, FM6000_SSCHED_RXQ_FREELIST_DONE, "RXQ" },
        { FM6000_SSCHED_TXQ_FREELIST_INIT, FM6000_SSCHED_TXQ_FREELIST_DONE, "TXQ" },
        { FM6000_SSCHED_HS_FREELIST_INIT,  FM6000_SSCHED_HS_FREELIST_DONE,  "HS"  },
        { FM6000_SSCHED_FREELIST_INIT,     FM6000_SSCHED_FREELIST_DONE,     "GLOBAL" },
    };
    unsigned i;
    for (i = 0; i < sizeof(fl)/sizeof(fl[0]); i++) {
        fm6000_csr_write(dev, fl[i].init, 1u);          /* trigger */
        if (poll_done(dev, fl[i].done, fl[i].name) < 0)
            return -1;
        fprintf(stderr, "fm6000_sched: freelist %s init done\n", fl[i].name);
    }
    return 0;
}

/* Insert one token (a scheduled port) into the TX and RX service rings. */
static int add_port_token(struct fm6000_dev *dev, unsigned port)
{
    uint32_t tok = FM6000_SSCHED_TOKEN(port, 0u, 0u);   /* not locked, not synced */

    fm6000_csr_write(dev, FM6000_SSCHED_TX_INIT_TOKEN, tok);
    if (poll_done(dev, FM6000_SSCHED_TX_INIT_COMPLETE, "TX_INIT_COMPLETE") < 0)
        return -1;
    fm6000_csr_write(dev, FM6000_SSCHED_RX_INIT_TOKEN, tok);
    if (poll_done(dev, FM6000_SSCHED_RX_INIT_COMPLETE, "RX_INIT_COMPLETE") < 0)
        return -1;
    fprintf(stderr, "fm6000_sched: added TX+RX token for port %u\n", port);
    return 0;
}

int main(int argc, char **argv)
{
    struct fm6000_dev dev;
    int i, rc = 0;

    if (fm6000_hw_open(&dev) < 0) {
        fprintf(stderr, "fm6000_sched: cannot open FM6000 BAR0 (root? enumerated?)\n");
        return 1;
    }

    fprintf(stderr, "fm6000_sched: SSCHED before: TX_NEXT_PORT[0]=0x%08x (0=empty ring)\n",
            fm6000_csr_read(&dev, FM6000_SSCHED_TX_NEXT_PORT(0)));

    if (freelist_init(&dev) < 0) { rc = 1; goto out; }

    if (argc > 1) {
        for (i = 1; i < argc && rc == 0; i++)
            rc = add_port_token(&dev, (unsigned)strtoul(argv[i], NULL, 0)) < 0 ? 1 : 0;
    } else {
        rc = add_port_token(&dev, 0u) < 0 ? 1 : 0;      /* default: CPU/DMA port 0 */
    }

    fprintf(stderr, "fm6000_sched: SSCHED after:  TX_NEXT_PORT[0]=0x%08x  CAM_probe=0x%08x %s\n",
            fm6000_csr_read(&dev, FM6000_SSCHED_TX_NEXT_PORT(0)),
            fm6000_csr_read(&dev, FM6000_BLK_GLORT),
            rc ? "(FAILED)" : "(ok)");
out:
    fm6000_hw_close(&dev);
    return rc;
}
