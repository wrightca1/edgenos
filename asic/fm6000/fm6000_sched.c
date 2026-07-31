/*
 * fm6000_sched.c - FM6000 SSCHED port-service-ring init (corrected, phase61).
 *
 * The port-0 (PCIe DMA) egress scheduler is uninitialized on a bare-M1 bring-up
 * (SSCHED ring empty), so frames forwarded to port 0 never drain to the DMA RX
 * ring (arista phase51/55). The real EOS ring-init — recovered by tracing the
 * reverse path api.fmSetSwitchState() -> fm6000BootSwitch -> the ring-init static
 * after fmPlatformSetRingMode in libFocalpointSDK.so (arista phase60/61) — is a
 * STRAIGHT-LINE WRITE sequence with NO polling:
 *
 *   1. per scheduled port:  write RX_INIT_TOKEN(0x8060) + TX_INIT_TOKEN(0x8020)
 *   2. write the NEXT_PORT visit table:  TX 0x8000+i / RX 0x8040+i  (i=0..19)
 *   3. write SLOW_PORT:  RX 0x8070+i  (i=0..4)
 *   4. COMMIT:  RX_INIT_COMPLETE(0x8061)=1  then  TX_INIT_COMPLETE(0x8021)=1
 *
 * CORRECTION to phase57: INIT_COMPLETE / FREELIST_DONE are WRITE-1 COMMIT STROBES,
 * not status bits. The old tool wrote one INIT_TOKEN then *polled* 0x8021 for a bit
 * HW never sets — so it hung/aborted BEFORE writing the NEXT_PORT table, leaving the
 * ring empty. That phantom poll was the byte-mover blocker.
 *
 * We replay the exact GOLDEN 7150 ring captured live from running EOS
 * (reference/live-captures/.../golden_ssched_full_2026-07-28.txt): the lab box has
 * its ports down, so the golden ring is minimal — TX_NEXT_PORT[0]=0x03020100
 * (ports 0,1,2,3) + [19]=0x004e0000 (port 78); everything else 0. Port 0 (the PCIe
 * DMA/CPU port) is the first scheduled slot — exactly what CPU-punt needs.
 *
 * Usage:  fm6000_sched            (default: replay the golden minimal ring)
 * Reuses fm6000_hw. Run with the SCD watchdog armed (this touches the scheduler).
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>

#include "fm6000_hw.h"
#include "fm6000_regs.h"

/* Golden minimal ring (live EOS capture, ports down). NEXT_PORT is 20 words of
 * 4 ring-slots/byte; only these two words are non-zero, the rest are 0. */
static const struct { unsigned idx; uint32_t val; } g_next_port[] = {
    { 0,  0x03020100u },   /* slots: ports 0,1,2,3 (port 0 = PCIe DMA, first) */
    { 19, 0x004e0000u },   /* slot: port 78 (0x4e, mgmt)                      */
};
/* Golden SLOW_PORT[0..4] (0x8070..0x8074). */
static const uint32_t g_slow_port[] = {
    0x0000000fu, 0x0000ffe0u, 0x0000feffu, 0x0000fff0u, 0x00000fffu,
};
/* Ports that get a token inserted (the ports appearing in the golden ring).
 * lock=1 keeps the slot even when idle (the CPU port must always be serviced). */
static const struct { unsigned port, locked, sync; } g_tokens[] = {
    { 0, 1, 0 }, { 1, 1, 0 }, { 2, 1, 0 }, { 3, 1, 0 }, { 78, 1, 1 },
};

int main(int argc, char **argv)
{
    struct fm6000_dev dev;
    unsigned i;
    (void)argc; (void)argv;

    if (fm6000_hw_open(&dev) < 0) {
        fprintf(stderr, "fm6000_sched: cannot open FM6000 BAR0 (root? enumerated?)\n");
        return 1;
    }

    fprintf(stderr, "fm6000_sched: before: TX_NEXT_PORT[0]=0x%08x TICK_CFG=0x%08x JSS[3]=0x%08x\n",
            fm6000_csr_read(&dev, FM6000_SSCHED_TX_NEXT_PORT(0)),
            fm6000_csr_read(&dev, FM6000_SSCHED_TICK_CFG),
            fm6000_csr_read(&dev, FM6000_BLK_JSS + 0x003));

    /* ---- Preboot equivalents: JSS + tick + sweeper to golden (live-verified) ----
     * These enable the scheduler-engine clock domain. Golden values from the
     * 2026-07-28 live capture (NOTE: JSS[3]=0x15, not the 0x14 the phase57 tool had). */
    fm6000_csr_write(&dev, FM6000_BLK_JSS + 0x001, 0x0521452au);
    fm6000_csr_write(&dev, FM6000_BLK_JSS + 0x002, 0x00000016u);
    fm6000_csr_write(&dev, FM6000_BLK_JSS + 0x003, 0x00000015u);   /* golden = 0x15 */
    fm6000_csr_write(&dev, FM6000_BLK_JSS + 0x004, 0x00000002u);
    fm6000_csr_write(&dev, FM6000_BLK_JSS + 0x008, 0x00000001u);
    fm6000_csr_write(&dev, FM6000_SSCHED_TICK_CFG, 0x00000002u);   /* tick period 2 */

    /* SWEEPER_CFG (golden 7150) — drives the scheduler ticks. 5 words. */
    fm6000_csr_write(&dev, FM6000_SWEEPER_CFG(0), 0x0008bb2cu);
    fm6000_csr_write(&dev, FM6000_SWEEPER_CFG(1), 0x00000002u);
    fm6000_csr_write(&dev, FM6000_SWEEPER_CFG(2), 0x00000000u);
    fm6000_csr_write(&dev, FM6000_SWEEPER_CFG(3), 0x0030a2c3u);
    fm6000_csr_write(&dev, FM6000_SWEEPER_CFG(4), 0x00002000u);

    /* Scheduler freelists are initialized by boot-ctrl cmd3 (FREELISTS) before this
     * tool runs (arista phase59); the FREELIST_INIT value is a computed size/base we
     * don't replicate here. If a future test shows the ring has no queue backing,
     * add the freelist INIT writes (0x80F0/F4/F8/FC) + DONE=1 here. */

    /* ---- Ring init: straight-line, NO polling (the corrected sequence) ---- */

    /* 1. insert a token per scheduled port (RX first, then TX — matches the SDK). */
    for (i = 0; i < sizeof(g_tokens)/sizeof(g_tokens[0]); i++) {
        uint32_t tok = FM6000_SSCHED_TOKEN(g_tokens[i].port, g_tokens[i].locked, g_tokens[i].sync);
        fm6000_csr_write(&dev, FM6000_SSCHED_RX_INIT_TOKEN, tok);
        fm6000_csr_write(&dev, FM6000_SSCHED_TX_INIT_TOKEN, tok);
    }

    /* 2. write the full 20-word NEXT_PORT visit table (TX + RX). Write every index
     *    explicitly (0 where golden is 0) so no stale entry desyncs the ring. */
    {
        uint32_t tx[20] = {0}, rx[20] = {0};
        for (i = 0; i < sizeof(g_next_port)/sizeof(g_next_port[0]); i++) {
            tx[g_next_port[i].idx] = g_next_port[i].val;
            rx[g_next_port[i].idx] = g_next_port[i].val;
        }
        for (i = 0; i < 20; i++) {
            fm6000_csr_write(&dev, FM6000_SSCHED_RX_NEXT_PORT(i), rx[i]);
            fm6000_csr_write(&dev, FM6000_SSCHED_TX_NEXT_PORT(i), tx[i]);
        }
    }

    /* 3. SLOW_PORT table (RX 0x8070+i). */
    for (i = 0; i < sizeof(g_slow_port)/sizeof(g_slow_port[0]); i++)
        fm6000_csr_write(&dev, FM6000_SSCHED_RX_SLOW_PORT(i), g_slow_port[i]);

    /* 4. COMMIT the ring — write-1 strobes, RX then TX. No poll. */
    fm6000_csr_write(&dev, FM6000_SSCHED_RX_INIT_COMPLETE, 1u);
    fm6000_csr_write(&dev, FM6000_SSCHED_TX_INIT_COMPLETE, 1u);

    /* 5. phase82: the rest of the GOLDEN scheduler state (live capture from the warm chip,
     *    reference/scd-dumps/fm6000-golden-scheduler-state-warm.txt). The minimal ring alone
     *    (cold82) didn't unblock the fill; add the EGRESS scheduler per-port config + DRR + the
     *    SSCHED replace-token values so the cold scheduler matches golden as closely as possible. */
    if (!getenv("FM6000_SCHED_MINIMAL")) {
        unsigned p;
        /* ESCHED_CFG_1 (0x2000+i) / CFG_2 (0x2080+i), 76 ports: port0 special, rest default 0x00ffffff */
        for (p = 0; p < 76; p++) {
            fm6000_csr_write(&dev, 0x2000u + p, p == 0 ? 0x00fff800u : 0x00ffffffu);
            fm6000_csr_write(&dev, 0x2080u + p, p == 0 ? 0x00fff000u : 0x00ffffffu);
        }
        /* ESCHED_DRR_CFG (MONITOR 0x3800+i): golden alternating even=0x00ffffff, odd=0x14ffffff */
        for (p = 0; p < 76; p++)
            fm6000_csr_write(&dev, 0x3800u + p, (p & 1) ? 0x14ffffffu : 0x00ffffffu);
        /* SSCHED replace-token last values (golden) */
        fm6000_csr_write(&dev, FM6000_SSCHED_TX_REPLACE_TOKEN, 0xc0300200u);
        fm6000_csr_write(&dev, FM6000_SSCHED_RX_REPLACE_TOKEN, 0x00200200u);
        fprintf(stderr, "fm6000_sched: full golden scheduler applied (ESCHED CFG_1/2 + DRR + replace tokens)\n");
    }

    fprintf(stderr, "fm6000_sched: after:  TX_NEXT_PORT[0]=0x%08x [19]=0x%08x  RX[0]=0x%08x\n",
            fm6000_csr_read(&dev, FM6000_SSCHED_TX_NEXT_PORT(0)),
            fm6000_csr_read(&dev, FM6000_SSCHED_TX_NEXT_PORT(19)),
            fm6000_csr_read(&dev, FM6000_SSCHED_RX_NEXT_PORT(0)));
    fprintf(stderr, "fm6000_sched: ring committed (golden minimal ring; port 0 scheduled). "
            "Verify: TX_NEXT_PORT[0] should read 0x03020100.\n");

    fm6000_hw_close(&dev);
    return 0;
}
