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
    /* MINIMAL by design: a CPU-injected frame's ingress source port IS the CPU
     * port, and L2F is indexed by source port — so we only need the CPU port's
     * one membership entry, NOT all 76. Blasting all 76 unpaced wedged the chip
     * (table-commit backpressure); see arista memory fm6000-bringup-safety. */
    cfg->src_port_first = FM6000_CPU_PORT;
    cfg->src_port_last  = FM6000_CPU_PORT;
}

/* Write ONE atomic table entry: n contiguous words, LSW (word 0) first, MSW
 * (word n-1) last. Datasheet §8.3: FM6000 tables wider than 32 bits are atomic —
 * the hardware caches the LSWs and commits the whole entry only when the MSW is
 * written, and there is ONE temp cache per bus master. So we must NOT read (or
 * write another table) between the words of an entry — an interleaved access
 * corrupts the pending write. We verify + wedge-check AFTER the entry commits. */
static int write_entry(struct fm6000_dev *dev, uint32_t base, unsigned n,
                       const uint32_t *vals, const char *what)
{
    unsigned i;
    uint32_t rb;
    for (i = 0; i < n; i++) {
        fprintf(stderr, "fm6000_l2: [%-10s] CSR[0x%05x] <= 0x%08x\n",
                what, base + i, vals[i]);
        fm6000_csr_write(dev, base + i, vals[i]);   /* LSW..MSW, no reads between */
    }
    fm6000_delay_us(200);
    rb = fm6000_csr_read(dev, base);                /* safe: entry committed now  */
    if (rb == 0xFFFFFFFFu) {
        fprintf(stderr, "fm6000_l2: *** CSR[0x%05x] reads 0xffffffff after entry "
                        "write — chip wedged/off-bus, ABORTING ***\n", base);
        return -1;
    }
    return 0;
}

int fm6000_l2_configure_cpu_loopback(struct fm6000_dev *dev,
                                     const struct fm6000_l2_cpu_cfg *cfg)
{
    uint32_t cpu_bit_word, cpu_bit_mask, dmask[3], gram[2];

    if (!dev || !dev->bar0 || !cfg)
        return -1;
    if (cfg->cpu_port < 0 || cfg->cpu_port > 75) {
        fprintf(stderr, "fm6000_l2: bad cpu_port %d\n", cfg->cpu_port);
        return -1;
    }

    cpu_bit_word = FM6000_DEST_WORD((unsigned)cfg->cpu_port);
    cpu_bit_mask = FM6000_DEST_BIT((unsigned)cfg->cpu_port);

#define ENTRY(base, n, tag, ...) do { \
        const uint32_t _v[] = { __VA_ARGS__ }; \
        if (write_entry(dev, (base), (n), _v, (tag)) < 0) return -1; \
    } while (0)

    /* Program the DMASK first, then the GLORT that points at it (pipeline resolve
     * order GLORT -> DMASK -> L2F). Each entry is written atomically (LSW->MSW). */

    /* 1) DMASK: L2F_TABLE_256[gid] = 76-bit dest mask, CPU-port bit only. 3 words.
     *    GLORT_RAM.DMaskBaseIdx indexes THIS table (golden: DMaskBaseIdx=1 ->
     *    L2F_TABLE_256[1]={CPU bit0, Et1 bit40}). NOT MCAST_DEST (that wedges even
     *    post-BIST — its RAM isn't parity-initialised; datasheet §step-12/CRM). */
    dmask[0] = (cpu_bit_word == 0) ? cpu_bit_mask : 0u;
    dmask[1] = (cpu_bit_word == 1) ? cpu_bit_mask : 0u;
    dmask[2] = (cpu_bit_word == 2) ? cpu_bit_mask : 0u;
    ENTRY(FM6000_L2F_TABLE_256(cfg->dmask_gid, 0), 3, "dmask",
          dmask[0], dmask[1], dmask[2]);

    /* 2) GLORT_CAM: match the CPU DGLORT exactly (1-word entry). */
    ENTRY(FM6000_GLORT_CAM(cfg->cam_idx), 1, "glort_cam",
          FM6000_GLORT_CAM_ENC(cfg->cpu_glort, 0x0000));

    /* 3) GLORT_RAM: HashCmd=0 single-dest, DMaskRange=0, DMaskBaseIdx=gid. 2-word
     *    atomic entry (word0 LSW = the DMaskBaseIdx field, word1 MSW commits). */
    gram[0] = FM6000_GLORT_RAM_W0(0u, cfg->dmask_gid);
    gram[1] = 0u;
    ENTRY(FM6000_GLORT_RAM(cfg->cam_idx, 0), 2, "glort_ram", gram[0], gram[1]);

    /* L2F 13-stage membership/profile (VLAN/STP/src-port) deferred: test whether
     * the frame punts with just GLORT->DMASK; if the L2F filter prunes it, add
     * L2F_TABLE_4K membership + a pass-through profile (datasheet §5.15). */
#undef ENTRY

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
