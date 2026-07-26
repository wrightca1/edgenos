/*
 * fm6000_l2.c - FM6000 L2-forwarding bring-up for CPU punt (RX). Clean-room.
 *
 * See fm6000_l2.h for the pipeline overview. Programs the minimal
 * GLORT -> MCAST_DEST(DMASK) -> L2F subset so a CPU-injected special-delivery
 * frame is forwarded to the CPU port (physical/logical 0, the PCIe/DMA path) and
 * returns on the fpdma RX ring.
 *
 * Encodings + the CPU port(0)/GLORT(0xFF00) are RE'd facts from the FocalPoint
 * SDK/diag + the running-EOS golden capture; see
 * notes/analysis/phase39-cpu-punt-l2-decode.md and
 * reference/live-captures/7150-fm6000/eos-golden-2026-07-26-l2 (arista RE repo).
 * Register offsets are the FM6000_* macros in fm6000_regs.h, never the vendor hdr.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include "fm6000_l2.h"
#include "fm6000_regs.h"

void fm6000_l2_cpu_cfg_default(struct fm6000_l2_cpu_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->cpu_port       = FM6000_CPU_PORT;     /* 0 */
    cfg->cpu_glort      = FM6000_CPU_GLORT;    /* 0xFF00 */
    cfg->cam_idx        = 8;                    /* a free GLORT slot            */
    cfg->dmask_gid      = 16;                   /* a free MCAST_DEST slot       */
    cfg->src_port_first = 0;                    /* admit CPU-port dest for all  */
    cfg->src_port_last  = 75;                   /* source ports (76 total)      */
}

static void csr_set(struct fm6000_dev *dev, uint32_t word, uint32_t val,
                    const char *what)
{
    fprintf(stderr, "fm6000_l2: [%-10s] CSR[0x%05x] <= 0x%08x\n", what, word, val);
    fm6000_csr_write(dev, word, val);
}

int fm6000_l2_configure_cpu_loopback(struct fm6000_dev *dev,
                                     const struct fm6000_l2_cpu_cfg *cfg)
{
    unsigned dw, sp;
    uint32_t cpu_bit_word, cpu_bit_mask;

    if (!dev || !dev->bar0 || !cfg)
        return -1;
    if (cfg->cpu_port < 0 || cfg->cpu_port > 75) {
        fprintf(stderr, "fm6000_l2: bad cpu_port %d\n", cfg->cpu_port);
        return -1;
    }

    cpu_bit_word = FM6000_DEST_WORD((unsigned)cfg->cpu_port);
    cpu_bit_mask = FM6000_DEST_BIT((unsigned)cfg->cpu_port);

    /* Program deepest table first so the result is consistent before the GLORT
     * points at it (pipeline resolve order: GLORT -> DMASK -> L2F). */

    /* 1) DMASK: MCAST_DEST_TABLE[gid] = a 76-bit mask with only the CPU-port bit.
     *    4 words: word0..2 = DestMask[0:75], word3 pad. */
    for (dw = 0; dw < 4; dw++)
        csr_set(dev, FM6000_MCAST_DEST(cfg->dmask_gid, dw),
                (dw == cpu_bit_word) ? cpu_bit_mask : 0u, "dmask");

    /* 2) GLORT: CAM entry matches the CPU DGLORT exactly (KeyInvert=0); RAM points
     *    at the DMASK group (HashCmd=0 single-dest, DMaskRange=0). */
    csr_set(dev, FM6000_GLORT_CAM(cfg->cam_idx),
            FM6000_GLORT_CAM_ENC(cfg->cpu_glort, 0x0000), "glort_cam");
    csr_set(dev, FM6000_GLORT_RAM(cfg->cam_idx, 0),
            FM6000_GLORT_RAM_W0(0u, cfg->dmask_gid), "glort_ram0");
    csr_set(dev, FM6000_GLORT_RAM(cfg->cam_idx, 1), 0u, "glort_ram1");

    /* 3) L2F: admit the CPU-port bit in the per-source-port membership masks, and
     *    set a pass-through profile so the stage doesn't drop the frame. The mask
     *    value carries only the CPU-port bit (dest we allow); indexed by src port. */
    for (sp = cfg->src_port_first; sp <= cfg->src_port_last; sp++) {
        for (dw = 0; dw < 3; dw++)
            csr_set(dev, FM6000_L2F_TABLE_256(sp, dw),    /* i0 = source port */
                    (dw == cpu_bit_word) ? cpu_bit_mask : 0u, "l2f_memb");
        csr_set(dev, FM6000_L2F_PROFILE(sp), FM6000_L2F_PROFILE_PASS, "l2f_prof");
    }

    fprintf(stderr,
        "fm6000_l2: CPU loopback programmed — cpu_port=%d glort=0x%04x "
        "cam_idx=%u dmask_gid=%u (inject F64 ftype=SPECIAL dglort=0x%04x)\n",
        cfg->cpu_port, cfg->cpu_glort, cfg->cam_idx, cfg->dmask_gid, cfg->cpu_glort);
    return 0;
}

void fm6000_l2_dump_state(struct fm6000_dev *dev)
{
    unsigned i;
    if (!dev || !dev->bar0)
        return;
    fprintf(stderr, "== fm6000_l2 state (for golden diff) ==\n");
    for (i = 0; i < 8; i++)
        fprintf(stderr, "  GLORT_CAM[%u]  0x%05x = 0x%08x   RAM0 = 0x%08x\n",
                i, FM6000_GLORT_CAM(i), fm6000_csr_read(dev, FM6000_GLORT_CAM(i)),
                fm6000_csr_read(dev, FM6000_GLORT_RAM(i, 0)));
    for (i = 0; i < 4; i++)
        fprintf(stderr, "  MCAST_DEST[16].w%u 0x%05x = 0x%08x\n",
                i, FM6000_MCAST_DEST(16, i), fm6000_csr_read(dev, FM6000_MCAST_DEST(16, i)));
    fprintf(stderr, "  L2F_TABLE_256[0].w0 0x%05x = 0x%08x  PROFILE[0] 0x%05x = 0x%08x\n",
            FM6000_L2F_TABLE_256(0, 0), fm6000_csr_read(dev, FM6000_L2F_TABLE_256(0, 0)),
            FM6000_L2F_PROFILE(0), fm6000_csr_read(dev, FM6000_L2F_PROFILE(0)));
}
