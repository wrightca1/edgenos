#include <bmd_config.h>
#if CDK_CONFIG_INCLUDE_BCM56840_A0 == 1

/*
 * RX DMA — multi-DCB ring matching Cumulus 2.5.0's setup on this chip.
 *
 * Reverse-engineered from Cumulus live capture 2026-05-13:
 *   - bcm-knet.c says NUM_CMICM_RX_CHAN=3, MAX_RX_DCBS=64 per ring
 *   - Cumulus's switchd uses RING mode with DESC_HALT_ADDR set to end of ring
 *   - Each DCB has RELOAD=1 so chip auto-reuses; chip walks ring continuously
 *   - DROP_RX_PKT_ON_CHAIN_END=0 so chip blocks if ring is full instead of drop
 *   - CMIC STAT.CHAIN_DONE is sticky-1 in idle on this chip variant; use the
 *     per-DCB DONE bit in the descriptor itself as completion signal.
 *
 * We allocate one big DCB ring + buffer pool at rx_start, set HALT_ADDR to the
 * last DCB (so chip wraps continuously), and poll DCB[rd_idx].DONE.  When a
 * DCB is consumed, we clear DONE/BYTES, advance rd_idx, and let the chip pick
 * it up again automatically (RELOAD=1).
 */

#include <bmd/bmd.h>
#include <bmd/bmd_dma.h>

#include <bmdi/arch/xgs_dma.h>
#include <bmdi/arch/xgsd_dma.h>

#include <cdk/cdk_assert.h>
#include <cdk/cdk_debug.h>

#include <cdk/chip/bcm56840_a0_defs.h>
#include <cdk/arch/xgsd_cmic.h>
#include <cdk/arch/xgsd_chip.h>
#include <cdk/arch/xgs_cmic.h>   /* CMIC_CONFIGr (COS_RX_EN) — XGS packed CMIC */

#include "bcm56840_a0_bmd.h"
#include "bcm56840_a0_internal.h"

#if BMD_CONFIG_INCLUDE_DMA == 1

/* Ring depth.  Cumulus's bcm-knet uses 64.  Keep this a power of 2 so the
 * ring-wrap is a cheap AND.  Memory cost: 64 * (DCB + buffer) ≈ 64 * 2KB. */
#define BMD_RX_RING_DEPTH       64
#define BMD_RX_BUF_SIZE         2048

typedef struct xgs_rx_ring_s {
    /* Contiguous block of BMD_RX_RING_DEPTH DCBs, allocated as one DMA blob. */
    RX_DCB_t   *dcbs;
    dma_addr_t  bdcbs;          /* bus addr of dcbs[0]; ring spans bdcbs..bdcbs+(N-1)*sizeof */
    /* Per-slot RX buffer pointers (one logical bmd_pkt_t per DCB). */
    bmd_pkt_t   pkts[BMD_RX_RING_DEPTH];
    dma_addr_t  pkt_baddrs[BMD_RX_RING_DEPTH];
    uint32_t    rd_idx;         /* Next slot we poll. Advances on every consumed pkt. */
    int         armed;          /* set to 1 after we arm the chip's DMA */
} xgs_rx_ring_t;

static xgs_rx_ring_t _rx_ring[BMD_CONFIG_MAX_UNITS];

/* Currently-served packet — bmd_rx_poll caller still holds the bmd_pkt_t* we
 * returned.  Each slot has its own bmd_pkt_t inside _rx_ring so consumers can
 * hold one at a time without us touching the rest. */

static int
_cpu_port_enable_set(int unit, int enable)
{
    int rd_err, wr_err;
    EPC_LINK_BMAPm_t epc_link;
    uint32_t epc_pbm_before, epc_pbm_after;

    rd_err = READ_EPC_LINK_BMAPm(unit, 0, &epc_link);
    CDK_ASSERT(CMIC_PORT < 32);
    epc_pbm_before = EPC_LINK_BMAPm_PORT_BITMAP_W0f_GET(epc_link);
    if (enable) {
        epc_pbm_after = epc_pbm_before | LSHIFT32(1, CMIC_PORT);
    } else {
        epc_pbm_after = epc_pbm_before & ~LSHIFT32(1, CMIC_PORT);
    }
    EPC_LINK_BMAPm_PORT_BITMAP_W0f_SET(epc_link, epc_pbm_after);
    wr_err = WRITE_EPC_LINK_BMAPm(unit, 0, epc_link);

    if (rd_err) return rd_err;
    if (wr_err) return wr_err;
    return CDK_E_NONE;
}

#endif

/*
 * bcm56840_a0_bmd_rx_start(unit, pkt)
 *
 * On first call: allocate the whole ring, initialise every DCB with a fresh
 * buffer, arm the chip's RX DMA on the head DCB, write DESC_HALT_ADDR to point
 * at the tail.  On subsequent calls: no-op (the ring runs autonomously after
 * the first arming — chip re-uses entries via RELOAD).
 *
 * The `pkt` arg is now ignored — we manage our own per-slot buffers internally.
 * Kept in the signature for API compat with the original single-DCB version.
 */
int
bcm56840_a0_bmd_rx_start(int unit, bmd_pkt_t *pkt)
{
#if BMD_CONFIG_INCLUDE_DMA == 1
    xgs_rx_ring_t *ring = &_rx_ring[unit];
    int rv;
    (void)pkt;  /* unused — we manage buffers internally */

    BMD_CHECK_UNIT(unit);

    if (ring->armed) {
        /* Already running.  No need to re-arm. */
        return CDK_E_NONE;
    }

    /*
     * XGS DMA path (2026-06-02 root-cause fix): the CMICm/xgsd per-channel
     * DMA registers at 0x31xxx do NOT accept writes on this chip (the RX
     * channel never armed — desc/ctrl read back 0), but the XGS *packed*
     * CMIC_DMA registers at 0x100 DO (proven by the working TX path, which
     * uses bmd_xgs_dma_tx_start).  So RX now uses the XGS single-DCB model:
     * one DCB + one buffer; arm -> poll DESC_DONE -> consume -> re-arm.
     * Re-arm is deferred to the next poll (rd_idx as flag) so the chip can't
     * overwrite the buffer while edged is still reading the delivered frame.
     */
    ring->dcbs = bmd_dma_alloc_coherent(unit, sizeof(RX_DCB_t), &ring->bdcbs);
    if (ring->dcbs == NULL) {
        return CDK_E_MEMORY;
    }
    CDK_MEMSET(ring->dcbs, 0, sizeof(RX_DCB_t));

    ring->pkts[0].data = bmd_dma_alloc_coherent(unit, BMD_RX_BUF_SIZE,
                                                &ring->pkt_baddrs[0]);
    if (ring->pkts[0].data == NULL) {
        return CDK_E_MEMORY;
    }
    ring->pkts[0].size  = BMD_RX_BUF_SIZE;
    ring->pkts[0].baddr = ring->pkt_baddrs[0];
    ring->pkts[0].port  = -1;

    RX_DCB_CLR(ring->dcbs[0]);
    RX_DCB_ADDRf_SET(ring->dcbs[0], ring->pkt_baddrs[0]);
    RX_DCB_BYTE_COUNTf_SET(ring->dcbs[0], BMD_RX_BUF_SIZE);

    /* Init the XGS DMA engine: CMIC_CONFIG SG+reload + chan directions.
     * Idempotent and read-modify-write, so it preserves the working TX
     * channel (CH0); it (re)asserts RX channel direction (CH1=RX). */
    bmd_xgs_dma_init(unit);

    /*
     * FP COPY_TO_CPU delivery fix (2026-06-08): clear CMIC_CONFIG.COS_RX_EN.
     * When COS_RX_EN=1 the packet DMA is CoS-based — each channel only pulls
     * the CPU CoS queues named in its per-channel CMIC_CMC_COS_CTRL_RX bitmap.
     * On this chip those CMICm regs (0x31xxx) don't accept writes, so the
     * bitmap is effectively 0 and FP-copied control traffic (which lands in a
     * non-default CoS) is never pulled — proven: match-any FP DROP dropped
     * everything but match-any FP COPY delivered nothing.  With COS_RX_EN=0
     * the single RX channel drains ALL CoS queues (working 0x10c path, RMW).
     */
    {
        CMIC_CONFIGr_t cc;
        int before, after;
        READ_CMIC_CONFIGr(unit, &cc);
        before = CMIC_CONFIGr_COS_RX_ENf_GET(cc);
        CMIC_CONFIGr_COS_RX_ENf_SET(cc, 0);
        WRITE_CMIC_CONFIGr(unit, cc);
        READ_CMIC_CONFIGr(unit, &cc);
        after = CMIC_CONFIGr_COS_RX_ENf_GET(cc);
        CDK_PRINTF("rx_start: CMIC_CONFIG.COS_RX_EN %d->%d (drain all CoS)\n",
                   before, after);
    }

    ring->rd_idx = 0;   /* re-arm flag: 0 = freshly armed, 1 = needs re-arm */

    /* Enable CPU port forwarding BEFORE arming DMA. */
    rv = _cpu_port_enable_set(unit, 1);
    if (rv != CDK_E_NONE) {
        CDK_PRINTF("rx_start: _cpu_port_enable_set failed: %d (continuing)\n", rv);
    }

    BMD_DMA_CACHE_FLUSH(ring->dcbs, sizeof(RX_DCB_t));

    /* Arm the RX channel via the XGS packed CMIC_DMA registers (0x100):
     * write the DCB bus address to CMIC_DMA_DESC0r+4*chan, set DMA_EN in
     * CMIC_DMA_STATr.  These low/mapped offsets accept writes (unlike the
     * 0x31xxx CMICm regs). */
    bmd_xgs_dma_rx_start(unit, ring->bdcbs);

    ring->armed = 1;

    return CDK_E_NONE;
#else
    return CDK_E_UNAVAIL;
#endif
}

/*
 * bcm56840_a0_bmd_rx_poll(unit, &pkt_out)
 *
 * Check DCB[rd_idx].DONE.  If set, fill pkt_out from the DCB+buffer and
 * advance rd_idx (with wrap).  Caller MUST eventually call rx_done on the
 * packet so we can reset the DCB.
 *
 * For now, since the original API is "start submits one pkt; poll returns
 * that same pkt; caller calls start again to resubmit", we approximate by:
 *   - Returning pkt for rd_idx-1 (the slot just consumed)
 *   - On the next bmd_rx_start call (which now becomes a no-op), the chip's
 *     RELOAD already refilled the DCB so we have nothing to do.
 *
 * We DO need to reset DONE/BYTES on the consumed DCB so RELOAD can repopulate.
 */
int
bcm56840_a0_bmd_rx_poll(int unit, bmd_pkt_t **ppkt)
{
#if BMD_CONFIG_INCLUDE_DMA == 1
    xgs_rx_ring_t *ring = &_rx_ring[unit];
    RX_DCB_t *dcb = &ring->dcbs[0];

    BMD_CHECK_UNIT(unit);

    if (!ring->armed) {
        return CDK_E_DISABLED;
    }

    /*
     * Deferred re-arm: a previous poll delivered a packet and set rd_idx=1.
     * edged has since consumed (written to TUN) the buffer, so it is now
     * safe to reset the DCB and re-arm the XGS RX channel for the next frame.
     */
    if (ring->rd_idx) {
        RX_DCB_CLR(*dcb);
        RX_DCB_ADDRf_SET(*dcb, ring->pkt_baddrs[0]);
        RX_DCB_BYTE_COUNTf_SET(*dcb, BMD_RX_BUF_SIZE);
        BMD_DMA_CACHE_FLUSH(dcb, sizeof(*dcb));
        bmd_xgs_dma_rx_start(unit, ring->bdcbs);
        ring->rd_idx = 0;
    }

    /* Non-blocking single poll for DESC_DONE on the RX channel. */
    if (bmd_xgs_dma_rx_poll(unit, 1) < 0) {
        return CDK_E_TIMEOUT;
    }
    BMD_DMA_CACHE_INVAL(dcb, sizeof(*dcb));

    if (RX_DCB_DONEf_GET(*dcb) == 0) {
        return CDK_E_TIMEOUT;
    }

    /* Got a packet. */
    ring->pkts[0].size = RX_DCB_BYTES_TRANSFERREDf_GET(*dcb);
    ring->pkts[0].port = L2P(unit, RX_DCB_SRC_PORTf_GET(*dcb));

    bmd_xgsd_parse_higig2(unit, &ring->pkts[0],
                          RX_DCB_MODULE_HEADERf_PTR(*dcb));

    *ppkt = &ring->pkts[0];

    /* Defer re-arm to the next poll (after edged consumes this buffer). */
    ring->rd_idx = 1;

    return CDK_E_NONE;
#else
    return CDK_E_UNAVAIL;
#endif
}

int
bcm56840_a0_bmd_rx_stop(int unit)
{
#if BMD_CONFIG_INCLUDE_DMA == 1
    xgs_rx_ring_t *ring = &_rx_ring[unit];
    int rv = CDK_E_NONE;
    int i;

    BMD_CHECK_UNIT(unit);

    if (!ring->armed) {
        return CDK_E_DISABLED;
    }

    rv = _cpu_port_enable_set(unit, 0);
    if (CDK_FAILURE(rv)) {
        return rv;
    }

    rv = bmd_xgsd_dma_rx_abort(unit, BMD_CONFIG_DMA_MAX_POLLS);
    if (CDK_FAILURE(rv)) {
        return rv;
    }

    /* Free per-slot buffers and the DCB ring. */
    for (i = 0; i < BMD_RX_RING_DEPTH; i++) {
        if (ring->pkts[i].data) {
            bmd_dma_free_coherent(unit, BMD_RX_BUF_SIZE,
                                  ring->pkts[i].data, ring->pkt_baddrs[i]);
        }
    }
    if (ring->dcbs) {
        bmd_dma_free_coherent(unit,
                              BMD_RX_RING_DEPTH * sizeof(RX_DCB_t),
                              ring->dcbs, ring->bdcbs);
    }
    CDK_MEMSET(ring, 0, sizeof(*ring));

    return rv;
#else
    return CDK_E_UNAVAIL;
#endif
}
#endif /* CDK_CONFIG_INCLUDE_BCM56840_A0 */
