/*
 * fm6000_crm.c - FM6000 CRM (Counter Rate Monitor) "Memory Set" table initializer.
 *
 * The datasheet step-12 "Initialize Memory" (§9, Table 9-3 command 0): the CRM
 * walks a register/table block and writes a value into every element WITH
 * HW-computed parity. This is what makes the forwarding tables the microcode
 * never touches (GLORT_CAM/RAM, L2F_256, the L2F 13-stage association tables)
 * parity-valid, so a pipeline lookup that reads them does not fault (the class
 * of wedge we hit programming GLORT/DMASK post-BIST — see arista notes phase45).
 *
 * BIST marches the *physical* SRAM; the CRM Memory-Set initializes the *logical*
 * table contents. Both are needed before software programs specific entries.
 *
 * Standalone (register-only BAR0 mmap, no kmod). Reusable core is
 * fm6000_crm_memory_set(); main() drives it from the CLI / the M1 bring-up.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fm6000_hw.h"
#include "fm6000_regs.h"

/* Fill `count` 32-bit registers starting at BAR0 word `base` with `value` (+HW
 * parity) using CRM command slot `slot`. `size` is FM6000_CRM_REG_SIZE_* (use
 * _32 and a word-granular count for a raw linear fill). Contiguous walk: all
 * block/stride shifts = 0 (block=1, stride=1). Returns 0 on success, -1 on
 * timeout/wedge. Leaves the CRM stopped. */
int fm6000_crm_memory_set(struct fm6000_dev *dev, unsigned slot,
                          uint32_t base, uint32_t count, unsigned size,
                          uint32_t value)
{
    uint32_t rb;
    int i;

    if (!dev || !dev->bar0 || slot >= FM6000_CRM_COMMAND_ENTRIES || count == 0)
        return -1;

    /* 1. Stop the CRM and wait for it to actually stop (it only stops at a
     *    command boundary). */
    fm6000_csr_write(dev, FM6000_CRM_CTRL, 0);
    for (i = 0; i < 1000; i++) {
        if (!(fm6000_csr_read(dev, FM6000_CRM_STATUS) & FM6000_CRM_STATUS_RUNNING))
            break;
        fm6000_delay_us(100);
    }
    if (fm6000_csr_read(dev, FM6000_CRM_STATUS) & FM6000_CRM_STATUS_RUNNING) {
        fprintf(stderr, "fm6000_crm: CRM would not stop\n");
        return -1;
    }

    /* 2. Program the command's REGISTER (target block), PARAM (fill value),
     *    PERIOD (0 = as fast as possible) and COMMAND (Memory Set + count).
     *    64-bit regs are w0=LSW then w1=MSW.
     *
     *    Block/Stride define a 2D address walk: write BlockSize1 contiguous regs,
     *    then jump Stride1 to the next block, BlockSize2 times, jump Stride2. For a
     *    CONTIGUOUS linear fill we need ONE block covering the whole count — else a
     *    block=1/stride=1 walk writes every OTHER register (verified live: odd
     *    offsets left untouched). So set BlockSize1Shift so (1<<bs1) >= count. */
    unsigned bs1 = 0;
    while ((1u << bs1) < count && bs1 < 15u) bs1++;
    fm6000_csr_write(dev, FM6000_CRM_REGISTER(slot, 0),
                     FM6000_CRM_REGISTER_W0(base, size, bs1, 0));
    fm6000_csr_write(dev, FM6000_CRM_REGISTER(slot, 1),
                     FM6000_CRM_REGISTER_W1(0, 0));
    fm6000_csr_write(dev, FM6000_CRM_PARAM(slot), value);
    fm6000_csr_write(dev, FM6000_CRM_PERIOD(slot, 0), 0);
    fm6000_csr_write(dev, FM6000_CRM_PERIOD(slot, 1), 0);
    fm6000_csr_write(dev, FM6000_CRM_COMMAND(slot, 0),
                     FM6000_CRM_COMMAND_W0(FM6000_CRM_CMD_MEMORY_SET, 0, count));
    fm6000_csr_write(dev, FM6000_CRM_COMMAND(slot, 1),
                     FM6000_CRM_COMMAND_W1(count));

    /* 3. Clear this command's interrupt-pending bit (W1C) so we can poll it. */
    fm6000_csr_write(dev, FM6000_CRM_IP(slot >> 5), 1u << (slot & 31));

    /* 4. Point the sequencer at our slot, then run once (First=Last=slot,
     *    Continuous=0 so Run self-clears at completion). */
    fm6000_csr_write(dev, FM6000_CRM_STATUS, (uint32_t)(slot & 0x3F) << 1);
    fm6000_csr_write(dev, FM6000_CRM_CTRL,
                     FM6000_CRM_CTRL_RUN | FM6000_CRM_CTRL_FIRST(slot) |
                     FM6000_CRM_CTRL_LAST(slot));

    /* 5. Wait for completion: Run self-clears (once-mode) and IP[slot] sets.
     *    Scale the timeout with count (the 8x4K L2F block is ~128K registers). */
    for (i = 0; i < 20000; i++) {
        if (!(fm6000_csr_read(dev, FM6000_CRM_CTRL) & FM6000_CRM_CTRL_RUN))
            break;
        fm6000_delay_us(100);
    }
    if (fm6000_csr_read(dev, FM6000_CRM_CTRL) & FM6000_CRM_CTRL_RUN) {
        fprintf(stderr, "fm6000_crm: Memory Set base=0x%05x count=%u did not "
                        "complete (Run still set)\n", base, count);
        fm6000_csr_write(dev, FM6000_CRM_CTRL, 0);
        return -1;
    }

    /* 6. Sanity: first + last element should read back the value. (A CSR read
     *    of a now-parity-valid location no longer faults.) */
    rb = fm6000_csr_read(dev, base);
    if (rb == 0xFFFFFFFFu) {
        fprintf(stderr, "fm6000_crm: base 0x%05x reads 0xffffffff after set "
                        "(chip off-bus/wedged)\n", base);
        return -1;
    }
    fprintf(stderr, "fm6000_crm: MemSet base=0x%05x count=%u val=0x%08x  "
                    "(IP=0x%08x first=0x%08x last=0x%08x)\n",
            base, count, value, fm6000_csr_read(dev, FM6000_CRM_IP(slot >> 5)),
            rb, fm6000_csr_read(dev, base + count - 1));
    return 0;
}

#ifndef FM6000_CRM_NO_MAIN
static const struct { const char *name; uint32_t base, count; } TABLES[] = {
    /* Every forwarding table the microcode leaves unconfigured (parity-invalid)
     * that a GLORT->DMASK->L2F 13-stage lookup reads. Word-granular linear fills.
     * The 13-stage reads the 4K tables (VLAN membership / STP / SRC_PORT) AND all
     * four 256 tables — not just the first — so init the whole L2F region, else a
     * stage reads uninit'd memory and (best case) AND-s the DMASK to 0 = drop. */
    { "GLORT_CAM",  0x0E000u,  1024u },     /* 1024 x 1w (Key,KeyInvert)        */
    { "GLORT_RAM",  0x0E800u,  2048u },     /* 1024 x 2w                        */
    { "LBS",        0x14000u,  0x1000u },   /* loopback-suppress CAM(76)+profile
                                             * — BIST leaves it GARBAGE; the LBS
                                             * 13-stage stage reads it and mangles
                                             * the DMASK (CPU-punt RX blocker).  */
    { "L2F_4K",     0x180000u, 0x20000u },  /* 8 x 4096 x 4w  (membership/STP)  */
    { "L2F_256",    0x1A0000u, 0x1000u },   /* 4 x 256 x 4w   (DMASK tables)    */
    /* STATS action-resolution (0x18000) — garbage after BIST; needed so the
     * DROP_CODE-keyed counters work for RX-drop diagnosis (arista phase48). */
    { "STATS_AR",   0x18000u,  0x2000u },   /* IDX_CAM/RAM + BANK_CFG + port maps */
};

int main(int argc, char **argv)
{
    struct fm6000_dev dev;
    uint32_t value = 0u;
    int rc = 0;

    if (fm6000_hw_open(&dev) < 0) {
        fprintf(stderr, "fm6000_crm: cannot open FM6000 BAR0 (root? enumerated?)\n");
        return 1;
    }

    if (argc >= 3) {
        /* Explicit single set: fm6000_crm <base_word> <count> [value] [size] */
        uint32_t base  = (uint32_t)strtoul(argv[1], NULL, 0);
        uint32_t count = (uint32_t)strtoul(argv[2], NULL, 0);
        unsigned size  = FM6000_CRM_REG_SIZE_32;
        if (argc > 3) value = (uint32_t)strtoul(argv[3], NULL, 0);
        if (argc > 4) size  = (unsigned)strtoul(argv[4], NULL, 0);
        rc = fm6000_crm_memory_set(&dev, 0, base, count, size, value);
    } else {
        /* Default: init the standard CPU-punt forwarding tables to 0+parity. */
        unsigned t;
        for (t = 0; t < sizeof(TABLES)/sizeof(TABLES[0]); t++) {
            fprintf(stderr, "== CRM Memory-Set %s ==\n", TABLES[t].name);
            if (fm6000_crm_memory_set(&dev, 0, TABLES[t].base, TABLES[t].count,
                                      FM6000_CRM_REG_SIZE_32, value) < 0) {
                rc = 1;
                break;   /* stop on first failure — do not keep poking a wedge */
            }
        }
    }

    fm6000_hw_close(&dev);
    return rc;
}
#endif
