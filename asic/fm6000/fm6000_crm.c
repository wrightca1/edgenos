/*
 * fm6000_crm.c - FM6000 CRM "Memory Set" table initializer (datasheet step-12 §9 Table 9-3 cmd 0).
 *
 * Faithful port of fm6000CrmSetMemoryExt @0x35fc78: encodes a memory's DIMENSIONS (d1,d2,d3) + element
 * width into CRM_REGISTER BlockSize/Stride fields, then runs the EXACT SDK per-fill sequence
 * (COMMAND, REGISTER, PARAM, PERIOD=0, CTRL=1(Run only), poll STATUS.Running). No stop, no IP-clear,
 * no CRM_STATUS write, no readback. Slot 0.
 *
 * CRITICAL (phase91): width-3 memories (MOD/MCAST/L2F) are physically 4-word rows — the SDK bumps
 * width 3->4 so Size=3. A flat Size=2 desyncs the walk from the physical ECC word -> off-bus.
 *
 * CLI: fm6000_crm <base_word> <d1> [value] [width_words=1] [d2=0] [d3=0]
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "fm6000_hw.h"
#include "fm6000_regs.h"

/* floor(log2(x)); 0 for x<=1.  == fpsdk helper @0x35f7e0 */
static uint32_t fm_ilog2(uint32_t x){ uint32_t i=0; while (x > 1){ x >>= 1; i++; } return i; }

/* Faithful port of fm6000CrmSetMemoryExt field-packing. elemStride/stride1/stride2 are the memory's
 * PHYSICAL word geometry. Returns 0 ok; 2/3 = SDK arg-validation errors. */
static int fm6000_crm_encode(uint32_t base, uint32_t width,
                             uint32_t d1, uint32_t d2, uint32_t d3,
                             uint32_t elemStride, uint32_t stride1, uint32_t stride2,
                             uint64_t *reg, uint64_t *cmd)
{
    uint32_t N, bs1 = 0xF, st1 = 0, bs2 = 0xF, st2 = 0;   /* entry defaults @0x35fc9d */

    if (width == 0 || d1 == 0 || elemStride == 0) return 2;
    if (d2 == 0 && d3 != 0) return 2;

    if (d2 == 0) {                                        /* 1D @0x35fdee */
        N = d1;
        if (width == 3 && elemStride == 4) width = 4;     /* @0x35fe00 */
        if (elemStride != width) return 3;
        /* bs1=0xF,st1=0,bs2=0xF,st2=0 kept */
    } else if (d3 == 0) {                                 /* 2D @0x35fe9c */
        if (width == 3 && elemStride == 4) width = 4;
        if (elemStride != width) return 3;
        N   = d1 * d2;
        bs1 = fm_ilog2(d1);
        st1 = fm_ilog2(stride1);
        if ((1u << bs1) != d1) {                          /* d1 not pow2 */
            uint32_t P = 1u << (bs1 + 1);
            if (P * width != stride1) return 3;
            bs1 = bs1 + 1;
            N   = P * d2 - stride1 + d1;
        }
    } else {                                              /* 3D @0x360019 */
        N   = d1 * d2 * d3;
        bs1 = fm_ilog2(d1);
        st1 = fm_ilog2(stride1);
        bs2 = fm_ilog2(d2);
        st2 = fm_ilog2(stride2);
        if ((1u << bs1) != d1) bs1++;
    }

    *reg = ((uint64_t)(base & 0x3FFFFF))
         | ((uint64_t)((width - 1) & 0x3) << 22)          /* Size */
         | ((uint64_t)(bs1 & 0xF) << 24)
         | ((uint64_t)(st1 & 0xF) << 28)
         | ((uint64_t)(bs2 & 0xF) << 32)
         | ((uint64_t)(st2 & 0xF) << 36);
    *cmd = ((uint64_t)(N & 0xFFFFF)) << 14;               /* Count[33:14]; Cmd=0, DataIdx=0 */
    return 0;
}

/* Encode (contiguous geometry) + run the SDK per-fill sequence on slot 0. */
int fm6000_crm_fill(struct fm6000_dev *dev, uint32_t base, uint32_t width,
                    uint32_t d1, uint32_t d2, uint32_t d3, uint32_t value)
{
    uint64_t reg, cmd;
    uint32_t es = (width == 3) ? 4 : width;               /* physical element stride (words) */
    uint32_t s1 = d1 * es;
    uint32_t s2 = d2 ? d2 * s1 : 0;
    int i, rc;

    rc = fm6000_crm_encode(base, width, d1, d2, d3, es, s1, s2, &reg, &cmd);
    if (rc) { fprintf(stderr, "fm6000_crm: encode error %d (base=0x%05x w=%u d1=0x%x)\n", rc, base, width, d1); return -1; }

    fm6000_csr_write(dev, FM6000_CRM_COMMAND(0, 0), (uint32_t)cmd);
    fm6000_csr_write(dev, FM6000_CRM_COMMAND(0, 1), (uint32_t)(cmd >> 32));
    fm6000_csr_write(dev, FM6000_CRM_REGISTER(0, 0), (uint32_t)reg);
    fm6000_csr_write(dev, FM6000_CRM_REGISTER(0, 1), (uint32_t)(reg >> 32));
    fm6000_csr_write(dev, FM6000_CRM_PARAM(0), value);
    fm6000_csr_write(dev, FM6000_CRM_PERIOD(0, 0), 0);
    fm6000_csr_write(dev, FM6000_CRM_PERIOD(0, 1), 0);
    fm6000_csr_write(dev, FM6000_CRM_CTRL, FM6000_CRM_CTRL_RUN);   /* Run only, First=Last=Cont=0 */
    for (i = 0; i < 20000; i++) {
        if (!(fm6000_csr_read(dev, FM6000_CRM_STATUS) & FM6000_CRM_STATUS_RUNNING)) break;
        fm6000_delay_us(100);
    }
    if (fm6000_csr_read(dev, FM6000_CRM_STATUS) & FM6000_CRM_STATUS_RUNNING) {
        fprintf(stderr, "fm6000_crm: base=0x%05x did NOT complete (Running stuck)\n", base);
        return -1;
    }
    fprintf(stderr, "fm6000_crm: base=0x%05x w=%u d1=0x%x d2=0x%x reg=0x%08x_%08x cmd=0x%08x_%08x DONE\n",
            base, width, d1, d2, (uint32_t)(reg >> 32), (uint32_t)reg,
            (uint32_t)(cmd >> 32), (uint32_t)cmd);
    return 0;
}

#ifndef FM6000_CRM_NO_MAIN
int main(int argc, char **argv)
{
    struct fm6000_dev dev;
    int rc;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <base_word> <d1> [value] [width_words=1] [d2] [d3]\n", argv[0]);
        return 2;
    }
    uint32_t base  = (uint32_t)strtoul(argv[1], NULL, 0);
    uint32_t d1    = (uint32_t)strtoul(argv[2], NULL, 0);
    uint32_t value = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0u;
    uint32_t width = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : 1u;
    uint32_t d2    = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 0) : 0u;
    uint32_t d3    = argc > 6 ? (uint32_t)strtoul(argv[6], NULL, 0) : 0u;

    if (fm6000_hw_open(&dev) < 0) {
        fprintf(stderr, "fm6000_crm: cannot open FM6000 BAR0 (root? enumerated?)\n");
        return 1;
    }
    rc = fm6000_crm_fill(&dev, base, width, d1, d2, d3, value);
    fm6000_hw_close(&dev);
    return rc ? 1 : 0;
}
#endif
