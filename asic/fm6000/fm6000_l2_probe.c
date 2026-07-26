/*
 * fm6000_l2_probe.c - standalone: program the CPU-punt L2 forwarding on the 7150.
 *
 * Opens the FM6000 BAR0 directly (root resource0 mmap; register-only, no kmod),
 * programs the minimal GLORT -> MCAST_DEST(DMASK) -> L2F so a CPU-injected
 * special-delivery frame (DGLORT=0xFF00) is forwarded to the CPU port. Run this
 * AFTER the L2-pipeline microcode is loaded and MSB released, then run
 * `fpdma_probe tx 0xff00` to inject + poll the RX ring ("moves a byte").
 *
 * Prereqs on the box (M1): FM6000 enumerated, microcode loaded (fm6000-up.sh).
 * Non-destructive to the board (touches only forwarding-table CSRs).
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fm6000_hw.h"
#include "fm6000_l2.h"

int main(int argc, char **argv)
{
    struct fm6000_dev dev;
    struct fm6000_l2_cpu_cfg cfg;

    if (fm6000_hw_open(&dev) < 0) {
        fprintf(stderr, "fm6000_l2_probe: cannot open FM6000 BAR0 "
                        "(root? enumerated? resource0 present?)\n");
        return 1;
    }
    fprintf(stderr, "fm6000_l2_probe: BAR0 @ %s (%zu bytes)\n",
            dev.pci_slot, dev.bar0_size);

    fm6000_l2_cpu_cfg_default(&cfg);
    /* Optional overrides: argv[1]=cpu_port argv[2]=cam_idx argv[3]=dmask_gid */
    if (argc > 1) cfg.cpu_port  = (int)strtol(argv[1], NULL, 0);
    if (argc > 2) cfg.cam_idx   = (unsigned)strtoul(argv[2], NULL, 0);
    if (argc > 3) cfg.dmask_gid = (unsigned)strtoul(argv[3], NULL, 0);

    fprintf(stderr, "== state BEFORE ==\n");
    fm6000_l2_dump_state(&dev);

    if (fm6000_l2_configure_cpu_loopback(&dev, &cfg) < 0) {
        fprintf(stderr, "fm6000_l2_probe: configure FAILED\n");
        fm6000_hw_close(&dev);
        return 1;
    }

    fprintf(stderr, "== state AFTER ==\n");
    fm6000_l2_dump_state(&dev);

    fm6000_hw_close(&dev);
    fprintf(stderr, "fm6000_l2_probe: done — now run `fpdma_probe tx 0x%04x`\n",
            cfg.cpu_glort);
    return 0;
}
