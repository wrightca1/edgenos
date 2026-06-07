/*
 * cumulus_replicate.c - replicate Cumulus's chip-memory state for
 *                       four documented chip→CPU drop causes.
 *
 * Cross-correlating dump_soc_diff.txt with dump_socmem_diff.txt
 * (decoded.md/14_register_memory_code_crosscorrelation.md) surfaced
 * four chip memories that Cumulus populates and our edged didn't:
 *
 *   1) EPC_LINK_BMAP       — egress-pipeline port bitmap (1 row)
 *   2) L2_USER_ENTRY       — 63 protocol-MAC CPU-trap rules
 *   3) EGR_VLAN(_STG)      — 53 service-VID egress rows + STG state
 *   4) FP_TCAM / FP_POLICY — 100 chip-side trap rules
 *
 * Each loader function below mirrors Cumulus's captured row contents
 * into our chip.  Auto-generated row data lives under generated/.
 *
 * Copyright (C) 2026 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "edged.h"

#include <cdk/chip/bcm56840_a0_defs.h>
#include <cdk/arch/xgs_chip.h>
#include <cdk/arch/xgs_reg.h>

#include "generated/cumulus_l2_user_entry.h"
#include "generated/cumulus_egr_vlan.h"
#include "generated/cumulus_egr_vlan_stg.h"
#include "generated/cumulus_fp_tcam.h"
#include "generated/cumulus_fp_policy_table.h"

/*
 * 1) EPC_LINK_BMAP
 *
 * Cumulus capture (decoded.md/14): PORT_BITMAP = 0x020000000000000007.
 * That's W0=0x7 (CPU port 0 + device ports 1+2 = swp1+swp2) and
 * W2=0x2 (bit 65, an internal aggregate/loopback port Cumulus always
 * sets).  Without this the egress pipeline drops every frame after
 * the MMU, including egress-to-CPU.
 */
static int cumulus_replicate_epc_link_bmap(int unit)
{
    EPC_LINK_BMAPm_t bmp;
    int rv;

    EPC_LINK_BMAPm_CLR(bmp);
    EPC_LINK_BMAPm_PORT_BITMAP_W0f_SET(bmp, 0x00000007);
    EPC_LINK_BMAPm_PORT_BITMAP_W1f_SET(bmp, 0x00000000);
    EPC_LINK_BMAPm_PORT_BITMAP_W2f_SET(bmp, 0x00000002);

    rv = WRITE_EPC_LINK_BMAPm(unit, 0, bmp);
    if (rv < 0) {
        syslog(LOG_ERR, "EPC_LINK_BMAP write failed: %d", rv);
        return 1;
    }
    syslog(LOG_INFO,
           "EPC_LINK_BMAP[0] = 0x020000000000000007 (CPU+swp1+swp2+bit65)");
    return 0;
}

/*
 * 2) L2_USER_ENTRY — protocol-MAC CPU-trap table.
 *
 * Cumulus populates 63 rows (indices 0..62) with the standard
 * 01:80:c2:00:00:XX BPDU/LLDP/LACP/STP family.  Each row sets
 * CPU=1, BPDU=1 so the chip copies matching frames to CPU instead
 * of forwarding.  KEY_TYPE=1 rows carry an extra protocol-pkt bit.
 */
static int cumulus_replicate_l2_user_entry(int unit)
{
    int errs = 0;
    unsigned int i;

    for (i = 0; i < CUMULUS_L2_USER_ENTRY_COUNT; i++) {
        const struct cumulus_l2_user_entry_row *r =
            &cumulus_l2_user_entry_rows[i];
        L2_USER_ENTRYm_t e;
        uint32_t mac_fval[2];
        uint32_t key_fval[2];
        uint32_t mask_fval[2];
        int rv;

        L2_USER_ENTRYm_CLR(e);

        mac_fval[0] = (uint32_t)(r->mac_addr & 0xFFFFFFFF);
        mac_fval[1] = (uint32_t)((r->mac_addr >> 32) & 0xFFFF);
        L2_USER_ENTRYm_MAC_ADDRf_SET(e, mac_fval);

        key_fval[0] = (uint32_t)(r->key & 0xFFFFFFFF);
        key_fval[1] = (uint32_t)((r->key >> 32) & 0x1FFFFFFFu);
        L2_USER_ENTRYm_KEYf_SET(e, key_fval);

        /* MASK from Cumulus capture: 0x1000ffffffffffff = top bit (1 << 60)
         * plus low 48 bits of MAC.  Identical across all rows. */
        mask_fval[0] = 0xFFFFFFFFu;
        mask_fval[1] = 0x1000FFFFu;
        L2_USER_ENTRYm_MASKf_SET(e, mask_fval);

        L2_USER_ENTRYm_VALIDf_SET(e, r->valid);
        if (r->key_type)             L2_USER_ENTRYm_KEY_TYPEf_SET(e, 1);
        if (r->l2_protocol_pkt)      L2_USER_ENTRYm_L2_PROTOCOL_PKTf_SET(e, 1);
        if (r->do_not_learn_macsa)   L2_USER_ENTRYm_DO_NOT_LEARN_MACSAf_SET(e, 1);
        L2_USER_ENTRYm_CPUf_SET(e, r->cpu);
        L2_USER_ENTRYm_BPDUf_SET(e, r->bpdu);

        rv = WRITE_L2_USER_ENTRYm(unit, r->index, e);
        if (rv < 0) {
            syslog(LOG_ERR,
                   "L2_USER_ENTRY[%u] write failed: %d", r->index, rv);
            errs++;
        }
    }

    /* OSPF multicast CPU traps — not in the captured Cumulus L2_USER table
     * (Cumulus relied on UMC + FP rules for these). Add deterministic exact-MAC
     * copy-to-CPU entries for AllSPFRouters 01:00:5e:00:00:05 and AllDRouters
     * 01:00:5e:00:00:06 so the OSPF daemon hears the Nexus regardless of the
     * VLAN UMC-flood configuration. Same encoding as the captured rows:
     * key_type=0 -> key = 0x0000<mac>, mask = 0x1000ffffffffffff, cpu=1. */
    {
        static const uint64_t ospf_macs[] = {
            0x01005e000005ULL, 0x01005e000006ULL,
        };
        unsigned base_idx = CUMULUS_L2_USER_ENTRY_COUNT;  /* after captured rows */
        for (unsigned k = 0; k < 2; k++) {
            L2_USER_ENTRYm_t e;
            uint32_t mac_fval[2], key_fval[2], mask_fval[2];
            L2_USER_ENTRYm_CLR(e);
            mac_fval[0] = (uint32_t)(ospf_macs[k] & 0xFFFFFFFF);
            mac_fval[1] = (uint32_t)((ospf_macs[k] >> 32) & 0xFFFF);
            L2_USER_ENTRYm_MAC_ADDRf_SET(e, mac_fval);
            key_fval[0] = mac_fval[0];
            key_fval[1] = mac_fval[1];          /* key_type 0 -> top bits clear */
            L2_USER_ENTRYm_KEYf_SET(e, key_fval);
            mask_fval[0] = 0xFFFFFFFFu;
            mask_fval[1] = 0x1000FFFFu;
            L2_USER_ENTRYm_MASKf_SET(e, mask_fval);
            L2_USER_ENTRYm_VALIDf_SET(e, 1);
            L2_USER_ENTRYm_DO_NOT_LEARN_MACSAf_SET(e, 1);
            L2_USER_ENTRYm_CPUf_SET(e, 1);
            int rv2 = WRITE_L2_USER_ENTRYm(unit, base_idx + k, e);
            syslog(LOG_INFO, "L2_USER_ENTRY[%u] OSPF %012llx -> CPU wr=%d",
                   base_idx + k, (unsigned long long)ospf_macs[k], rv2);
            if (rv2 < 0) errs++;
        }
    }

    syslog(LOG_INFO,
           "L2_USER_ENTRY: programmed %u rows (Cumulus protocol-MAC traps) + 2 OSPF, "
           "errors=%d", (unsigned)CUMULUS_L2_USER_ENTRY_COUNT, errs);
    return errs;
}

/*
 * 3) EGR_VLAN + EGR_VLAN_STG
 *
 * Cumulus's service-VID scheme: VID 1 = baseline, VID 3301..3352 =
 * per-port (CPU + port_N) bidirectional service VIDs.  Each row also
 * sets STG=1 so EGR_VLAN_STG[1] governs forwarding.
 */
static int cumulus_replicate_egr_vlan(int unit)
{
    int errs = 0;
    unsigned int i;

    for (i = 0; i < CUMULUS_EGR_VLAN_COUNT; i++) {
        const struct cumulus_egr_vlan_row *r = &cumulus_egr_vlan_rows[i];
        EGR_VLANm_t v;
        int rv;

        EGR_VLANm_CLR(v);
        EGR_VLANm_VALIDf_SET(v, r->valid);
        EGR_VLANm_STGf_SET(v, r->stg);
        EGR_VLANm_PORT_BITMAP_W0f_SET(v, r->port_bitmap_w0);
        EGR_VLANm_PORT_BITMAP_W1f_SET(v, r->port_bitmap_w1);
        EGR_VLANm_PORT_BITMAP_W2f_SET(v, r->port_bitmap_w2);
        EGR_VLANm_UT_PORT_BITMAP_W0f_SET(v, r->ut_port_bitmap_w0);
        EGR_VLANm_UT_PORT_BITMAP_W1f_SET(v, r->ut_port_bitmap_w1);
        EGR_VLANm_UT_PORT_BITMAP_W2f_SET(v, r->ut_port_bitmap_w2);
        EGR_VLANm_UT_BITMAP_W0f_SET(v, r->ut_bitmap_w0);
        EGR_VLANm_UT_BITMAP_W1f_SET(v, r->ut_bitmap_w1);
        EGR_VLANm_UT_BITMAP_W2f_SET(v, r->ut_bitmap_w2);

        rv = WRITE_EGR_VLANm(unit, r->index, v);
        if (rv < 0) {
            syslog(LOG_ERR, "EGR_VLAN[%u] write failed: %d", r->index, rv);
            errs++;
        }
    }

    /*
     * EGR_VLAN[1] override: Cumulus left VID 1's PORT_BITMAP empty
     * because their traffic ran on service VIDs 3301..3352.  Our
     * kernel-originated traffic (ARP, IPv4) defaults to PVID=1 with
     * untagged egress.  An empty PORT_BITMAP causes the egress pipe
     * to drop everything on VID 1, including CPU-trapped frames.
     *
     * Override to {CPU, swp1, swp2} = bits 0,1,2 → 0x7.  UT_* bitmaps
     * mirror that so untagged egress is allowed on every member port.
     */
    {
        EGR_VLANm_t v;
        int rv;
        EGR_VLANm_CLR(v);
        EGR_VLANm_VALIDf_SET(v, 1);
        EGR_VLANm_STGf_SET(v, 1);
        EGR_VLANm_PORT_BITMAP_W0f_SET(v, 0x7);
        EGR_VLANm_UT_PORT_BITMAP_W0f_SET(v, 0x7);
        EGR_VLANm_UT_BITMAP_W0f_SET(v, 0x7);
        rv = WRITE_EGR_VLANm(unit, 1, v);
        if (rv < 0) {
            syslog(LOG_ERR, "EGR_VLAN[1] override write failed: %d", rv);
            errs++;
        } else {
            syslog(LOG_INFO,
                   "EGR_VLAN[1] override: PORT_BITMAP=0x7 (CPU+swp1+swp2)");
        }
    }

    /*
     * EGR_VLAN_STG[1] — per-port spanning-tree state for STG 1.
     * Cumulus's capture only set PORT1=PORT2=3 (FORWARDING) because they
     * had only swp1+swp2 active.  We set every port 0..65 to FORWARDING
     * so any of our up ports works.  Two bits per port slot, so
     * 0xFFFFFFFF in each word = 16 ports × 3.  Ports 64,65 sit in
     * word 4's low nibble (0xF).
     */
    {
        EGR_VLAN_STGm_t stg;
        int rv;
        EGR_VLAN_STGm_CLR(stg);
        stg.egr_vlan_stg[0] = 0xFFFFFFFFu;  /* ports 0..15  -> all FORWARDING */
        stg.egr_vlan_stg[1] = 0xFFFFFFFFu;  /* ports 16..31 -> all FORWARDING */
        stg.egr_vlan_stg[2] = 0xFFFFFFFFu;  /* ports 32..47 -> all FORWARDING */
        stg.egr_vlan_stg[3] = 0xFFFFFFFFu;  /* ports 48..63 -> all FORWARDING */
        stg.egr_vlan_stg[4] = 0x0000000Fu;  /* ports 64,65  -> FORWARDING */

        rv = WRITE_EGR_VLAN_STGm(unit, 1, stg);
        if (rv < 0) {
            syslog(LOG_ERR, "EGR_VLAN_STG[1] write failed: %d", rv);
            errs++;
        }
    }

    syslog(LOG_INFO,
           "EGR_VLAN: programmed %u rows + STG[1]=FORWARDING (errors=%d)",
           (unsigned)CUMULUS_EGR_VLAN_COUNT, errs);
    return errs;
}

/*
 * 4) FP_TCAM + FP_POLICY_TABLE — chip-side CPU-trap rules
 *
 * 100 entries each at chip indices 256..355.  Policy rows set
 * Y_COPY_TO_CPU=3 / R_COPY_TO_CPU=3 / G_COPY_TO_CPU=3 (copy on every
 * meter color) and DROP=1 (drop the forwarded copy, only the CPU
 * copy survives).  This is what 00control_plane.rules compiles into.
 */
static int cumulus_replicate_fp(int unit)
{
    int errs = 0;
    unsigned int i;

    for (i = 0; i < CUMULUS_FP_TCAM_COUNT; i++) {
        const struct cumulus_fp_tcam_row *r = &cumulus_fp_tcam_rows[i];
        FP_TCAMm_t t;
        uint32_t key_fval[8];
        uint32_t mask_fval[8];
        int rv;

        memcpy(key_fval,  r->key,  sizeof(key_fval));
        memcpy(mask_fval, r->mask, sizeof(mask_fval));

        FP_TCAMm_CLR(t);
        FP_TCAMm_VALIDf_SET(t, r->valid);
        FP_TCAMm_KEYf_SET(t,  key_fval);
        FP_TCAMm_MASKf_SET(t, mask_fval);

        rv = WRITE_FP_TCAMm(unit, r->index, t);
        if (rv < 0) {
            syslog(LOG_ERR, "FP_TCAM[%u] write failed: %d", r->index, rv);
            errs++;
        }
    }

    for (i = 0; i < CUMULUS_FP_POLICY_TABLE_COUNT; i++) {
        const struct cumulus_fp_policy_table_row *r =
            &cumulus_fp_policy_table_rows[i];
        FP_POLICY_TABLEm_t p;
        int rv;

        FP_POLICY_TABLEm_CLR(p);
        FP_POLICY_TABLEm_Y_DROPf_SET(p, r->y_drop);
        FP_POLICY_TABLEm_Y_COPY_TO_CPUf_SET(p, r->y_copy_to_cpu);
        FP_POLICY_TABLEm_R_DROPf_SET(p, r->r_drop);
        FP_POLICY_TABLEm_R_COPY_TO_CPUf_SET(p, r->r_copy_to_cpu);
        FP_POLICY_TABLEm_G_DROPf_SET(p, r->g_drop);
        FP_POLICY_TABLEm_G_COPY_TO_CPUf_SET(p, r->g_copy_to_cpu);
        /* METER_PAIR_MODE_MODIFIER + COUNTER_MODE captured but not yet ported;
         * Cumulus uses them for paired-meter accounting which we don't have
         * meter tables programmed for. */

        rv = WRITE_FP_POLICY_TABLEm(unit, r->index, p);
        if (rv < 0) {
            syslog(LOG_ERR, "FP_POLICY_TABLE[%u] write failed: %d",
                   r->index, rv);
            errs++;
        }
    }

    syslog(LOG_INFO,
           "FP: programmed %u TCAM + %u POLICY rows (errors=%d)",
           (unsigned)CUMULUS_FP_TCAM_COUNT,
           (unsigned)CUMULUS_FP_POLICY_TABLE_COUNT, errs);
    return errs;
}

/*
 * Enable CMICm CMC0 PCIE IRQ mask + read back DMA state for diagnostics.
 *
 * Symptom: /proc/interrupts shows linux-kernel-bde IRQ count = 0;
 * handle_asic_rx polls CDK_E_TIMEOUT forever.  Either the chip doesn't
 * raise IRQs because the mask is empty (most likely; bmd_init never
 * touches it) or the DMA channel state is broken.  This function makes
 * both visible and fixes the mask half:
 *
 *   CMIC_CMC0_PCIE_IRQ_MASK0r (0x31414) bits:
 *     15  CH0_CHAIN_DONE  (TX channel chain completion)
 *     14  CH0_DESC_DONE
 *     13  CH1_CHAIN_DONE  (RX channel chain completion)
 *     12  CH1_DESC_DONE
 *
 *   CMIC_CMC_DMA_CTRLr (0x31140) + 4*chan: per-channel CTRL
 *   CMIC_CMC_DMA_STATr (0x31150): channel state bits (sticky)
 *   CMIC_CMC_DMA_DESCr (0x31158) + 4*chan: current DCB ptr
 */
static void cumulus_enable_cmicm_irq(int unit)
{
    /* PCIE_IRQ_MASK0 — enable our two channels (TX=0, RX=1).
     *
     * Compare two access paths.  Direct (cdk_xgs_reg32) was observed
     * to not stick (write of 0xf000 read back as 0).  iProc-AXI
     * sub-window 7 is the alternative and is what the Cumulus BDE
     * uses; if it sticks via that path but not direct, BAR0 doesn't
     * really cover the full 256 KB AXI window like the kernel-bde
     * comment claims. */
    const uint32_t want = (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15);
    uint32_t direct_before = 0, direct_after = 0;
    uint32_t iproc_before = 0, iproc_after = 0;
    uint32_t tmp;

    /* Path A: direct via cdk_xgs_reg32 (BDE_IOC_REG_WRITE → iowrite32) */
    cdk_xgs_reg32_read(unit, 0x31414, &direct_before);
    tmp = direct_before | want;
    cdk_xgs_reg32_write(unit, 0x31414, &tmp);
    cdk_xgs_reg32_read(unit, 0x31414, &direct_after);

    /* Path B: iProc AXI sub-window 7 remap */
    bde_iproc_read32(0x31414, &iproc_before);
    bde_iproc_write32(0x31414, iproc_before | want);
    bde_iproc_read32(0x31414, &iproc_after);

    syslog(LOG_INFO,
           "CMICm PCIE_IRQ_MASK0: direct(before=0x%08x after=0x%08x) "
           "iproc(before=0x%08x after=0x%08x) want=0x%08x",
           direct_before, direct_after, iproc_before, iproc_after, want);

    /* Per-channel state dump (direct vs iProc) for CTRL + STAT + DESC */
    {
        uint32_t d_ctrl0 = 0, d_ctrl1 = 0, d_stat = 0, d_desc0 = 0, d_desc1 = 0;
        uint32_t i_ctrl0 = 0, i_ctrl1 = 0, i_stat = 0, i_desc0 = 0, i_desc1 = 0;
        cdk_xgs_reg32_read(unit, 0x31140 + 0,  &d_ctrl0);
        cdk_xgs_reg32_read(unit, 0x31140 + 4,  &d_ctrl1);
        cdk_xgs_reg32_read(unit, 0x31150,      &d_stat);
        cdk_xgs_reg32_read(unit, 0x31158 + 0,  &d_desc0);
        cdk_xgs_reg32_read(unit, 0x31158 + 4,  &d_desc1);
        bde_iproc_read32(0x31140 + 0, &i_ctrl0);
        bde_iproc_read32(0x31140 + 4, &i_ctrl1);
        bde_iproc_read32(0x31150,     &i_stat);
        bde_iproc_read32(0x31158 + 0, &i_desc0);
        bde_iproc_read32(0x31158 + 4, &i_desc1);
        syslog(LOG_INFO,
               "CMICm direct: ctrl0=0x%08x ctrl1=0x%08x stat=0x%08x "
               "desc0=0x%08x desc1=0x%08x",
               d_ctrl0, d_ctrl1, d_stat, d_desc0, d_desc1);
        syslog(LOG_INFO,
               "CMICm iproc:  ctrl0=0x%08x ctrl1=0x%08x stat=0x%08x "
               "desc0=0x%08x desc1=0x%08x",
               i_ctrl0, i_ctrl1, i_stat, i_desc0, i_desc1);
    }
}

/*
 * Read back one row from each table and log it.  Confirms the
 * WRITE_*m calls actually reached the chip (errors=0 from the writer
 * only means the s-channel transaction completed, not that the chip
 * stored the value — verifying with a chip-side read closes that gap).
 */
static void cumulus_replicate_readback(int unit)
{
    /* EPC_LINK_BMAP[0].  READ_*m macros expect &struct (cdk uses
     * `&m._member` precedence trick), unlike WRITE_*m which already
     * has the & built in. */
    {
        EPC_LINK_BMAPm_t bmp;
        int rv = READ_EPC_LINK_BMAPm(unit, 0, &bmp);
        if (rv < 0) {
            syslog(LOG_ERR, "readback EPC_LINK_BMAP rv=%d", rv);
        } else {
            uint32_t w0 = EPC_LINK_BMAPm_PORT_BITMAP_W0f_GET(bmp);
            uint32_t w1 = EPC_LINK_BMAPm_PORT_BITMAP_W1f_GET(bmp);
            uint32_t w2 = EPC_LINK_BMAPm_PORT_BITMAP_W2f_GET(bmp);
            syslog(LOG_INFO,
                   "readback EPC_LINK_BMAP[0]: W0=0x%08x W1=0x%08x W2=0x%08x",
                   w0, w1, w2);
        }
    }

    /* L2_USER_ENTRY[0] — LLDP MAC trap */
    {
        L2_USER_ENTRYm_t e;
        int rv = READ_L2_USER_ENTRYm(unit, 0, &e);
        if (rv < 0) {
            syslog(LOG_ERR, "readback L2_USER_ENTRY[0] rv=%d", rv);
        } else {
            uint32_t valid = L2_USER_ENTRYm_VALIDf_GET(e);
            uint32_t cpu   = L2_USER_ENTRYm_CPUf_GET(e);
            uint32_t bpdu  = L2_USER_ENTRYm_BPDUf_GET(e);
            uint32_t mac_buf[2] = {0, 0};
            L2_USER_ENTRYm_MAC_ADDRf_GET(e, mac_buf);
            syslog(LOG_INFO,
                   "readback L2_USER_ENTRY[0]: VALID=%u CPU=%u BPDU=%u "
                   "MAC=0x%04x%08x", valid, cpu, bpdu, mac_buf[1], mac_buf[0]);
        }
    }

    /* EGR_VLAN[1] = default VLAN */
    {
        EGR_VLANm_t v;
        int rv = READ_EGR_VLANm(unit, 1, &v);
        if (rv < 0) {
            syslog(LOG_ERR, "readback EGR_VLAN[1] rv=%d", rv);
        } else {
            uint32_t valid = EGR_VLANm_VALIDf_GET(v);
            uint32_t stg   = EGR_VLANm_STGf_GET(v);
            uint32_t pb0   = EGR_VLANm_PORT_BITMAP_W0f_GET(v);
            syslog(LOG_INFO,
                   "readback EGR_VLAN[1]: VALID=%u STG=%u PB_W0=0x%08x",
                   valid, stg, pb0);
        }
    }

    /* EGR_VLAN_STG[1] */
    {
        EGR_VLAN_STGm_t stg;
        int rv = READ_EGR_VLAN_STGm(unit, 1, &stg);
        if (rv < 0) {
            syslog(LOG_ERR, "readback EGR_VLAN_STG[1] rv=%d", rv);
        } else {
            syslog(LOG_INFO,
                   "readback EGR_VLAN_STG[1]: w0=0x%08x w1=0x%08x w2=0x%08x "
                   "w3=0x%08x w4=0x%08x",
                   stg.egr_vlan_stg[0], stg.egr_vlan_stg[1],
                   stg.egr_vlan_stg[2], stg.egr_vlan_stg[3],
                   stg.egr_vlan_stg[4]);
        }
    }

    /* FP_TCAM[256] = first programmed rule */
    {
        FP_TCAMm_t t;
        int rv = READ_FP_TCAMm(unit, 256, &t);
        if (rv < 0) {
            syslog(LOG_ERR, "readback FP_TCAM[256] rv=%d", rv);
        } else {
            uint32_t valid = FP_TCAMm_VALIDf_GET(t);
            syslog(LOG_INFO,
                   "readback FP_TCAM[256]: VALID=%u w0=0x%08x w1=0x%08x "
                   "w14=0x%08x",
                   valid, t.fp_tcam[0], t.fp_tcam[1], t.fp_tcam[14]);
        }
    }

    /* FP_POLICY_TABLE[256] */
    {
        FP_POLICY_TABLEm_t p;
        int rv = READ_FP_POLICY_TABLEm(unit, 256, &p);
        if (rv < 0) {
            syslog(LOG_ERR, "readback FP_POLICY_TABLE[256] rv=%d", rv);
        } else {
            uint32_t yc = FP_POLICY_TABLEm_Y_COPY_TO_CPUf_GET(p);
            uint32_t rc = FP_POLICY_TABLEm_R_COPY_TO_CPUf_GET(p);
            uint32_t gc = FP_POLICY_TABLEm_G_COPY_TO_CPUf_GET(p);
            syslog(LOG_INFO,
                   "readback FP_POLICY_TABLE[256]: G_COPY_TO_CPU=%u "
                   "Y_COPY_TO_CPU=%u R_COPY_TO_CPU=%u", gc, yc, rc);
        }
    }
}

int cumulus_replicate_init(void)
{
    int ioerr = 0;
    int unit = edged.unit;

    syslog(LOG_INFO, "cumulus_replicate: applying captured chip-memory state");

    ioerr += cumulus_replicate_epc_link_bmap(unit);
    ioerr += cumulus_replicate_l2_user_entry(unit);
    ioerr += cumulus_replicate_egr_vlan(unit);
    ioerr += cumulus_replicate_fp(unit);

    cumulus_enable_cmicm_irq(unit);
    cumulus_replicate_readback(unit);

    if (ioerr) {
        syslog(LOG_ERR, "cumulus_replicate: %d I/O errors", ioerr);
        return -1;
    }
    syslog(LOG_INFO, "cumulus_replicate: complete");
    return 0;
}
