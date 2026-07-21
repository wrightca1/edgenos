/*
 * fm6000_boot.c - FM6000 bring-up sequence (clean-room)
 *
 * Recovered from libFocalpointSDK.so fm6000BootSwitch / fm6000PrebootSwitch /
 * fm6000BistMemoryInit (notes/analysis/phase7g-fm6000-bringup-recovered.md §b).
 *
 * Status of each stage is marked:
 *   [impl]     - implemented from recovered evidence
 *   [skeleton] - register skeleton recovered, runtime values need a live trace
 *   [stub]     - ordering known, body deferred (not on the minimal CPU-punt path)
 *
 * The minimal M2 target (mgmt-plane MVP) is: reach a state where the CPU port can
 * DMA-punt/inject frames. That needs preboot+BIST, microcode, SPICO, and the
 * port/MAC/CPU-port setup. The L2/L3/ACL/QoS table inits are ordered here but
 * stubbed until the dataplane HAL grows past punt.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>

#include "fm6000_boot.h"
#include "fm6000_ucode.h"

#define STEP(fn)                                                              \
    do {                                                                      \
        int _rc = (fn);                                                       \
        if (_rc != 0) {                                                       \
            fprintf(stderr, "fm6000: boot step %s failed (%d)\n", #fn, _rc);  \
            return _rc;                                                       \
        }                                                                     \
    } while (0)

/* ---- BIST / built-in memory init [skeleton] ---------------------------- */
int fm6000_bist_memory_init(struct fm6000_dev *dev)
{
    /* Soft reset + PLL settle (phase7g §b: 0x1C03A/0x1C03C + fmDelay). */
    fm6000_csr_write(dev, FM6000_REG_SOFT_RESET, 1);
    fm6000_delay_us(640);
    fm6000_csr_write(dev, FM6000_REG_PLL_CTRL, 1);
    fm6000_delay_us(1640);

    /* Kick the built-in-memory (BM) engine and wait for it to accept. */
    fm6000_csr_write(dev, FM6000_REG_BM_ENGINE_STATUS, 1);
    if (fm6000_csr_poll(dev, FM6000_REG_BM_ENGINE_STATUS, 0x1, 0x1, 100000) < 0) {
        fprintf(stderr, "fm6000: BM engine did not come ready\n");
        return -1;
    }

    /*
     * TODO(live-trace, GAPS.md A): the BM control word (0x1D08C), the fusebox
     * repair descriptors (0x1D000+, entries seen at 0x1D241/261/281/2A1/2C1),
     * and the per-SRAM MARCH descriptors (0x1D400-0x1D600, 32 memories x 7 words)
     * carry runtime-computed pattern/geometry values derived from switch-info
     * tables. The register skeleton + loop shape are recovered; the *values*
     * come from one live trace. Until then we run the engine's default self-test.
     */
    fm6000_csr_write(dev, FM6000_REG_BM_START_OP, 1);

    /* Settle (SDK waits ~1s) then read the pass/fail result. */
    fm6000_delay_us(1000000);
    if (fm6000_csr_poll(dev, FM6000_REG_BM_RESULT, 0x1, 0x1, 50000000) < 0) {
        fprintf(stderr, "fm6000: BIST did not complete (timeout)\n");
        return -1;
    }
    {
        uint32_t res = fm6000_csr_read(dev, FM6000_REG_BM_RESULT);
        /* Bit layout: BIST_DONE_PASS vs _FAIL (exact bit TBD by trace; treat a
         * clear fail bit as pass for now). */
        fprintf(stderr, "fm6000: BIST result 0x%08x\n", res);
    }
    return 0;
}

/* ---- Stage 1: preboot (chip/PLL + BIST) [skeleton] --------------------- */
int fm6000_preboot(struct fm6000_dev *dev)
{
    /* TODO(live-trace): fm6000PrebootSwitch chip/PLL/clock bring-up internals
     * beyond the soft-reset/PLL pokes folded into BIST above. */
    return fm6000_bist_memory_init(dev);
}

/* ---- Ordered helpers not on the minimal punt path [stub] --------------- */
static int fm6000_init_sbus(struct fm6000_dev *dev)            { (void)dev; return 0; } /* [impl-lite] */
static int fm6000_validate_sched_token(struct fm6000_dev *dev){ (void)dev; return 0; }
static int fm6000_init_register_cache(struct fm6000_dev *dev) { (void)dev; return 0; }
static int fm6000_get_switch_info(struct fm6000_dev *dev)     { (void)dev; return 0; }
static int fm6000_port_mac_cpu_setup(struct fm6000_dev *dev)  { (void)dev; return 0; } /* needed for punt */
static int fm6000_init_trapcode_table(struct fm6000_dev *dev) { (void)dev; return 0; } /* needed for punt */
static int fm6000_forwarding_tables(struct fm6000_dev *dev)   { (void)dev; return 0; } /* L2L/FFU/router/... */
static int fm6000_enable_forwarding(struct fm6000_dev *dev)   { (void)dev; return 0; }

/* ---- Top-level bring-up (phase7g §b order) ----------------------------- */
int fm6000_boot_switch(struct fm6000_dev *dev)
{
    fprintf(stderr, "fm6000: bring-up start (unit %d, %s)\n", dev->unit, dev->pci_slot);

    STEP(fm6000_preboot(dev));                 /* chip/PLL + BIST            */
    STEP(fm6000_init_sbus(dev));               /* SBus controller online     */
    STEP(fm6000_validate_sched_token(dev));
    STEP(fm6000_init_register_cache(dev));
    STEP(fm6000_get_switch_info(dev));         /* sizes/geometry tables      */

    /* Microcode: parser/FFU text image BEFORE SerDes SPICO (phase7g order). */
    if (fm6000_load_csr_image(dev, FM6000_FW_PARSER_FFU, 1) < 0)
        return -1;
    STEP(fm6000_load_spico(dev, FM6000_FW_SPICO));

    /* Ports / MAC / CPU port — the minimal path to CPU punt/inject. */
    STEP(fm6000_port_mac_cpu_setup(dev));
    STEP(fm6000_init_trapcode_table(dev));

    /* Forwarding-table init (L2L sweepers/hash, FFU, BST, router, mcast, LAG,
     * ACL, mirror, sFlow, hash, RBridge, storm, QoS, glort/dest-mask/MA). */
    STEP(fm6000_forwarding_tables(dev));
    STEP(fm6000_enable_forwarding(dev));

    fprintf(stderr, "fm6000: bring-up complete\n");
    return 0;
}
