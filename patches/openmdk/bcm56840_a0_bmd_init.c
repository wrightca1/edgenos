#include <bmd_config.h>
#if CDK_CONFIG_INCLUDE_BCM56840_A0 == 1

/*
 * 
 *
 * This software is governed by the Broadcom Switch APIs license.
 * This license is set out in https://raw.githubusercontent.com/Broadcom-Network-Switching-Software/OpenMDK/master/Legal/LICENSE file.
 * 
 * Copyright 2007-2020 Broadcom Inc. All rights reserved.
 */

#include <bmd/bmd.h>
#include <bmd/bmd_device.h>

#include <bmdi/arch/xgs_dma.h>

#include <cdk/chip/bcm56840_a0_defs.h>
#include <cdk/arch/xgs_chip.h>
#include <cdk/cdk_util.h>
#include <cdk/cdk_debug.h>

#include "bcm56840_a0_bmd.h"
#include "bcm56840_a0_internal.h"

#define PIPE_RESET_TIMEOUT_MSEC         5

#define JUMBO_MAXSZ                     0x3fe8

#define MMU_NUM_COS                     10
#define MMU_NUM_MC_COS                  5
#define MMU_NUM_EX_COS                  64
#define MMU_NUM_CPU_COS                 48
#define MMU_NUM_PG                      8

#define NUM_COS                         8
#define NUM_MC_COS                      1

#define MMU_TOTAL_CELLS                 (45 * 1024L)
#define MMU_ETH_FRAME_CELLS             12
#define MMU_JUMBO_FRAME_CELLS           128
#define MMU_GLOBAL_HDRM_LIMIT_CELLS     636
#define MMU_IN_PG_HDRM_CELLS            128
#define MMU_OUT_PORT_MIN_CELLS          8

#define MMU_PORT_MIN_CELLS              72
#define MMU_PORT_MIN_PACKETS            1

#define MMU_CELL_BYTES                  208
#define MMU_MAX_PKT_BYTES               (10 * 1024L)
#define MMU_PKT_HDR_BYTES               64
#define MMU_JUMBO_FRAME_BYTES           9216
#define MMU_DEFAULT_MTU_BYTES           1536

#define MMU_PG_PER_PORT                 1
#define MMU_PG_HDRM_LIMIT_CELLS         36
#define MMU_PG_HDRM_LIMIT_PKTS          36
#define MMU_PG_RESET_OFFSET_CELLS       24
#define MMU_PG_RESET_OFFSET_PKTS        1

#define MMU_OP_PORT_MIN_CELLS           12
#define MMU_OP_PORT_MIN_PKTS            1
#define MMU_OP_RESET_OFFSET_CELLS       24
#define MMU_OP_RESET_OFFSET_PKTS        2

#define MMU_SOP_POLICY                  0
#define MMU_MOP_POLICY                  7

#define TDM_BLKS_PER_PIPE               9
#define TDM_BLKS_PER_GROUP              5
#define TDM_PORTS_PER_BLK               4
#define TDM_NUM_COLUMNS                 4
#define TDM_NUM_SLOTS                   8

#define TDM_SLOT_IDLE                   -1
#define TDM_SLOT_NULL                   -2

#define FW_ALIGN_BYTES                  16
#define FW_ALIGN_MASK                   (FW_ALIGN_BYTES - 1)

static int
_port_map_init(int unit)
{
    int ioerr = 0;
    int port, lport, mport;
    int num_pport = NUM_PHYS_PORTS;
    int num_lport = NUM_LOGIC_PORTS;
    int num_mport = NUM_MMU_PORTS;
    cdk_pbmp_t pbmp;
    ING_PHYS_TO_LOGIC_MAPm_t ing_p2l;
    IFP_GM_LOGIC_TO_PHYS_MAPr_t ifp_l2p;
    EGR_LOGIC_TO_PHYS_MAPr_t egr_l2p;
    EGR_VLAN_LOGIC_TO_PHYS_MAPr_t egr_vlan_l2p;
    MMU_TO_PHYS_MAPr_t mmu_m2p;
    MMU_TO_LOGIC_MAPr_t mmu_m2l;

    bcm56840_a0_xlport_pbmp_get(unit, &pbmp);
    CDK_PBMP_PORT_ADD(pbmp, CMIC_PORT);

    /* Ingress physical to logical port mapping */
    ING_PHYS_TO_LOGIC_MAPm_CLR(ing_p2l);
    for (port = 0; port < num_pport; port++) {
        lport = P2L(unit, port);
        if (lport < 0) {
            lport = 0x7f;
        }
        ING_PHYS_TO_LOGIC_MAPm_LOGIC_PORTf_SET(ing_p2l, lport);
        ioerr += WRITE_ING_PHYS_TO_LOGIC_MAPm(unit, port, ing_p2l);
    }

    /* Ingress logical to physical port mapping */
    for (lport = 0; lport < num_lport; lport++) {
        port = L2P(unit, lport);
        if (port < 0) {
            port = 0x7f;
        }
        IFP_GM_LOGIC_TO_PHYS_MAPr_PHYS_PORTf_SET(ifp_l2p, port);
        ioerr += WRITE_IFP_GM_LOGIC_TO_PHYS_MAPr(unit, lport, ifp_l2p);
    }

    /* Egress logical to physical port mapping */
    for (lport = 0; lport < num_lport; lport++) {
        port = L2P(unit, lport);
        if (port < 0) {
            port = 0x7f;
        }
        EGR_LOGIC_TO_PHYS_MAPr_PHYS_PORTf_SET(egr_l2p, port);
        ioerr += WRITE_EGR_LOGIC_TO_PHYS_MAPr(unit, lport, egr_l2p);
        EGR_VLAN_LOGIC_TO_PHYS_MAPr_PHYS_PORTf_SET(egr_vlan_l2p, port);
        ioerr += WRITE_EGR_VLAN_LOGIC_TO_PHYS_MAPr(unit, lport, egr_vlan_l2p);
    }

    /* MMU to physical port mapping and MMU to logical port mapping */
    for (mport = 0; mport < num_mport; mport++) {
        port = M2P(unit, mport);
        if (port < 0) {
            port = 0x7f;
            lport = -1;
        } else {
            lport = P2L(unit, port);
        }
        if (lport < 0) {
            lport = 0x7f;
        }
        MMU_TO_PHYS_MAPr_PHYS_PORTf_SET(mmu_m2p, port);
        ioerr += WRITE_MMU_TO_PHYS_MAPr(unit, mport, mmu_m2p);
        MMU_TO_LOGIC_MAPr_LOGIC_PORTf_SET(mmu_m2l, lport);
        ioerr += WRITE_MMU_TO_LOGIC_MAPr(unit, mport, mmu_m2l);
    }

    return ioerr;
}

static void
_tdm_add_spacing(int target_len, int *target, int source_len,
                 int *source, int pad)
{
    int source_index, target_index;
    int result[TDM_NUM_SLOTS];

    /* Space out sequence with IDLE or NULL slot.
     *   IDLE slot occupies a space in hardware TDM table
     *   NULL slot will be removed from the merge result of this routine
     * Following is the slot usage for sprading sequence into 8 slots:
     *   40G bandwidth block: 40-40-40-40-40-40-40-40
     *   30G bandwidth block: xx-30-30-30-xx-30-30-30
     *   20G bandwidth block: xx-20-xx-20-xx-20-xx-20
     *   15G bandwidth block: xx-xx-15-xx-xx-15-xx-15
     *   10G bandwidth block: xx-xx-xx-10-xx-xx-xx-10
     *    5G bandwidth block: xx-xx-xx-xx-xx-xx-xx-5 (for 1G or 2.5G port)
     * Following is the slot usage for sprading sequence into 6 slots:
     *   30G bandwidth block: 30-30-30-30-30-30
     *   20G bandwidth block: xx-20-20-xx-20-20
     *   15G bandwidth block: xx-15-xx-15-xx-15
     *   10G bandwidth block: xx-xx-10-xx-xx-10
     *    5G bandwidth block: xx-xx-xx-xx-xx-5 (for 1G or 2.5G port)
     */
    for (target_index = 0; target_index < target_len; target_index++) {
        result[target_index] = pad;
    }
    for (source_index = 0; source_index < source_len; source_index++) {
        target_index = target_len - 1 -
            (source_len - 1 - source_index) * target_len / source_len;
        result[target_index] = source[source_index];
    }
    for (target_index = 0; target_index < target_len; target_index++) {
        target[target_index] = result[target_index];
    }
}

static void
_tdm_merge(int target_num_seq, int target_len, int source_num_seq,
           int padded_len0, int source_len0, int *seq0,
           int padded_len1, int source_len1, int *seq1,
           int padded_len2, int source_len2, int *seq2,
           int padded_len3, int source_len3, int *seq3)
{
    int padded_len[TDM_NUM_COLUMNS], source_len[TDM_NUM_COLUMNS];
    int *seq[TDM_NUM_COLUMNS];
    int adjust_seq[TDM_NUM_COLUMNS][TDM_NUM_SLOTS];
    int seq_len, *seq_ptr;
    int column, slot, count;

    padded_len[0] = padded_len0;
    padded_len[1] = padded_len1;
    padded_len[2] = padded_len2;
    padded_len[3] = padded_len3;
    source_len[0] = source_len0;
    source_len[1] = source_len1;
    source_len[2] = source_len2;
    source_len[3] = source_len3;
    seq[0] = seq0;
    seq[1] = seq1;
    seq[2] = seq2;
    seq[3] = seq3;

    for (column = 0; column < source_num_seq; column++) {
        seq_len = source_len[column];
        seq_ptr = seq[column];
        if (padded_len[column] > seq_len) {
            /* pad with evenly distributed idle slots */
            _tdm_add_spacing(padded_len[column], adjust_seq[column],
                             source_len[column], seq[column], TDM_SLOT_IDLE);
            seq_len = padded_len[column];
            seq_ptr = adjust_seq[column];
        }
        /* pad with evenly distributed null slots */
        _tdm_add_spacing(target_len, adjust_seq[column], seq_len,
                         seq_ptr, TDM_SLOT_NULL);
    }

    count = 0;
    for (slot = 0; slot < target_len; slot++) {
        for (column = 0; column < source_num_seq; column++) {
            if (adjust_seq[column][slot] != TDM_SLOT_NULL) {
                seq[count % target_num_seq][count / target_num_seq] =
                    adjust_seq[column][slot];
                count++;
            }
        }
    }
    for (; count < target_len * target_num_seq; count++) {
        seq[count % target_num_seq][count / target_num_seq] =
            TDM_SLOT_NULL;
    }
}

static int
_calculate_tdm_sequence(int unit, int pipe, int group,
                        int *tdm_seq, int *tdm_seq_len,
                        int extra_port, int extra_port_bandwidth)
{
    int group_min, group_max;
    int num_blk, blk_offset, blk_offset1;
    int num_col, num_row;
    int port, lport, idx, port_idx;
    int slot, slot_size, count, last_swap;
    int speed_max;
    int slot_count[TDM_PORTS_PER_BLK], max_slot_count;
    int blk_bandwidth[TDM_BLKS_PER_GROUP], max_blk_bandwidth;
    int blk_seq[TDM_BLKS_PER_GROUP][8], group_seq[2][32];
    int sort_blk_list[TDM_BLKS_PER_GROUP];
    int sort_blk_len[TDM_BLKS_PER_GROUP];
    int blk_count, blk_idx;

    if (pipe < 0 || pipe > 1 || group < -1 || group > 1) {
        return CDK_E_FAIL;
    }

    if (group == -1) {
        group_min = 0;
        group_max = 1;
    } else {
        group_min = group_max = group;
    }

    /* Collect port information */
    CDK_MEMSET(group_seq, -1, sizeof(group_seq));
    slot_size = 5000;
    for (group = group_min; group <= group_max; group++) {
        CDK_MEMSET(sort_blk_list, -1, sizeof(sort_blk_list));
        CDK_MEMSET(blk_bandwidth, 0, sizeof(blk_bandwidth));
        CDK_MEMSET(blk_seq, -1, sizeof(blk_seq));
        blk_count = 0;
        max_blk_bandwidth = 0;
        num_blk = group ? 4 : 5;
        blk_idx = pipe * TDM_BLKS_PER_PIPE + group * TDM_BLKS_PER_GROUP;
        for (blk_offset = 0; blk_offset < num_blk; blk_offset++) {
            port = 1 + (blk_idx + blk_offset) * TDM_PORTS_PER_BLK;
            /* Skip QGPORT (1G x 4) in XLPORT0 block (if present) */
            if (port == 1 && bcm56840_a0_has_qgport(unit)) {
                CDK_VVERB(("CONT: blk_offset = %d\n", blk_offset));
                continue;
            }

            /* Find the number of slots needed for each port in the block */
            max_slot_count = 0;
            for (port_idx = 0; port_idx < TDM_PORTS_PER_BLK; port_idx++) {
                lport = P2L(unit, port + port_idx);
                if (lport == -1) {
                    continue;
                }
                speed_max = bcm56840_a0_port_speed_max(unit, port + port_idx);
                if (speed_max == 1000 || speed_max == 2500) {
                    speed_max = 5000;
                }
                blk_bandwidth[blk_offset] += speed_max;
                slot_count[port_idx] = speed_max / slot_size;
                if (max_slot_count < slot_count[port_idx]) {
                    max_slot_count = slot_count[port_idx];
                }
            }

            if (blk_bandwidth[blk_offset] == 0) {
                CDK_VVERB(("CONT: blk_bandwidth[%d] = 0\n", blk_offset));
                continue;
            }
            if (max_blk_bandwidth < blk_bandwidth[blk_offset]) {
                max_blk_bandwidth = blk_bandwidth[blk_offset];
            }
            blk_count++;

            /* Construct per block TDM sequence */
            count = 0;
            for (slot = 0; slot < max_slot_count; slot++) {
                for (port_idx = 0; port_idx < TDM_PORTS_PER_BLK;
                     port_idx++) {
                    lport = P2L(unit, port + port_idx);
                    if (lport == -1) {
                        continue;
                    }
                    if (slot_count[port_idx] == 0) {
                        continue;
                    }
                    blk_seq[blk_offset][count] = lport;
                    CDK_VVERB(("blk_seq[%d][%d] = %d\n", blk_offset, count, lport));
                    slot_count[port_idx]--;
                    count++;
                }
            }
        }

        if (blk_count == 0) {
            continue;
        }

        /* Sort block by bandwidth, place highest bandwidth block first */
        count = 0;
        for (blk_offset = 0; blk_offset < num_blk; blk_offset++) {
            if (blk_bandwidth[blk_offset] > 0) {
                sort_blk_list[count++] = blk_offset;
            }
        }
        do {
            last_swap = 0;
            for (idx = 0; idx < count - 1; idx++) {
                blk_offset = sort_blk_list[idx];
                blk_offset1 = sort_blk_list[idx + 1];
                if (blk_bandwidth[blk_offset] >= blk_bandwidth[blk_offset1]) {
                    continue;
                }
                sort_blk_list[idx] = blk_offset1;
                sort_blk_list[idx + 1] = blk_offset;
                last_swap = idx + 1;
            }
            count = last_swap;
        } while (count > 1);

        for (blk_idx = 0; blk_idx < TDM_BLKS_PER_GROUP; blk_idx++) {
            blk_offset = sort_blk_list[blk_idx];
            sort_blk_len[blk_idx] =
                blk_offset < 0 ? 0 : blk_bandwidth[blk_offset] / slot_size;
            CDK_VVERB(("sort_blk_len[%d] = %d\n", blk_idx, sort_blk_len[blk_idx]));
        }

        if (CDK_XGS_FLAGS(unit) & CHIP_FLAG_BW640G) {
            CDK_VVERB(("CASE: BW640G\n"));
            num_col = 4;
            num_row = 8;
            if (blk_count == num_col + 1) { /* need to get rid of 1 column */
                if (sort_blk_len[3] + sort_blk_len[4] <= num_row) {
                    if (sort_blk_len[3] == 6) { /* *-*-*-30-x (x <= 10) */
                        /* merge 30, (padded) 10 into 1 column */
                        _tdm_merge(1, num_row, 2,
                                   -1, sort_blk_len[3], blk_seq[sort_blk_list[3]],
                                   2, sort_blk_len[4], blk_seq[sort_blk_list[4]],
                                   -1, 0, 0, -1, 0, 0);
                    } else { /* *-*-*-x-y (x <= 20, y <= 20) */
                        /* merge (padded) 20, (padded) 20 into 1 column */
                        _tdm_merge(1, num_row, 2,
                                   4, sort_blk_len[3], blk_seq[sort_blk_list[3]],
                                   4, sort_blk_len[4], blk_seq[sort_blk_list[4]],
                                   -1, 0, 0, -1, 0, 0);
                    }
                } else if (sort_blk_len[4] == 6) { /* *-30-30-30-30 */
                    /* merge 30, 30, 30, 30 into 3 columns */
                    _tdm_merge(3, num_row, 4,
                               -1, sort_blk_len[1], blk_seq[sort_blk_list[1]],
                               -1, sort_blk_len[2], blk_seq[sort_blk_list[2]],
                               -1, sort_blk_len[3], blk_seq[sort_blk_list[3]],
                               -1, sort_blk_len[4], blk_seq[sort_blk_list[4]]);
                } else { /* *-*-30-30-x (x == 15 || x == 20) */
                    /* merge 30, 30, (padded) 20 into 2 columns */
                    _tdm_merge(2, num_row, 3,
                               -1, sort_blk_len[2], blk_seq[sort_blk_list[2]],
                               -1, sort_blk_len[3], blk_seq[sort_blk_list[3]],
                               4, sort_blk_len[4], blk_seq[sort_blk_list[4]],
                               -1, 0, 0);
                }
            }
        } else if (max_blk_bandwidth <= 30000) {  /* 480G/320G case 1 */
            CDK_VVERB(("CASE: BW <= 30000\n"));
            num_col = 4;
            num_row = 6;
            if (blk_count == num_col + 1) { /* need to get rid of 1 column */
                if (sort_blk_len[3] + sort_blk_len[4] <= num_row) {
                    if (sort_blk_len[3] == 4) { /* *-*-*-20-x (x <= 10) */
                        /* merge 20, (padded) 10 into 1 column */
                        _tdm_merge(1, num_row, 2,
                                   -1, sort_blk_len[3], blk_seq[sort_blk_list[3]],
                                   2, sort_blk_len[4], blk_seq[sort_blk_list[4]],
                                   -1, 0, 0, -1, 0, 0);
                    } else { /* *-*-*-x-y (x <= 15, y <= 15) */
                        /* merge (padded) 15, (padded) 15 into 1 column */
                        _tdm_merge(1, num_row, 2,
                                   3, sort_blk_len[3], blk_seq[sort_blk_list[3]],
                                   3, sort_blk_len[4], blk_seq[sort_blk_list[4]],
                                   -1, 0, 0, -1, 0, 0);
                    }
                } else { /* *-*-20-20-x (x == 15 || x == 20) */
                    /* merge 20, 20, (padded) 20 into 2 columns */
                    _tdm_merge(2, num_row, 3,
                               -1, sort_blk_len[2], blk_seq[sort_blk_list[2]],
                               -1, sort_blk_len[3], blk_seq[sort_blk_list[3]],
                               4, sort_blk_len[4], blk_seq[sort_blk_list[4]],
                               -1, 0, 0);
                }
            }
        } else { /* 480G/320G case 2 */
            CDK_VVERB(("CASE: otherwise\n"));
            num_col = 3;
            num_row = 8;
            if (blk_count == (num_col + 1)) { /* need to get rid of 1 column */
                CDK_VVERB(("CASE: num_col + 1\n"));
                if (sort_blk_len[2] + sort_blk_len[3] <= num_row) {
                    if (sort_blk_len[2] == 6) { /* 40-*-30-x (x <= 10) */
                        /* merge 30, (padded) 10 into 1 column */
                        _tdm_merge(1, num_row, 2,
                                   -1, sort_blk_len[2], blk_seq[sort_blk_list[2]],
                                   2, sort_blk_len[3], blk_seq[sort_blk_list[3]],
                                   -1, 0, 0, -1, 0, 0);
                    } else { /* 40-*-x-y (x <= 20, y <= 20) */
                        /* merge (padded) 20, (padded) 20 into 1 column */
                        _tdm_merge(1, num_row, 2,
                                   4, sort_blk_len[2], blk_seq[sort_blk_list[2]],
                                   4, sort_blk_len[3], blk_seq[sort_blk_list[3]],
                                   -1, 0, 0, -1, 0, 0);
                    }
                } else { /* 40-30-30-x (x == 15 || x == 20) */
                    /* merge 30, 30, (padded) 20 into 2 columns */
                    _tdm_merge(2, num_row, 3,
                               -1, sort_blk_len[1], blk_seq[sort_blk_list[1]],
                               -1, sort_blk_len[2], blk_seq[sort_blk_list[2]],
                               4, sort_blk_len[3], blk_seq[sort_blk_list[3]],
                               -1, 0, 0);
                }
            } else if (blk_count == (num_col + 2)) {
                CDK_VVERB(("CASE: num_col + 2\n"));
                /* need to get rid of 2 columns */
                if (sort_blk_len[2] + sort_blk_len[3] + sort_blk_len[4] <=
                    num_row) {
                    if (sort_blk_len[2] >= 4) { /* 40-*-x-y-z (x >= 20) */
                        /* first merge columns 3 and 4 into 1 column */
                        _tdm_merge(1, num_row, 2,
                                   -1, sort_blk_len[3], blk_seq[sort_blk_list[3]],
                                   -1, sort_blk_len[4], blk_seq[sort_blk_list[4]],
                                   -1, 0, 0, -1, 0, 0);
                        /* then merge columns 2 and merged 3+4 into 1 column */
                        _tdm_merge(1, num_row, 2,
                                   -1, sort_blk_len[2], blk_seq[sort_blk_list[2]],
                                   -1, sort_blk_len[3] + sort_blk_len[4],
                                   blk_seq[sort_blk_list[3]],
                                   -1, 0, 0, -1, 0, 0);
                    } else { /* 40-*-x-y-z (x < 20) */
                        /* directly merge all 3 columns into 1 column */
                        _tdm_merge(1, num_row, 3,
                                   -1, sort_blk_len[2], blk_seq[sort_blk_list[2]],
                                   -1, sort_blk_len[3], blk_seq[sort_blk_list[3]],
                                   -1, sort_blk_len[4], blk_seq[sort_blk_list[4]],
                                   -1, 0, 0);
                    }
                } else if (sort_blk_len[1] + sort_blk_len[4] <= 8  &&
                           sort_blk_len[2] + sort_blk_len[3] <= 8) {
                    /* merge columns 1 and 4 into 1 column */
                    _tdm_merge(1, num_row, 2,
                               -1, sort_blk_len[1], blk_seq[sort_blk_list[1]],
                               -1, sort_blk_len[4], blk_seq[sort_blk_list[4]],
                               -1, 0, 0, -1, 0, 0);
                    /* merge columns 2 and 3 into 1 column */
                    _tdm_merge(1, num_row, 2,
                               -1, sort_blk_len[2], blk_seq[sort_blk_list[2]],
                               -1, sort_blk_len[3], blk_seq[sort_blk_list[3]],
                               -1, 0, 0, -1, 0, 0);
                } else if (sort_blk_len[2] == 6) { /* 40-30-30-15-5 */
                    /* first merge columns 3 and 4 into 1 column */
                    _tdm_merge(1, num_row, 2,
                               -1, sort_blk_len[3], blk_seq[sort_blk_list[3]],
                               -1, sort_blk_len[4], blk_seq[sort_blk_list[4]],
                               -1, 0, 0, -1, 0, 0);
                    /* then merge 30, 30, merged 3+4 into 2 columns */
                    _tdm_merge(2, num_row, 3,
                               -1, sort_blk_len[1], blk_seq[sort_blk_list[1]],
                               -1, sort_blk_len[2], blk_seq[sort_blk_list[2]],
                               -1, sort_blk_len[3] + sort_blk_len[4],
                               blk_seq[sort_blk_list[3]],
                               -1, 0, 0);
                } else { /* 40-30-x-15-15 (x == 15 || x == 20) */
                    /* first merge columns 3 and 4 into 1 column */
                    _tdm_merge(1, num_row, 2,
                               -1, sort_blk_len[3], blk_seq[sort_blk_list[3]],
                               -1, sort_blk_len[4], blk_seq[sort_blk_list[4]],
                               -1, 0, 0, -1, 0, 0);
                    /* then merge 30, merged 3+4, (padded) 20 into 2 columns */
                    _tdm_merge(2, num_row, 3,
                               -1, sort_blk_len[1], blk_seq[sort_blk_list[1]],
                               -1, sort_blk_len[3] + sort_blk_len[4],
                               blk_seq[sort_blk_list[3]],
                               4, sort_blk_len[2], blk_seq[sort_blk_list[2]],
                               -1, 0, 0);
                }
            }
        }

        if (blk_count <= num_col) {
            /* If no merge was done, just evenly distributed the sequence
             * in each block */
            for (blk_idx = 0; blk_idx < num_col; blk_idx++) {
                blk_offset = sort_blk_list[blk_idx];
                if (blk_offset < 0) {
                    break;
                }
                _tdm_add_spacing(num_row, blk_seq[blk_offset],
                                 sort_blk_len[blk_idx],
                                 blk_seq[blk_offset], TDM_SLOT_IDLE);
            }
        }

        for (blk_idx = 0; blk_idx < num_col; blk_idx++) {
            blk_offset = sort_blk_list[blk_idx];
            if (blk_offset < 0) {
                break;
            }
            for (idx = 0; idx < num_row; idx++) {
                group_seq[group][idx * num_col + blk_idx] =
                    blk_seq[blk_offset][idx];
            }
        }
    }

    /* Construct final TDM sequence from 2 group TDM sequences */
    count = (CDK_XGS_FLAGS(unit) & CHIP_FLAG_BW640G) ? 32 : 24;
    idx = 0;
    for (slot = 0; slot < count; slot++) {
        for (group = group_min; group <= group_max; group++) {
            tdm_seq[idx++] = group_seq[group][slot];
        }
        if (extra_port != -1) {
            if (slot == count - 1 ||
                (extra_port_bandwidth == 10000 && slot == count / 2 - 1)) {
                tdm_seq[idx++] = extra_port;
            }
        }
    }
    *tdm_seq_len = idx;

    return CDK_E_NONE;
}

static int
_mmu_tdm_init(int unit)
{
    int rv;
    int pipe, base;
    int port, lport, mport, extra_port;
    int idx;
    ARB_TDM_TABLE_0m_t arb_tdm0;
    ARB_TDM_TABLE_1m_t arb_tdm1;
    int tdm_seq[128], tdm_seq_len;
    uint32_t bw;

    /* Set bandwidth adjustment */
    bw = 10000;
    if (CDK_XGS_FLAGS(unit) & CHIP_FLAG_BW640G) {
        bw = 5000;
    }

    for (pipe = 0; pipe < 2; pipe++) {
        if (pipe == 0) { /* X pipe */
            extra_port = 0;
            base = 0;
        } else { /* Y pipe */
            extra_port = -1;
            base = 33;
        }
        rv = _calculate_tdm_sequence(unit, pipe, -1,
                                     tdm_seq, &tdm_seq_len, extra_port, bw);
        if (CDK_FAILURE(rv)) {
            return rv;
        }

        CDK_VERB(("MMU_%c TDM:", pipe ? 'Y' : 'X'));
        for (idx = 0; idx < tdm_seq_len; idx++) {
            if (idx % 16 == 0) {
                CDK_VERB(("\n    "));
            }
            CDK_VERB((" %2d", tdm_seq[idx]));
        }
        CDK_VERB(("\n"));

        if (pipe == 0) {
            ARB_TDM_TABLE_0m_CLR(arb_tdm0);
            for (idx = 0; idx < tdm_seq_len; idx++) {
                lport = tdm_seq[idx];
                mport = 0x3f;
                if (lport >= 0) {
                    port = L2P(unit, lport);
                    if (port >= 0) {
                        mport = P2M(unit, port) - base;
                    }
                }
                ARB_TDM_TABLE_0m_PORT_NUMf_SET(arb_tdm0, mport);
                if (idx == tdm_seq_len - 1) {
                    ARB_TDM_TABLE_0m_WRAP_ENf_SET(arb_tdm0, 1);
                }
                WRITE_ARB_TDM_TABLE_0m(unit, idx, arb_tdm0);
            }
        } else {
            ARB_TDM_TABLE_1m_CLR(arb_tdm1);
            for (idx = 0; idx < tdm_seq_len; idx++) {
                lport = tdm_seq[idx];
                mport = 0x3f;
                if (lport >= 0) {
                    port = L2P(unit, lport);
                    if (port >= 0) {
                        mport = P2M(unit, port) - base;
                    }
                }
                ARB_TDM_TABLE_1m_PORT_NUMf_SET(arb_tdm1, mport);
                if (idx == tdm_seq_len - 1) {
                    ARB_TDM_TABLE_1m_WRAP_ENf_SET(arb_tdm1, 1);
                }
                WRITE_ARB_TDM_TABLE_1m(unit, idx, arb_tdm1);
            }
        }
    }

    return CDK_E_NONE;
}

static int
_mmu_init(int unit)
{
    int ioerr = 0;
    int rv = CDK_E_NONE;
    ES_TDM_CONFIGr_t es_tdm_cfg;
    PORT_MAX_PKT_SIZEr_t max_pkt_sz;
    PORT_PRI_GRP0r_t port_pri_grp0;
    PORT_PRI_GRP1r_t port_pri_grp1;
    PORT_PRI_XON_ENABLEr_t xon_enable;
    PORT_MIN_CELLr_t port_min_cell;
    PORT_MIN_PG_ENABLEr_t port_min_pg_en;
    USE_SP_SHAREDr_t use_sp_shared;
    BUFFER_CELL_LIMIT_SPr_t buf_cell_limit;
    CELL_RESET_LIMIT_OFFSET_SPr_t cell_reset_limit;
    PORT_MAX_SHARED_CELLr_t max_shared_cell;
    PORT_RESUME_LIMIT_CELLr_t resume_limit_cell;
    PORT_SHARED_MAX_PG_ENABLEr_t shr_max_pg_en;
    PG_SHARED_LIMIT_CELLr_t shr_limit_cell;
    PG_RESET_OFFSET_CELLr_t reset_offset_cell;
    PG_HDRM_LIMIT_CELLr_t hdrm_limit_cell;
    GLOBAL_HDRM_LIMITr_t global_hdrm_limit;
    MMU_THDO_CONFIG_0m_t thdo_cfg0;
    MMU_THDO_OFFSET_0m_t thdo_offset0;
    MMU_THDO_CONFIG_1m_t thdo_cfg1;
    MMU_THDO_OFFSET_1m_t thdo_offset1;
    MMU_THDO_CONFIG_EX_0m_t thdo_cfgx0;
    MMU_THDO_OFFSET_EX_0m_t thdo_offsetx0;
    MMU_THDO_CONFIG_EX_1m_t thdo_cfgx1;
    MMU_THDO_OFFSET_EX_1m_t thdo_offsetx1;
    OP_QUEUE_CONFIG_CELLr_t opq_cfg_cell;
    OP_QUEUE_CONFIG1_CELLr_t opq_cfg1_cell;
    OP_QUEUE_RESET_OFFSET_CELLr_t opq_rst_offs_cell;
    OP_THR_CONFIGr_t op_thr_cfg;
    OP_BUFFER_SHARED_LIMIT_CELLr_t ob_shr_limit_cell;
    OP_BUFFER_SHARED_LIMIT_RESUME_CELLr_t ob_shr_resume_cell;
    OP_UC_PORT_CONFIG_CELLr_t op_uc_cfg_cell;
    OP_PORT_CONFIG_CELLr_t op_cfg_cell;
    OVQ_DROP_THRESHOLD0r_t ovq_drth;
    OVQ_DROP_THRESHOLD_RESET_LIMITr_t ovq_drth_rst_limit;
    OVQ_MCQ_CREDITSr_t mcq_credits;
    OVQ_FLOWCONTROL_THRESHOLDr_t ovq_fc_th;
    MCQ_FIFO_BASE_REGr_t mcq_fifo_base;
    INPUT_PORT_RX_ENABLE0_64r_t inp_rx_en0;
    OUTPUT_PORT_RX_ENABLE0_64r_t outp_rx_en0;
    INPUT_PORT_RX_ENABLE1_64r_t inp_rx_en1;
    OUTPUT_PORT_RX_ENABLE1_64r_t outp_rx_en1;
    S3_CONFIGr_t s3_cfg;
    S3_CONFIG_MCr_t s3_cfg_mc;
    S2_CONFIGr_t s2_cfg;
    S2_S3_ROUTINGr_t s2_s3_routing0;
    S2_S3_ROUTINGr_t s2_s3_routing1;
    S2_S3_ROUTINGr_t s2_s3_routing2;
    cdk_pbmp_t mmu_pbmp, pbmp;
    uint32_t xlport0_bw;
    uint32_t pg_pbm;
    uint32_t fval;
    uint32_t max_packet_cells, jumbo_frame_cells, default_mtu_cells;
    uint32_t total_cells, in_reserved_cells, out_reserved_cells;
    int num_lanes, num_port, num_cosq;
    int port, mport, base, idx;

    /* Setup TDM for MMU */
    rv = _mmu_tdm_init(unit);

    /* Get front-panel ports */
    bcm56840_a0_xlport_pbmp_get(unit, &pbmp);

    /* Get MMU ports */
    bcm56840_a0_xlport_pbmp_get(unit, &mmu_pbmp);
    CDK_PBMP_PORT_ADD(mmu_pbmp, CMIC_PORT);

    /* Calculate max bandwidth for XLPORT0 */
    xlport0_bw = 0;
    CDK_PBMP_ITER(pbmp, port) {
        if (XLPORT_BLKIDX(port) > 0) {
            break;
        }
        xlport0_bw += bcm56840_a0_port_speed_max(unit, port);
    }

    /* Enable QGPORT (1G x 4) in XLPORT0 block */
    ES_TDM_CONFIGr_CLR(es_tdm_cfg);
    CDK_PBMP_ITER(pbmp, port) {
        if (xlport0_bw == 0 || xlport0_bw > 4000) {
            break;
        }
        if (XLPORT_BLKIDX(port) > 0) {
            break;
        }
        mport = P2M(unit, port);
        if (mport < 0) {
            mport = 0x3f;
        } else {
            ES_TDM_CONFIGr_EN_CPU_SLOT_SHARINGf_SET(es_tdm_cfg, 1);
        }
        idx = XLPORT_SUBPORT(port);
        if (idx == 0) {
            ES_TDM_CONFIGr_GB_PORT0f_SET(es_tdm_cfg, mport);
        } else if (idx == 1) {
            ES_TDM_CONFIGr_GB_PORT1f_SET(es_tdm_cfg, mport);
        } else if (idx == 2) {
            ES_TDM_CONFIGr_GB_PORT2f_SET(es_tdm_cfg, mport);
        } else if (idx == 3) {
            ES_TDM_CONFIGr_GB_PORT3f_SET(es_tdm_cfg, mport);
        }
    }
    ioerr += WRITE_ES_TDM_CONFIGr(unit, es_tdm_cfg);

    /* Number of front-panel ports */
    num_port = 0;
    CDK_PBMP_ITER(pbmp, port) {
        num_port++;
    }
    /* Include CPU port in COS count */
    num_cosq = (num_port + 1) * (NUM_COS + NUM_MC_COS);

    max_packet_cells =
        (MMU_MAX_PKT_BYTES + MMU_PKT_HDR_BYTES + MMU_CELL_BYTES - 1) /
        MMU_CELL_BYTES;
    jumbo_frame_cells =
        (MMU_JUMBO_FRAME_BYTES + MMU_PKT_HDR_BYTES + MMU_CELL_BYTES - 1) /
        MMU_CELL_BYTES;
    default_mtu_cells =
        (MMU_DEFAULT_MTU_BYTES + MMU_PKT_HDR_BYTES + MMU_CELL_BYTES - 1) /
        MMU_CELL_BYTES;

    /*
     * Input port pool allocation precedence:
     *   reserved space: per-port per-PG minimum space
     *   reserved space: per-port minimum space (include cpu port)
     *   shared space = total - input port reserved - output port reserved
     *   reserved space: per-port per-PG headroom
     *   reserved space: per-device global headroom
     * Output port:
     *   reserved space: per-port per-queue minimum space
    *   shared space = total - output port reserved
     */
    total_cells = MMU_TOTAL_CELLS;
    in_reserved_cells = (num_port + 1) * jumbo_frame_cells +
        num_port * MMU_PG_PER_PORT * MMU_IN_PG_HDRM_CELLS +
        num_port * default_mtu_cells;
    out_reserved_cells = num_cosq * MMU_OUT_PORT_MIN_CELLS;

    pg_pbm = 0;
    for (idx = 8 - MMU_PG_PER_PORT; idx < 8; idx++) {
        pg_pbm |= LSHIFT32(1, idx);
    }

    /* Input port misc per-port setting */
    PORT_MAX_PKT_SIZEr_CLR(max_pkt_sz);
    PORT_MAX_PKT_SIZEr_PORT_MAX_PKT_SIZEf_SET(max_pkt_sz, max_packet_cells);

    idx = MMU_NUM_PG - 1;
    PORT_PRI_GRP0r_CLR(port_pri_grp0);
    PORT_PRI_GRP0r_PRI0_GRPf_SET(port_pri_grp0, idx);
    PORT_PRI_GRP0r_PRI1_GRPf_SET(port_pri_grp0, idx);
    PORT_PRI_GRP0r_PRI2_GRPf_SET(port_pri_grp0, idx);
    PORT_PRI_GRP0r_PRI3_GRPf_SET(port_pri_grp0, idx);
    PORT_PRI_GRP0r_PRI4_GRPf_SET(port_pri_grp0, idx);
    PORT_PRI_GRP0r_PRI5_GRPf_SET(port_pri_grp0, idx);
    PORT_PRI_GRP0r_PRI6_GRPf_SET(port_pri_grp0, idx);
    PORT_PRI_GRP0r_PRI7_GRPf_SET(port_pri_grp0, idx);

    PORT_PRI_GRP1r_CLR(port_pri_grp1);
    PORT_PRI_GRP1r_PRI8_GRPf_SET(port_pri_grp1, idx);
    PORT_PRI_GRP1r_PRI9_GRPf_SET(port_pri_grp1, idx);
    PORT_PRI_GRP1r_PRI10_GRPf_SET(port_pri_grp1, idx);
    PORT_PRI_GRP1r_PRI11_GRPf_SET(port_pri_grp1, idx);
    PORT_PRI_GRP1r_PRI12_GRPf_SET(port_pri_grp1, idx);
    PORT_PRI_GRP1r_PRI13_GRPf_SET(port_pri_grp1, idx);
    PORT_PRI_GRP1r_PRI14_GRPf_SET(port_pri_grp1, idx);
    PORT_PRI_GRP1r_PRI15_GRPf_SET(port_pri_grp1, idx);

    PORT_PRI_XON_ENABLEr_CLR(xon_enable);
    PORT_PRI_XON_ENABLEr_SET(xon_enable, 1);

    CDK_PBMP_ITER(mmu_pbmp, port) {
        mport = P2M(unit, port);
        ioerr += WRITE_PORT_MAX_PKT_SIZEr(unit, mport, max_pkt_sz);
        ioerr += WRITE_PORT_PRI_GRP0r(unit, mport, port_pri_grp0);
        ioerr += WRITE_PORT_PRI_GRP1r(unit, mport, port_pri_grp1);
        if (port == CMIC_PORT) {
            continue;
        }
        ioerr += WRITE_PORT_PRI_XON_ENABLEr(unit, mport, xon_enable);
    }

    /* Input port per-port per-PG minimum space (use reset value 0) */

    /* Input port per-port minimum space */
    PORT_MIN_CELLr_CLR(port_min_cell);
    PORT_MIN_CELLr_PORT_MINf_SET(port_min_cell, jumbo_frame_cells);
    PORT_MIN_PG_ENABLEr_CLR(port_min_pg_en);
    PORT_MIN_PG_ENABLEr_PG_BMPf_SET(port_min_pg_en, pg_pbm);
    CDK_PBMP_ITER(mmu_pbmp, port) {
        mport = P2M(unit, port);
        ioerr += WRITE_PORT_MIN_CELLr(unit, mport, port_min_cell);
        ioerr += WRITE_PORT_MIN_PG_ENABLEr(unit, mport, port_min_pg_en);
    }

    /* Input port shared space (use service pool 0 only) */
    USE_SP_SHAREDr_CLR(use_sp_shared);
    USE_SP_SHAREDr_ENABLEf_SET(use_sp_shared, 1);
    ioerr += WRITE_USE_SP_SHAREDr(unit, use_sp_shared);

    BUFFER_CELL_LIMIT_SPr_CLR(buf_cell_limit);
    fval = total_cells - in_reserved_cells - out_reserved_cells;
    BUFFER_CELL_LIMIT_SPr_LIMITf_SET(buf_cell_limit, fval);
    ioerr += WRITE_BUFFER_CELL_LIMIT_SPr(unit, 0, buf_cell_limit);

    CELL_RESET_LIMIT_OFFSET_SPr_CLR(cell_reset_limit);
    fval = 30 * default_mtu_cells;
    CELL_RESET_LIMIT_OFFSET_SPr_OFFSETf_SET(cell_reset_limit, fval);
    ioerr += WRITE_CELL_RESET_LIMIT_OFFSET_SPr(unit, 0, cell_reset_limit);

    /* Input port per-port shared space limit and reset offset */
    PORT_MAX_SHARED_CELLr_CLR(max_shared_cell);
    PORT_MAX_SHARED_CELLr_PORT_MAXf_SET(max_shared_cell, total_cells);
    PORT_RESUME_LIMIT_CELLr_CLR(resume_limit_cell);
    fval = total_cells - (default_mtu_cells * 2);
    PORT_RESUME_LIMIT_CELLr_CELLSf_SET(resume_limit_cell, fval);
    CDK_PBMP_ITER(mmu_pbmp, port) {
        mport = P2M(unit, port);
        ioerr += WRITE_PORT_MAX_SHARED_CELLr(unit, mport, max_shared_cell);
        ioerr += WRITE_PORT_RESUME_LIMIT_CELLr(unit, mport, resume_limit_cell);
    }

    /* Input port per-port per-PG shared space limit and reset offset */
    PORT_SHARED_MAX_PG_ENABLEr_CLR(shr_max_pg_en);
    PORT_SHARED_MAX_PG_ENABLEr_PG_BMPf_SET(shr_max_pg_en, pg_pbm);
    PG_SHARED_LIMIT_CELLr_CLR(shr_limit_cell);
    PG_SHARED_LIMIT_CELLr_PG_SHARED_LIMITf_SET(shr_limit_cell, 5);
    PG_SHARED_LIMIT_CELLr_PG_SHARED_DYNAMICf_SET(shr_limit_cell, 1);
    PG_RESET_OFFSET_CELLr_CLR(reset_offset_cell);
    fval = default_mtu_cells * 2;
    PG_RESET_OFFSET_CELLr_PG_RESET_OFFSETf_SET(reset_offset_cell, fval);
    CDK_PBMP_ITER(mmu_pbmp, port) {
        mport = P2M(unit, port);
        ioerr += WRITE_PORT_SHARED_MAX_PG_ENABLEr(unit, mport, shr_max_pg_en);
        for (idx = 8 - MMU_PG_PER_PORT; idx < 8; idx++) {
            ioerr += WRITE_PG_SHARED_LIMIT_CELLr(unit, mport, idx,
                                                 shr_limit_cell);
            ioerr += WRITE_PG_RESET_OFFSET_CELLr(unit, mport, idx,
                                                 reset_offset_cell);
        }
    }

    /* Input port per-port per-PG headroom
     * Use only 1 PG (highest priority PG for the port) */
    PG_HDRM_LIMIT_CELLr_CLR(hdrm_limit_cell);
    PG_HDRM_LIMIT_CELLr_PG_GEf_SET(hdrm_limit_cell, 1);
    fval = MMU_IN_PG_HDRM_CELLS;
    CDK_PBMP_ITER(mmu_pbmp, port) {
        mport = P2M(unit, port);
        for (idx = 0; idx < 8; idx++) {
            if (idx < 8 - MMU_PG_PER_PORT) {
                PG_HDRM_LIMIT_CELLr_PG_HDRM_LIMITf_SET(hdrm_limit_cell, 0);
            } else {
                PG_HDRM_LIMIT_CELLr_PG_HDRM_LIMITf_SET(hdrm_limit_cell, fval);
            }
            ioerr += WRITE_PG_HDRM_LIMIT_CELLr(unit, mport, idx,
                                               hdrm_limit_cell);
        }
    }

    /* Input port per-device global headroom */
    GLOBAL_HDRM_LIMITr_CLR(global_hdrm_limit);
    fval = num_port * default_mtu_cells;
    GLOBAL_HDRM_LIMITr_GLOBAL_HDRM_LIMITf_SET(global_hdrm_limit, fval);
    ioerr += WRITE_GLOBAL_HDRM_LIMITr(unit, global_hdrm_limit);

    /* Output port per-normal-port per-queue minimum space (unicast, X pipe) */
    MMU_THDO_CONFIG_0m_CLR(thdo_cfg0);
    MMU_THDO_CONFIG_0m_Q_SHARED_ALPHA_CELLf_SET(thdo_cfg0, 5);
    MMU_THDO_CONFIG_0m_Q_MIN_CELLf_SET(thdo_cfg0, default_mtu_cells);
    MMU_THDO_CONFIG_0m_Q_LIMIT_DYNAMIC_CELLf_SET(thdo_cfg0, 1);
    MMU_THDO_OFFSET_0m_CLR(thdo_offset0);
    MMU_THDO_OFFSET_0m_RESET_OFFSET_CELLf_SET(thdo_offset0, 1);
    CDK_PBMP_ITER(mmu_pbmp, port) {
        /* X pipe only */
        if (port > 36) {
            break;
        }
        mport = P2M(unit, port);
        /* Skip ports with extended COS queues */
        if (mport < 5) {
            continue;
        }
        /* 10 COS queues per standard MMU port */
        base = (mport - 5) * MMU_NUM_COS;
        for (idx = 0; idx < NUM_COS; idx++) {
            ioerr += WRITE_MMU_THDO_CONFIG_0m(unit, base + idx, thdo_cfg0);
            ioerr += WRITE_MMU_THDO_OFFSET_0m(unit, base + idx, thdo_offset0);
        }
    }

    /* Output port per-normal-port per-queue minimum space (unicast, Y pipe) */
    MMU_THDO_CONFIG_1m_CLR(thdo_cfg1);
    MMU_THDO_CONFIG_1m_Q_SHARED_ALPHA_CELLf_SET(thdo_cfg1, 5);
    MMU_THDO_CONFIG_1m_Q_MIN_CELLf_SET(thdo_cfg1, default_mtu_cells);
    MMU_THDO_CONFIG_1m_Q_LIMIT_DYNAMIC_CELLf_SET(thdo_cfg1, 1);
    MMU_THDO_OFFSET_1m_CLR(thdo_offset1);
    MMU_THDO_OFFSET_1m_RESET_OFFSET_CELLf_SET(thdo_offset1, 1);
    CDK_PBMP_ITER(mmu_pbmp, port) {
        /* Y pipe only */
        if (port < 37) {
            continue;
        }
        mport = P2M(unit, port);
        /* Skip ports with extended COS queues */
        if (mport < 38) {
            continue;
        }
        /* 10 COS queues per standard MMU port */
        base = (mport - 5) * MMU_NUM_COS;
        for (idx = 0; idx < NUM_COS; idx++) {
            ioerr += WRITE_MMU_THDO_CONFIG_1m(unit, base + idx, thdo_cfg1);
            ioerr += WRITE_MMU_THDO_OFFSET_1m(unit, base + idx, thdo_offset1);
        }
    }

    /* Output port per extended-queue port per-queue minimum space (X pipe) */
    MMU_THDO_CONFIG_EX_0m_CLR(thdo_cfgx0);
    MMU_THDO_CONFIG_EX_0m_Q_SHARED_ALPHA_CELLf_SET(thdo_cfgx0, 5);
    MMU_THDO_CONFIG_EX_0m_Q_MIN_CELLf_SET(thdo_cfgx0, default_mtu_cells);
    MMU_THDO_CONFIG_EX_0m_Q_LIMIT_DYNAMIC_CELLf_SET(thdo_cfgx0, 1);
    MMU_THDO_OFFSET_EX_0m_CLR(thdo_offsetx0);
    MMU_THDO_OFFSET_EX_0m_RESET_OFFSET_CELLf_SET(thdo_offsetx0, 1);
    CDK_PBMP_ITER(mmu_pbmp, port) {
        /* X pipe only */
        if (port > 36) {
            break;
        }
        mport = P2M(unit, port);
        /* Skip CMIC and ports with normal COS queues */
        if (mport < 1 || mport >= 5) {
            continue;
        }
        /*
         * 74 entries per port:
         *     extended unicast queues: index 0-63 (not used)
         *     normal unicast queue: index 64-73
         */
        base = ((mport - 1) * (MMU_NUM_EX_COS + MMU_NUM_COS)) + MMU_NUM_EX_COS;
        for (idx = 0; idx < NUM_COS; idx++) {
            ioerr += WRITE_MMU_THDO_CONFIG_EX_0m(unit, base + idx, thdo_cfgx0);
            ioerr += WRITE_MMU_THDO_OFFSET_EX_0m(unit, base + idx, thdo_offsetx0);
        }
    }

    /* Output port per extended-queue port per-queue minimum space (Y pipe) */
    MMU_THDO_CONFIG_EX_1m_CLR(thdo_cfgx1);
    MMU_THDO_CONFIG_EX_1m_Q_SHARED_ALPHA_CELLf_SET(thdo_cfgx1, 5);
    MMU_THDO_CONFIG_EX_1m_Q_MIN_CELLf_SET(thdo_cfgx1, default_mtu_cells);
    MMU_THDO_CONFIG_EX_1m_Q_LIMIT_DYNAMIC_CELLf_SET(thdo_cfgx1, 1);
    MMU_THDO_OFFSET_EX_1m_CLR(thdo_offsetx1);
    MMU_THDO_OFFSET_EX_1m_RESET_OFFSET_CELLf_SET(thdo_offsetx1, 1);
    CDK_PBMP_ITER(mmu_pbmp, port) {
        /* Y pipe only */
        if (port < 37) {
            continue;
        }
        mport = P2M(unit, port);
        /* Skip LB port and ports with normal COS queues */
        if (mport < 34 || mport >= 38) {
            continue;
        }
        /*
         * 74 entries per port:
         *     extended unicast queues: index 0-63 (not used)
         *     normal unicast queue: index 64-73
         */
        base = ((mport - 34) * (MMU_NUM_EX_COS + MMU_NUM_COS)) + MMU_NUM_EX_COS;
        for (idx = 0; idx < NUM_COS; idx++) {
            ioerr += WRITE_MMU_THDO_CONFIG_EX_1m(unit, base + idx, thdo_cfgx1);
            ioerr += WRITE_MMU_THDO_OFFSET_EX_1m(unit, base + idx, thdo_offsetx1);
        }
    }

    /* Output port per-port per-queue minimum space (multicast) */
    OP_QUEUE_CONFIG_CELLr_CLR(opq_cfg_cell);
    OP_QUEUE_CONFIG_CELLr_Q_SHARED_ALPHA_CELLf_SET(opq_cfg_cell, 5);
    OP_QUEUE_CONFIG_CELLr_Q_MIN_CELLf_SET(opq_cfg_cell, default_mtu_cells);
    OP_QUEUE_CONFIG1_CELLr_CLR(opq_cfg1_cell);
    OP_QUEUE_CONFIG1_CELLr_Q_LIMIT_DYNAMIC_CELLf_SET(opq_cfg1_cell, 1);
    OP_QUEUE_RESET_OFFSET_CELLr_CLR(opq_rst_offs_cell);
    OP_QUEUE_RESET_OFFSET_CELLr_Q_RESET_OFFSET_CELLf_SET(opq_rst_offs_cell, 1);
    CDK_PBMP_ITER(mmu_pbmp, port) {
        mport = P2M(unit, port);
        for (idx = 0; idx < NUM_MC_COS; idx++) {
            ioerr += WRITE_OP_QUEUE_CONFIG_CELLr(unit, mport, idx,
                                                 opq_cfg_cell);
            ioerr += WRITE_OP_QUEUE_CONFIG1_CELLr(unit, mport, idx,
                                                  opq_cfg1_cell);
            ioerr += WRITE_OP_QUEUE_RESET_OFFSET_CELLr(unit, mport, idx,
                                                       opq_rst_offs_cell);
        }
    }

    /* Output port shared space per-chip settings */
    OP_THR_CONFIGr_CLR(op_thr_cfg);
    OP_THR_CONFIGr_MOP_POLICYf_SET(op_thr_cfg, 7);
    OP_THR_CONFIGr_ASF_PKT_SIZEf_SET(op_thr_cfg, 3);
    OP_THR_CONFIGr_ASF_QUEUE_SIZEf_SET(op_thr_cfg, 3);
    OP_THR_CONFIGr_MOP_POLICYf_SET(op_thr_cfg, 7);
    ioerr += WRITE_OP_THR_CONFIGr(unit, op_thr_cfg);

    OP_BUFFER_SHARED_LIMIT_CELLr_CLR(ob_shr_limit_cell);
    OP_BUFFER_SHARED_LIMIT_CELLr_SET(ob_shr_limit_cell,
                                     total_cells - out_reserved_cells);
    ioerr += WRITE_OP_BUFFER_SHARED_LIMIT_CELLr(unit, 0, ob_shr_limit_cell);

    OP_BUFFER_SHARED_LIMIT_RESUME_CELLr_CLR(ob_shr_resume_cell);
    OP_BUFFER_SHARED_LIMIT_RESUME_CELLr_SET(ob_shr_resume_cell,
                                            total_cells - out_reserved_cells);
    ioerr += WRITE_OP_BUFFER_SHARED_LIMIT_RESUME_CELLr(unit, 0,
                                                       ob_shr_resume_cell);

    /* Output port shared space unicast per-port limit */
    OP_UC_PORT_CONFIG_CELLr_CLR(op_uc_cfg_cell);
    fval = total_cells - out_reserved_cells;
    OP_UC_PORT_CONFIG_CELLr_OP_SHARED_LIMIT_CELLf_SET(op_uc_cfg_cell, fval);
    fval = total_cells - out_reserved_cells - default_mtu_cells;
    OP_UC_PORT_CONFIG_CELLr_OP_SHARED_RESET_VALUE_CELLf_SET(op_uc_cfg_cell, fval);

    CDK_PBMP_ITER(mmu_pbmp, port) {
        if (port == CMIC_PORT) {
            continue;
        }
        mport = P2M(unit, port);
        ioerr += WRITE_OP_UC_PORT_CONFIG_CELLr(unit, mport, 0, op_uc_cfg_cell);
    }

    /* Output port shared space multicast per-port limit */
    OP_PORT_CONFIG_CELLr_CLR(op_cfg_cell);
    fval = total_cells - out_reserved_cells;
    OP_PORT_CONFIG_CELLr_OP_SHARED_LIMIT_CELLf_SET(op_cfg_cell, fval);
    fval = total_cells - out_reserved_cells - default_mtu_cells;
    OP_PORT_CONFIG_CELLr_OP_SHARED_RESET_VALUE_CELLf_SET(op_cfg_cell, fval);
    CDK_PBMP_ITER(mmu_pbmp, port) {
        mport = P2M(unit, port);
        ioerr += WRITE_OP_PORT_CONFIG_CELLr(unit, mport, 0, op_cfg_cell);
    }

    OVQ_DROP_THRESHOLD0r_CLR(ovq_drth);
    fval = total_cells / 4;
    OVQ_DROP_THRESHOLD0r_OVQ_DROP_THRESHOLD0f_SET(ovq_drth, fval);
    ioerr += WRITE_OVQ_DROP_THRESHOLD0r(unit, ovq_drth);

    OVQ_DROP_THRESHOLD_RESET_LIMITr_CLR(ovq_drth_rst_limit);
    OVQ_DROP_THRESHOLD_RESET_LIMITr_SET(ovq_drth_rst_limit, total_cells / 4);
    ioerr += WRITE_OVQ_DROP_THRESHOLD_RESET_LIMITr(unit, ovq_drth_rst_limit);

    OVQ_MCQ_CREDITSr_CLR(mcq_credits);
    CDK_PBMP_ITER(mmu_pbmp, port) {
        mport = P2M(unit, port);
        num_lanes = bcm56840_a0_port_num_lanes(unit, port);
        for (idx = 0; idx < NUM_MC_COS; idx++) {
            /*
             * Each subport has 1000 credits.
             * Divide total credit from all subports by 5 multicast queues,
             * then truncate the result to multiple of 16.
             */
            fval = (num_lanes * 1000 / 5) / 16;
            OVQ_MCQ_CREDITSr_CREDITSf_SET(mcq_credits, fval);
            ioerr += WRITE_OVQ_MCQ_CREDITSr(unit, mport, idx, mcq_credits);
        }
    }

    OVQ_FLOWCONTROL_THRESHOLDr_CLR(ovq_fc_th);
    OVQ_FLOWCONTROL_THRESHOLDr_OVQ_FC_ENABLEf_SET(ovq_fc_th, 1);
    OVQ_FLOWCONTROL_THRESHOLDr_OVQ_FC_THRESHOLDf_SET(ovq_fc_th, 11250);
    OVQ_FLOWCONTROL_THRESHOLDr_OVQ_FC_THRESHOLD_RESET_LIMITf_SET(ovq_fc_th, 11249);
    ioerr += WRITE_OVQ_FLOWCONTROL_THRESHOLDr(unit, ovq_fc_th);

    MCQ_FIFO_BASE_REGr_CLR(mcq_fifo_base);
    CDK_PBMP_ITER(mmu_pbmp, port) {
        if (port == CMIC_PORT) {
            continue;
        }
        mport = P2M(unit, port);
        MCQ_FIFO_BASE_REGr_Q_MCQ_FIFO_BASEf_SET(mcq_fifo_base, 12);
        ioerr += WRITE_MCQ_FIFO_BASE_REGr(unit, mport, 1, mcq_fifo_base);
        MCQ_FIFO_BASE_REGr_Q_MCQ_FIFO_BASEf_SET(mcq_fifo_base, 24);
        ioerr += WRITE_MCQ_FIFO_BASE_REGr(unit, mport, 2, mcq_fifo_base);
        MCQ_FIFO_BASE_REGr_Q_MCQ_FIFO_BASEf_SET(mcq_fifo_base, 36);
        ioerr += WRITE_MCQ_FIFO_BASE_REGr(unit, mport, 3, mcq_fifo_base);
        MCQ_FIFO_BASE_REGr_Q_MCQ_FIFO_BASEf_SET(mcq_fifo_base, 48);
        ioerr += WRITE_MCQ_FIFO_BASE_REGr(unit, mport, 4, mcq_fifo_base);
    }

    /* Enable input/output ports (X pipe) */
    CDK_PBMP_CLEAR(pbmp);
    CDK_PBMP_ITER(mmu_pbmp, port) {
        mport = P2M(unit, port);
        if (mport < 33) {
            CDK_PBMP_PORT_ADD(pbmp, mport);
        }
    }
    INPUT_PORT_RX_ENABLE0_64r_CLR(inp_rx_en0);
    OUTPUT_PORT_RX_ENABLE0_64r_CLR(outp_rx_en0);
    fval = CDK_PBMP_WORD_GET(pbmp, 0);
    INPUT_PORT_RX_ENABLE0_64r_INPUT_PORT_RX_ENABLE_LOf_SET(inp_rx_en0, fval);
    OUTPUT_PORT_RX_ENABLE0_64r_OUTPUT_PORT_RX_ENABLE0_LOf_SET(outp_rx_en0, fval);
    fval = CDK_PBMP_WORD_GET(pbmp, 1);
    INPUT_PORT_RX_ENABLE0_64r_INPUT_PORT_RX_ENABLE_HIf_SET(inp_rx_en0, fval);
    OUTPUT_PORT_RX_ENABLE0_64r_OUTPUT_PORT_RX_ENABLE0_HIf_SET(outp_rx_en0, fval);
    ioerr += WRITE_INPUT_PORT_RX_ENABLE0_64r(unit, inp_rx_en0);
    ioerr += WRITE_OUTPUT_PORT_RX_ENABLE0_64r(unit, outp_rx_en0);

    /* Enable input/output ports (Y pipe) */
    CDK_PBMP_CLEAR(pbmp);
    CDK_PBMP_ITER(mmu_pbmp, port) {
        mport = P2M(unit, port);
        /* Y pipe only, skip LB port */
        if (mport >= 34) {
            CDK_PBMP_PORT_ADD(pbmp, mport - 33);
        }
    }
    INPUT_PORT_RX_ENABLE1_64r_CLR(inp_rx_en1);
    OUTPUT_PORT_RX_ENABLE1_64r_CLR(outp_rx_en1);
    fval = CDK_PBMP_WORD_GET(pbmp, 0);
    INPUT_PORT_RX_ENABLE1_64r_INPUT_PORT_RX_ENABLE_LOf_SET(inp_rx_en1, fval);
    OUTPUT_PORT_RX_ENABLE1_64r_OUTPUT_PORT_RX_ENABLE1_LOf_SET(outp_rx_en1, fval);
    fval = CDK_PBMP_WORD_GET(pbmp, 1);
    INPUT_PORT_RX_ENABLE1_64r_INPUT_PORT_RX_ENABLE_HIf_SET(inp_rx_en1, fval);
    OUTPUT_PORT_RX_ENABLE1_64r_OUTPUT_PORT_RX_ENABLE1_HIf_SET(outp_rx_en1, fval);
    ioerr += WRITE_INPUT_PORT_RX_ENABLE1_64r(unit, inp_rx_en1);
    ioerr += WRITE_OUTPUT_PORT_RX_ENABLE1_64r(unit, outp_rx_en1);

    S3_CONFIGr_CLR(s3_cfg);
    S3_CONFIGr_ROUTE_UC_TO_S2f_SET(s3_cfg, 1);
    S3_CONFIG_MCr_CLR(s3_cfg_mc);
    S3_CONFIG_MCr_USE_MC_GROUPf_SET(s3_cfg_mc, 1);
    CDK_PBMP_ITER(mmu_pbmp, port) {
        mport = P2M(unit, port);
        ioerr += WRITE_S3_CONFIGr(unit, mport, s3_cfg);
        ioerr += WRITE_S3_CONFIG_MCr(unit, mport, s3_cfg_mc);
    }

    S2_CONFIGr_CLR(s2_cfg);
    S2_CONFIGr_SCHEDULING_SELECTf_SET(s2_cfg, 0x15);
    /* all unicast queues (4 - 11) and port group 0 (0) to S2.0 */
    S2_S3_ROUTINGr_CLR(s2_s3_routing0);
    S2_S3_ROUTINGr_S3_GROUP_NO_I0f_SET(s2_s3_routing0, 4);
    S2_S3_ROUTINGr_S3_GROUP_NO_I1f_SET(s2_s3_routing0, 5);
    S2_S3_ROUTINGr_S3_GROUP_NO_I2f_SET(s2_s3_routing0, 6);
    S2_S3_ROUTINGr_S3_GROUP_NO_I3f_SET(s2_s3_routing0, 7);
    S2_S3_ROUTINGr_S3_GROUP_NO_I4f_SET(s2_s3_routing0, 8);
    S2_S3_ROUTINGr_S3_GROUP_NO_I5f_SET(s2_s3_routing0, 9);
    S2_S3_ROUTINGr_S3_GROUP_NO_I6f_SET(s2_s3_routing0, 10);
    S2_S3_ROUTINGr_S3_GROUP_NO_I7f_SET(s2_s3_routing0, 11);
    S2_S3_ROUTINGr_S3_GROUP_NO_I8f_SET(s2_s3_routing0, 0);
    /* port group 1 (1) to S2.1, use 0x1f for unused slot */
    S2_S3_ROUTINGr_CLR(s2_s3_routing1);
    S2_S3_ROUTINGr_S3_GROUP_NO_I0f_SET(s2_s3_routing1, 1);
    S2_S3_ROUTINGr_S3_GROUP_NO_I1f_SET(s2_s3_routing1, 0x1f);
    S2_S3_ROUTINGr_S3_GROUP_NO_I2f_SET(s2_s3_routing1, 0x1f);
    S2_S3_ROUTINGr_S3_GROUP_NO_I3f_SET(s2_s3_routing1, 0x1f);
    S2_S3_ROUTINGr_S3_GROUP_NO_I4f_SET(s2_s3_routing1, 0x1f);
    S2_S3_ROUTINGr_S3_GROUP_NO_I5f_SET(s2_s3_routing1, 0x1f);
    S2_S3_ROUTINGr_S3_GROUP_NO_I6f_SET(s2_s3_routing1, 0x1f);
    S2_S3_ROUTINGr_S3_GROUP_NO_I7f_SET(s2_s3_routing1, 0x1f);
    S2_S3_ROUTINGr_S3_GROUP_NO_I8f_SET(s2_s3_routing1, 0x1f);
    /* port group 2 (2) and port group 3 (3) to S2.2 */
    S2_S3_ROUTINGr_CLR(s2_s3_routing2);
    S2_S3_ROUTINGr_S3_GROUP_NO_I0f_SET(s2_s3_routing2, 2);
    S2_S3_ROUTINGr_S3_GROUP_NO_I1f_SET(s2_s3_routing2, 3);
    S2_S3_ROUTINGr_S3_GROUP_NO_I2f_SET(s2_s3_routing2, 0x1f);
    S2_S3_ROUTINGr_S3_GROUP_NO_I3f_SET(s2_s3_routing2, 0x1f);
    S2_S3_ROUTINGr_S3_GROUP_NO_I4f_SET(s2_s3_routing2, 0x1f);
    S2_S3_ROUTINGr_S3_GROUP_NO_I5f_SET(s2_s3_routing2, 0x1f);
    S2_S3_ROUTINGr_S3_GROUP_NO_I6f_SET(s2_s3_routing2, 0x1f);
    S2_S3_ROUTINGr_S3_GROUP_NO_I7f_SET(s2_s3_routing2, 0x1f);
    S2_S3_ROUTINGr_S3_GROUP_NO_I8f_SET(s2_s3_routing2, 0x1f);

    CDK_PBMP_ITER(mmu_pbmp, port) {
        mport = P2M(unit, port);
        ioerr += WRITE_S2_CONFIGr(unit, mport, s2_cfg);
        ioerr += WRITE_S2_S3_ROUTINGr(unit, mport, 0, s2_s3_routing0);
        ioerr += WRITE_S2_S3_ROUTINGr(unit, mport, 1, s2_s3_routing1);
        ioerr += WRITE_S2_S3_ROUTINGr(unit, mport, 2, s2_s3_routing2);
    }

    return ioerr ? CDK_E_IO : rv;
}

static int
_pg_tdm_init(int unit)
{
    int ioerr = 0;
    PORT_GROUP5_TDM_REGr_t pg5_tdm;
    PORT_GROUP4_TDM_REGr_t pg4_tdm;
    PORT_GROUP5_TDM_CONTROLr_t pg5_tdm_ctrl;
    PORT_GROUP4_TDM_CONTROLr_t pg4_tdm_ctrl;
    uint32_t addr, rval;
    int rv;
    int pipe, group, port, lport, idx;
    int tdm_seq[32], tdm_seq_len, tdm_seq_last;

    for (pipe = 0; pipe < 2; pipe++) {
        for (group = 0; group < 2; group++) {
            rv = _calculate_tdm_sequence(unit, pipe, group,
                                         tdm_seq, &tdm_seq_len, -1, 0);
            if (CDK_FAILURE(rv)) {
                return rv;
            }

            CDK_VERB(("PORT_GROUP%c_%c TDM:",
                      group ? '4' : '5', pipe ? 'Y' : 'X'));
            for (idx = 0; idx < tdm_seq_len; idx++) {
                if (idx % 16 == 0) {
                    CDK_VERB(("\n    "));
                }
                CDK_VERB((" %2d", tdm_seq[idx]));
            }
            CDK_VERB(("\n"));

            rval = 0;
            tdm_seq_last = tdm_seq_len - 1;
            for (idx = 0; idx <= tdm_seq_last; idx++) {
                lport = tdm_seq[idx];
                port = 0x7f;
                if (lport >= 0) {
                    port = L2P(unit, lport);
                }
                rval |= LSHIFT32(port, (8 * (idx & 0x3)));
                if ((idx & 0x3) == 3 || idx == (tdm_seq_last)) {
                    addr = idx >> 2;
                    if (group == 0) {
                        PORT_GROUP5_TDM_REGr_SET(pg5_tdm, rval);
                        ioerr += WRITE_PORT_GROUP5_TDM_REGr(unit, pipe,
                                                            addr, pg5_tdm);
                    } else {
                        PORT_GROUP4_TDM_REGr_SET(pg4_tdm, rval);
                        ioerr += WRITE_PORT_GROUP4_TDM_REGr(unit, pipe,
                                                            addr, pg4_tdm);
                    }
                    rval = 0;
                }
            }
            if (group == 0) {
                PORT_GROUP5_TDM_CONTROLr_CLR(pg5_tdm_ctrl);
                PORT_GROUP5_TDM_CONTROLr_TDM_WRAP_PTRf_SET(pg5_tdm_ctrl,
                                                           tdm_seq_last);
                ioerr += WRITE_PORT_GROUP5_TDM_CONTROLr(unit, pipe,
                                                        pg5_tdm_ctrl);
            } else {
                PORT_GROUP4_TDM_CONTROLr_CLR(pg4_tdm_ctrl);
                PORT_GROUP4_TDM_CONTROLr_TDM_WRAP_PTRf_SET(pg4_tdm_ctrl,
                                                           tdm_seq_last);
                ioerr += WRITE_PORT_GROUP4_TDM_CONTROLr(unit, pipe,
                                                        pg4_tdm_ctrl);
            }
        }
    }

    return ioerr;
}

static int
_firmware_helper(void *ctx, uint32_t offset, uint32_t size, void *data)
{
    int ioerr = 0;
    int rv = CDK_E_NONE;
    XLPORT_WC_UCMEM_CTRLr_t ucmem_ctrl;
    XLPORT_WC_UCMEM_DATAm_t ucmem_data;
    int unit, port;
    const char *drv_name;
    uint32_t wbuf[4];
    uint32_t wdata;
    uint32_t *fw_data;
    uint32_t *fw_entry;
    uint32_t fw_size;
    uint32_t idx, wdx;
    uint32_t le_host;

    /* Get unit, port and driver name from context */
    bmd_phy_fw_info_get(ctx, &unit, &port, &drv_name);

    /* Check if PHY driver requests optimized MDC clock */
    if (data == NULL) {
        CMIC_RATE_ADJUSTr_t rate_adjust;
        uint32_t val = 1;

        /* Offset value is MDC clock in kHz (or zero for default) */
        if (offset) {
            val = offset / 1500;
        }
        ioerr += READ_CMIC_RATE_ADJUSTr(unit, &rate_adjust);
        CMIC_RATE_ADJUSTr_DIVIDENDf_SET(rate_adjust, val);
        ioerr += WRITE_CMIC_RATE_ADJUSTr(unit, rate_adjust);

        return ioerr ? CDK_E_IO : CDK_E_NONE;
    }

    if (CDK_STRSTR(drv_name, "warpcore") == NULL) {
        return CDK_E_UNAVAIL;
    }

    if (size == 0) {
        return CDK_E_INTERNAL;
    }

    /* Aligned firmware size */
    fw_size = (size + FW_ALIGN_MASK) & ~FW_ALIGN_MASK;

    /* Enable parallel bus access */
    ioerr += READ_XLPORT_WC_UCMEM_CTRLr(unit, &ucmem_ctrl, port);
    XLPORT_WC_UCMEM_CTRLr_ACCESS_MODEf_SET(ucmem_ctrl, 1);
    ioerr += WRITE_XLPORT_WC_UCMEM_CTRLr(unit, ucmem_ctrl, port);

    /* We need to byte swap on little endian host */
    le_host = 1;
    if (*((uint8_t *)&le_host) == 0) {
        le_host = 0;
    }

    /* DMA buffer needs 32-bit words in little endian order */
    fw_data = (uint32_t *)data;
    for (idx = 0; idx < fw_size; idx += 16) {
        if (idx + 15 < size) {
            fw_entry = &fw_data[idx >> 2];
        } else {
            /* Use staging buffer for modulo bytes */
            CDK_MEMSET(wbuf, 0, sizeof(wbuf));
            CDK_MEMCPY(wbuf, &fw_data[idx >> 2], 16 - (fw_size - size));
            fw_entry = wbuf;
        }
        for (wdx = 0; wdx < 4; wdx++) {
            wdata = fw_entry[wdx];
            if (le_host) {
                wdata = cdk_util_swap32(wdata);
            }
            XLPORT_WC_UCMEM_DATAm_SET(ucmem_data, wdx^0x3, wdata);
        }
        WRITE_XLPORT_WC_UCMEM_DATAm(unit, idx >> 4, ucmem_data, port);
    }

    /* Disable parallel bus access */
    XLPORT_WC_UCMEM_CTRLr_ACCESS_MODEf_SET(ucmem_ctrl, 0);
    ioerr += WRITE_XLPORT_WC_UCMEM_CTRLr(unit, ucmem_ctrl, port);

    return ioerr ? CDK_E_IO : rv;
}

static int
_port_init(int unit, int port)
{
    int ioerr = 0;
    int lport = P2L(unit, port);
    EGR_VLAN_CONTROL_1r_t egr_vlan_ctrl1;
    PORT_TABm_t port_tab;
    EGR_PORTm_t egr_port;
    EGR_ENABLEm_t egr_enable;

    /* Skip if no logical port is configured for this port */
    if (lport < 0) {
        return 0;
    }

    /* Default port VLAN and tag action, enable L2 HW learning */
    PORT_TABm_CLR(port_tab);
    PORT_TABm_PORT_VIDf_SET(port_tab, 1);
    PORT_TABm_FILTER_ENABLEf_SET(port_tab, 1);
    PORT_TABm_OUTER_TPID_ENABLEf_SET(port_tab, 1);
    PORT_TABm_CML_FLAGS_NEWf_SET(port_tab, 8);
    PORT_TABm_CML_FLAGS_MOVEf_SET(port_tab, 8);
    ioerr += WRITE_PORT_TABm(unit, lport, port_tab);

    /* Filter VLAN on egress */
    EGR_PORTm_CLR(egr_port);
    EGR_PORTm_EN_EFILTERf_SET(egr_port, 1);
    ioerr += WRITE_EGR_PORTm(unit, lport, egr_port);

    /* Configure egress VLAN for backward compatibility */
    ioerr += READ_EGR_VLAN_CONTROL_1r(unit, lport, &egr_vlan_ctrl1);
    EGR_VLAN_CONTROL_1r_VT_MISS_UNTAGf_SET(egr_vlan_ctrl1, 0);
    EGR_VLAN_CONTROL_1r_REMARK_OUTER_DOT1Pf_SET(egr_vlan_ctrl1, 1);
    ioerr += WRITE_EGR_VLAN_CONTROL_1r(unit, lport, egr_vlan_ctrl1);

    /* Egress enable */
    ioerr += READ_EGR_ENABLEm(unit, port, &egr_enable);
    EGR_ENABLEm_PRT_ENABLEf_SET(egr_enable, 1);
    ioerr += WRITE_EGR_ENABLEm(unit, port, egr_enable);

    return ioerr;
}

static int
_gport_init(int unit, int port)
{
    int ioerr = 0;
    COMMAND_CONFIGr_t command_cfg;
    TX_IPG_LENGTHr_t tx_ipg;

    ioerr += READ_COMMAND_CONFIGr(unit, port, &command_cfg);
    COMMAND_CONFIGr_SW_RESETf_SET(command_cfg, 1);
    ioerr += WRITE_COMMAND_CONFIGr(unit, port, command_cfg);

    /* Ensure that MAC (Rx) and loopback mode is disabled */
    ioerr += READ_COMMAND_CONFIGr(unit, port, &command_cfg);
    COMMAND_CONFIGr_LOOP_ENAf_SET(command_cfg, 0);
    COMMAND_CONFIGr_RX_ENAf_SET(command_cfg, 0);
    COMMAND_CONFIGr_TX_ENAf_SET(command_cfg, 1);
    ioerr += WRITE_COMMAND_CONFIGr(unit, port, command_cfg);

    TX_IPG_LENGTHr_SET(tx_ipg, 12);
    ioerr += WRITE_TX_IPG_LENGTHr(unit, port, tx_ipg);

    return ioerr;
}

int
bcm56840_a0_xport_init(int unit, int port)
{
    int ioerr = 0;
    XMAC_TX_CTRLr_t txctrl;
    XMAC_RX_CTRLr_t rxctrl;
    XMAC_RX_MAX_SIZEr_t rxmaxsz;
    XMAC_CTRLr_t mac_ctrl;

    /* Common port initialization */
    ioerr += _port_init(unit, port);

    /* Initialize UniMAC */
    ioerr += _gport_init(unit, port);

    /* Ensure that MAC (Rx) and loopback mode is disabled */
    XMAC_CTRLr_CLR(mac_ctrl);
    XMAC_CTRLr_SOFT_RESETf_SET(mac_ctrl, 1);
    ioerr += WRITE_XMAC_CTRLr(unit, port, mac_ctrl);

    XMAC_CTRLr_SOFT_RESETf_SET(mac_ctrl, 0);
    XMAC_CTRLr_TX_ENf_SET(mac_ctrl, 1);
    if (bcm56840_a0_port_speed_max(unit, port) == 40000) {
        XMAC_CTRLr_XLGMII_ALIGN_ENBf_SET(mac_ctrl, 1);
    }
    ioerr += WRITE_XMAC_CTRLr(unit, port, mac_ctrl);

    /* Configure Tx (Inter-Packet-Gap, recompute CRC mode, IEEE header) */
    XMAC_TX_CTRLr_CLR(txctrl);
    XMAC_TX_CTRLr_PAD_THRESHOLDf_SET(txctrl, 0x40);
    XMAC_TX_CTRLr_AVERAGE_IPGf_SET(txctrl, 0xc);
    XMAC_TX_CTRLr_CRC_MODEf_SET(txctrl, 0x2);
    ioerr += WRITE_XMAC_TX_CTRLr(unit, port, txctrl);

    /* Configure Rx (strip CRC, strict preamble, IEEE header) */
    XMAC_RX_CTRLr_CLR(rxctrl);
    XMAC_RX_CTRLr_STRICT_PREAMBLEf_SET(rxctrl, 1);
    ioerr += WRITE_XMAC_RX_CTRLr(unit, port, rxctrl);

    /* Set max Rx frame size */
    XMAC_RX_MAX_SIZEr_CLR(rxmaxsz);
    XMAC_RX_MAX_SIZEr_RX_MAX_SIZEf_SET(rxmaxsz, JUMBO_MAXSZ);
    ioerr += WRITE_XMAC_RX_MAX_SIZEr(unit, port, rxmaxsz);

    return ioerr;
}

int
bcm56840_a0_bmd_init(int unit)
{
    int ioerr = 0;
    int rv;
    ING_HW_RESET_CONTROL_1r_t ing_rst_ctl_1;
    ING_HW_RESET_CONTROL_2r_t ing_rst_ctl_2;
    EGR_HW_RESET_CONTROL_0r_t egr_rst_ctl_0;
    EGR_HW_RESET_CONTROL_1r_t egr_rst_ctl_1;
    EGR_VLAN_CONTROL_1r_t egr_vlan_ctrl_1;
    EGR_IPMC_CFG2r_t egr_ipmc_cfg2;
    CPU_PBMm_t cpu_pbm;
    CPU_PBM_2m_t cpu_pbm_2;
    ISBS_PORT_TO_PIPE_MAPPINGm_t isbs_map;
    ESBS_PORT_TO_PIPE_MAPPINGm_t esbs_map;
    EGR_ING_PORTm_t egr_ing_port;
    XLPORT_MODE_REGr_t xlport_mode;
    XLPORT_PORT_ENABLEr_t xlport_en;
    XLPORT_XMAC_CONTROLr_t xlp_mac_ctrl;
    XLPORT_MIB_RESETr_t mib_reset;
    PORT_GROUP5_QGPORT_ENABLEr_t pg5_qgp_en;
    IARB_MAIN_TDMm_t iarb_main_tdm;
    IARB_TDM_CONTROLr_t iarb_tdm_ctrl;
    MISCCONFIGr_t misc_cfg;
    ING_EN_EFILTER_BITMAPm_t ing_en_efilter;
    CMIC_RATE_ADJUSTr_t rate_adjust;
    CMIC_RATE_ADJUST_INT_MDIOr_t rate_adjust_int;
    RDBGC0_SELECTr_t rdbgc0_select;
    VLAN_PROFILE_TABm_t vlan_profile;
    ING_VLAN_TAG_ACTION_PROFILEm_t vlan_action;
    EGR_VLAN_TAG_ACTION_PROFILEm_t egr_action;
    cdk_pbmp_t pbmp;
    uint32_t pbm[3];
    int num_lanes, mac_mode, qgp_en;
    int mdio_div;
    int port, lport, sub_port;
    int idx, tdm_max;

    BMD_CHECK_UNIT(unit);

    /* Reset the IPIPE block */
    ING_HW_RESET_CONTROL_1r_CLR(ing_rst_ctl_1);
    ioerr += WRITE_ING_HW_RESET_CONTROL_1r(unit, ing_rst_ctl_1);
    ING_HW_RESET_CONTROL_2r_CLR(ing_rst_ctl_2);
    ING_HW_RESET_CONTROL_2r_RESET_ALLf_SET(ing_rst_ctl_2, 1);
    ING_HW_RESET_CONTROL_2r_VALIDf_SET(ing_rst_ctl_2, 1);
    ING_HW_RESET_CONTROL_2r_COUNTf_SET(ing_rst_ctl_2, 0x20000);
    ioerr += WRITE_ING_HW_RESET_CONTROL_2r(unit, ing_rst_ctl_2);

    /* Reset the EPIPE block */
    EGR_HW_RESET_CONTROL_0r_CLR(egr_rst_ctl_0);
    EGR_HW_RESET_CONTROL_1r_CLR(egr_rst_ctl_1);
    ioerr += WRITE_EGR_HW_RESET_CONTROL_0r(unit, egr_rst_ctl_0);
    EGR_HW_RESET_CONTROL_1r_RESET_ALLf_SET(egr_rst_ctl_1, 1);
    EGR_HW_RESET_CONTROL_1r_VALIDf_SET(egr_rst_ctl_1, 1);
    EGR_HW_RESET_CONTROL_1r_COUNTf_SET(egr_rst_ctl_1, 0x2000);
    ioerr += WRITE_EGR_HW_RESET_CONTROL_1r(unit, egr_rst_ctl_1);

    for (idx = 0; idx < PIPE_RESET_TIMEOUT_MSEC; idx++) {
        ioerr += READ_ING_HW_RESET_CONTROL_2r(unit, &ing_rst_ctl_2);
        if (ING_HW_RESET_CONTROL_2r_DONEf_GET(ing_rst_ctl_2)) {
            break;
        }
        BMD_SYS_USLEEP(1000);
    }
    if (idx >= PIPE_RESET_TIMEOUT_MSEC) {
        CDK_WARN(("bcm56840_a0_bmd_init[%d]: IPIPE reset timeout\n", unit));
        return ioerr ? CDK_E_IO : CDK_E_TIMEOUT;
    }
        
    for (; idx < PIPE_RESET_TIMEOUT_MSEC; idx++) {
        ioerr += READ_EGR_HW_RESET_CONTROL_1r(unit, &egr_rst_ctl_1);
        if (EGR_HW_RESET_CONTROL_1r_DONEf_GET(egr_rst_ctl_1)) {
            break;
        }
        BMD_SYS_USLEEP(1000);
    }
    if (idx >= PIPE_RESET_TIMEOUT_MSEC) {
        CDK_WARN(("bcm56840_a0_bmd_init[%d]: EPIPE reset timeout\n", unit));
        return ioerr ? CDK_E_IO : CDK_E_TIMEOUT;
    }

    /* Clear pipe reset registers */
    ING_HW_RESET_CONTROL_2r_CLR(ing_rst_ctl_2);
    ioerr += WRITE_ING_HW_RESET_CONTROL_2r(unit, ing_rst_ctl_2);
    EGR_HW_RESET_CONTROL_1r_CLR(egr_rst_ctl_1);
    ioerr += WRITE_EGR_HW_RESET_CONTROL_1r(unit, egr_rst_ctl_1);

    /* Clear registers that are implemented in memory */
    EGR_VLAN_CONTROL_1r_CLR(egr_vlan_ctrl_1);
    EGR_IPMC_CFG2r_CLR(egr_ipmc_cfg2);
    bcm56840_a0_xlport_pbmp_get(unit, &pbmp);
    CDK_PBMP_PORT_ADD(pbmp, CMIC_PORT);
    CDK_PBMP_ITER(pbmp, port) {
        lport = P2L(unit, port);
        ioerr += WRITE_EGR_VLAN_CONTROL_1r(unit, lport, egr_vlan_ctrl_1);
        ioerr += WRITE_EGR_IPMC_CFG2r(unit, lport, egr_ipmc_cfg2);
    }

    /* Initialize port mappings */
    ioerr += _port_map_init(unit);

    /* Configure CPU port */
    CPU_PBMm_CLR(cpu_pbm);
    CPU_PBMm_BITMAP_W0f_SET(cpu_pbm, 1);
    ioerr += WRITE_CPU_PBMm(unit, 0, cpu_pbm);
    CPU_PBM_2m_CLR(cpu_pbm_2);
    CPU_PBM_2m_BITMAP_W0f_SET(cpu_pbm_2, 1);
    ioerr += WRITE_CPU_PBM_2m(unit, 0, cpu_pbm_2);

    /* Configure logical ports belonging to Y-pipe */
    CDK_MEMSET(pbm, 0, sizeof(pbm));
    for (port = NUM_PHYS_PORTS/2; port < NUM_PHYS_PORTS; port++) {
        lport = P2L(unit, port);
        if (lport >= 0) {
            pbm[lport >> 5] |= LSHIFT32(1, (lport & 0x1f));
        }
    }
    ISBS_PORT_TO_PIPE_MAPPINGm_CLR(isbs_map);
    ISBS_PORT_TO_PIPE_MAPPINGm_BITMAPf_SET(isbs_map, pbm);
    WRITE_ISBS_PORT_TO_PIPE_MAPPINGm(unit, 0, isbs_map);
    ESBS_PORT_TO_PIPE_MAPPINGm_CLR(esbs_map);
    ESBS_PORT_TO_PIPE_MAPPINGm_BITMAPf_SET(esbs_map, pbm);
    WRITE_ESBS_PORT_TO_PIPE_MAPPINGm(unit, 0, esbs_map);

    /* Configure CPU HiGig ingress */
    EGR_ING_PORTm_CLR(egr_ing_port);
    EGR_ING_PORTm_PORT_TYPEf_SET(egr_ing_port, 1);
    WRITE_EGR_ING_PORTm(unit, CMIC_HG_LPORT, egr_ing_port);

    /* Initialize XLPORTs */
    qgp_en = 0;
    bcm56840_a0_xlport_pbmp_get(unit, &pbmp);
    CDK_PBMP_ITER(pbmp, port) {
        /* We only need to write once per block */
        if (XLPORT_SUBPORT(port) != 0) {
            continue;
        }
        XLPORT_MODE_REGr_CLR(xlport_mode);
        XLPORT_PORT_ENABLEr_CLR(xlport_en);
        /* Configure each sub port */
        for (sub_port = 0; sub_port <= 3; sub_port++) {
            if (!CDK_PBMP_MEMBER(pbmp, port + sub_port)) {
                continue;
            }
            num_lanes = bcm56840_a0_port_num_lanes(unit, port + sub_port);
            mac_mode = 0;
            if (num_lanes == 1) {
                XLPORT_MODE_REGr_CORE_PORT_MODEf_SET(xlport_mode, 2);
                XLPORT_MODE_REGr_PHY_PORT_MODEf_SET(xlport_mode, 2);
                if (bcm56840_a0_port_speed_max(unit, port) <= 2500) {
                    mac_mode = 1;
                    if (XLPORT_BLKIDX(port) == 0) {
                        qgp_en = 1;
                    }
                }
            } else if (num_lanes == 2) {
                XLPORT_MODE_REGr_CORE_PORT_MODEf_SET(xlport_mode, 1);
                XLPORT_MODE_REGr_PHY_PORT_MODEf_SET(xlport_mode, 1);
            }
            if (sub_port == 0) {
                XLPORT_MODE_REGr_PORT0_MAC_MODEf_SET(xlport_mode, mac_mode);
                XLPORT_PORT_ENABLEr_PORT0f_SET(xlport_en, 1);
            } else if (sub_port == 1) {
                XLPORT_MODE_REGr_PORT1_MAC_MODEf_SET(xlport_mode, mac_mode);
                XLPORT_PORT_ENABLEr_PORT1f_SET(xlport_en, 1);
            } else if (sub_port == 2) {
                XLPORT_MODE_REGr_PORT2_MAC_MODEf_SET(xlport_mode, mac_mode);
                XLPORT_PORT_ENABLEr_PORT2f_SET(xlport_en, 1);
            } else if (sub_port == 3) {
                XLPORT_MODE_REGr_PORT3_MAC_MODEf_SET(xlport_mode, mac_mode);
                XLPORT_PORT_ENABLEr_PORT3f_SET(xlport_en, 1);
            }
        }
        ioerr += WRITE_XLPORT_MODE_REGr(unit, xlport_mode, port);
        ioerr += WRITE_XLPORT_PORT_ENABLEr(unit, xlport_en, port);
    }

    /* Reset MIB counters in all blocks */
    CDK_XGS_BLKTYPE_PBMP_GET(unit, BLKTYPE_XLPORT, &pbmp);
    CDK_PBMP_ITER(pbmp, port) {
        XLPORT_MIB_RESETr_CLR(mib_reset);
        XLPORT_MIB_RESETr_CLR_CNTf_SET(mib_reset, 0xf);
        ioerr += WRITE_XLPORT_MIB_RESETr(unit, mib_reset, port);
        XLPORT_MIB_RESETr_CLR_CNTf_SET(mib_reset, 0);
        ioerr += WRITE_XLPORT_MIB_RESETr(unit, mib_reset, port);
    }

    /* Enable QGPORT (1G x 4) in XLPORT0 block */
    if (qgp_en) {
        PORT_GROUP5_QGPORT_ENABLEr_CLR(pg5_qgp_en);
        PORT_GROUP5_QGPORT_ENABLEr_QGPORT_ENABLEf_SET(pg5_qgp_en, qgp_en);
        ioerr += WRITE_PORT_GROUP5_QGPORT_ENABLEr(unit, 0, pg5_qgp_en);
    }

    /* Setup TDM within each port group */
    rv = _pg_tdm_init(unit);
    if (CDK_FAILURE(rv)) {
        return rv;
    }

    /* Setup main TDM between each port group */
    tdm_max = 32;
    if (CDK_XGS_FLAGS(unit) & CHIP_FLAG_BW640G) {
        tdm_max = 64;
    }
    IARB_MAIN_TDMm_CLR(iarb_main_tdm);
    for (idx = 0; idx < tdm_max; idx++) {
        IARB_MAIN_TDMm_TDM_SLOTf_SET(iarb_main_tdm, idx & 1);
        ioerr += WRITE_IARB_MAIN_TDMm(unit, idx, iarb_main_tdm);
    }
    IARB_MAIN_TDMm_TDM_SLOTf_SET(iarb_main_tdm, 2);
    ioerr += WRITE_IARB_MAIN_TDMm(unit, tdm_max, iarb_main_tdm);

    /* Enable arbiter */
    ioerr += READ_IARB_TDM_CONTROLr(unit, &iarb_tdm_ctrl);
    IARB_TDM_CONTROLr_DISABLEf_SET(iarb_tdm_ctrl, 0);
#if BMD_CONFIG_SIMULATION
    IARB_TDM_CONTROLr_DISABLEf_SET(iarb_tdm_ctrl, 1);
#endif
    IARB_TDM_CONTROLr_TDM_WRAP_PTRf_SET(iarb_tdm_ctrl, tdm_max);
    ioerr += WRITE_IARB_TDM_CONTROLr(unit, iarb_tdm_ctrl);

    /* Enable Field Processor metering clock */
    ioerr += READ_MISCCONFIGr(unit, &misc_cfg);
    MISCCONFIGr_METERING_CLK_ENf_SET(misc_cfg, 1);
    ioerr += WRITE_MISCCONFIGr(unit, misc_cfg);

    /* Ensure that link bitmap is cleared */
    ioerr += CDK_XGS_MEM_CLEAR(unit, EPC_LINK_BMAPm);

    /* Enable egress VLAN checks for all ports */
    bcm56840_a0_xlport_pbmp_get(unit, &pbmp);
    CDK_PBMP_PORT_ADD(pbmp, CMIC_PORT);
    ING_EN_EFILTER_BITMAPm_CLR(ing_en_efilter);
    ING_EN_EFILTER_BITMAPm_BITMAP_W0f_SET(ing_en_efilter,
                                          CDK_PBMP_WORD_GET(pbmp, 0));
    ING_EN_EFILTER_BITMAPm_BITMAP_W1f_SET(ing_en_efilter,
                                          CDK_PBMP_WORD_GET(pbmp, 1));
    ING_EN_EFILTER_BITMAPm_BITMAP_W2f_SET(ing_en_efilter,
                                          CDK_PBMP_WORD_GET(pbmp, 2));
    ioerr += WRITE_ING_EN_EFILTER_BITMAPm(unit, 0, ing_en_efilter);

    /*
     * Set MDIO reference clocks based on core clock:
     * mdio_refclk = coreclk * (1/divisor)
     *
     * Actual MDIO clock is reference clock divided by 2:
     * mdio_clk = mdio_refclk/2
     */

    /* mdio_refclk = 495 MHz * (1/40) = 12 MHz */
    mdio_div = 40;

    /* Configure internal MDC (refclk/2 = 6 MHz) */
    CMIC_RATE_ADJUST_INT_MDIOr_CLR(rate_adjust_int);
    CMIC_RATE_ADJUST_INT_MDIOr_DIVISORf_SET(rate_adjust_int, mdio_div);
    CMIC_RATE_ADJUST_INT_MDIOr_DIVIDENDf_SET(rate_adjust_int, 1);
    ioerr += WRITE_CMIC_RATE_ADJUST_INT_MDIOr(unit, rate_adjust_int);

    /* Configure external MDC (1/4 * refclk/2 = 1.5 MHz) */
    CMIC_RATE_ADJUSTr_CLR(rate_adjust);
    CMIC_RATE_ADJUSTr_DIVISORf_SET(rate_adjust, 4 * mdio_div);
    CMIC_RATE_ADJUSTr_DIVIDENDf_SET(rate_adjust, 1);
    ioerr += WRITE_CMIC_RATE_ADJUSTr(unit, rate_adjust);

    /* Configure discard counter */
    RDBGC0_SELECTr_CLR(rdbgc0_select);
    RDBGC0_SELECTr_BITMAPf_SET(rdbgc0_select, 0x0400ad11);
    ioerr += WRITE_RDBGC0_SELECTr(unit, rdbgc0_select);

    /* Initialize MMU */
    rv = _mmu_init(unit);
    if (CDK_FAILURE(rv)) {
        return rv;
    }

    /* Default VLAN profile.
     *
     * The _PFM fields above are MULTICAST forwarding mode (per the
     * register docs: "PFM value for IPv4 multicast lookups"), they
     * don't affect unicast.  Keep upstream values 1 for now.
     *
     * Patched for EdgeNOS: write the profile to *every* index so
     * every VLAN points at a valid profile regardless of its
     * VLAN_PROFILE_PTR (which bmd_vlan_create leaves at default 0,
     * not VLAN_PROFILE_TABm_MAX). */
    /* Force L3 OFF at the VLAN level so the chip ignores the L3
     * pipeline entirely and just bridges every frame by dst MAC.
     * Our L2 table has the swpN MACs -> CPU port, so IPv4 frames
     * addressed to our interfaces should now reach the CPU even
     * without L3_HOST entries.  Multicast L2_ENABLE bits stay on
     * so broadcast / multicast (ARP) keeps working as before. */
    VLAN_PROFILE_TABm_CLR(vlan_profile);
    VLAN_PROFILE_TABm_L2_PFMf_SET(vlan_profile, 1);
    VLAN_PROFILE_TABm_L3_IPV4_PFMf_SET(vlan_profile, 1);
    VLAN_PROFILE_TABm_L3_IPV6_PFMf_SET(vlan_profile, 1);
    VLAN_PROFILE_TABm_IPMCV6_ENABLEf_SET(vlan_profile, 0);
    VLAN_PROFILE_TABm_IPMCV4_ENABLEf_SET(vlan_profile, 0);
    VLAN_PROFILE_TABm_IPMCV6_L2_ENABLEf_SET(vlan_profile, 1);
    VLAN_PROFILE_TABm_IPMCV4_L2_ENABLEf_SET(vlan_profile, 1);
    VLAN_PROFILE_TABm_IPV6L3_ENABLEf_SET(vlan_profile, 0);
    VLAN_PROFILE_TABm_IPV4L3_ENABLEf_SET(vlan_profile, 0);
    {
        int _vpi;
        for (_vpi = 0; _vpi <= VLAN_PROFILE_TABm_MAX; _vpi++) {
            ioerr += WRITE_VLAN_PROFILE_TABm(unit, _vpi, vlan_profile);
        }
    }

    /* Ensure that all incoming packets get tagged appropriately */
    ING_VLAN_TAG_ACTION_PROFILEm_CLR(vlan_action);
    ING_VLAN_TAG_ACTION_PROFILEm_UT_OTAG_ACTIONf_SET(vlan_action, 1);
    ING_VLAN_TAG_ACTION_PROFILEm_SIT_PITAG_ACTIONf_SET(vlan_action, 3);
    ING_VLAN_TAG_ACTION_PROFILEm_SIT_OTAG_ACTIONf_SET(vlan_action, 1);
    ING_VLAN_TAG_ACTION_PROFILEm_SOT_POTAG_ACTIONf_SET(vlan_action, 2);
    ING_VLAN_TAG_ACTION_PROFILEm_DT_POTAG_ACTIONf_SET(vlan_action, 2);
    ioerr += WRITE_ING_VLAN_TAG_ACTION_PROFILEm(unit, 0, vlan_action);

    /* Create special egress action profile for HiGig ports */
    EGR_VLAN_TAG_ACTION_PROFILEm_CLR(egr_action);
    EGR_VLAN_TAG_ACTION_PROFILEm_SOT_OTAG_ACTIONf_SET(egr_action, 3);
    EGR_VLAN_TAG_ACTION_PROFILEm_DT_OTAG_ACTIONf_SET(egr_action, 3);
    ioerr += WRITE_EGR_VLAN_TAG_ACTION_PROFILEm(unit, 1, egr_action);

    /* Probe PHYs */
    bcm56840_a0_xlport_pbmp_get(unit, &pbmp);
    CDK_PBMP_ITER(pbmp, port) {
        if (CDK_SUCCESS(rv)) {
            rv = bmd_phy_probe(unit, port);
        }
        if (CDK_SUCCESS(rv)) {
            num_lanes = bcm56840_a0_port_num_lanes(unit, port);
            if (num_lanes == 2) {
                rv = bmd_phy_mode_set(unit, port, "warpcore",
                                      BMD_PHY_MODE_2LANE, 1);
            }
        }
        if (CDK_SUCCESS(rv)) {
            rv = bmd_phy_fw_helper_set(unit, port, _firmware_helper);
        }
    }

    if (CDK_SUCCESS(rv)) {
        rv = bmd_phy_staged_init(unit, &pbmp);
    }

    /* Configure XLPORTs */
    CDK_PBMP_ITER(pbmp, port) {
        /* Clear MAC hard reset after warpcore is initialized */
        if (XLPORT_SUBPORT(port) == 0) {
            XLPORT_XMAC_CONTROLr_CLR(xlp_mac_ctrl);
            ioerr += WRITE_XLPORT_XMAC_CONTROLr(unit, xlp_mac_ctrl, port);
        }
        /* Initialize XLPORTs after XMAC is out of reset */
        ioerr += bcm56840_a0_xport_init(unit, port);
    }

#if BMD_CONFIG_INCLUDE_DMA
    /* Common port initialization for CPU port */
    ioerr += _port_init(unit, CMIC_PORT);

    if (CDK_SUCCESS(rv)) {
        rv = bmd_xgs_dma_init(unit);
    }
#endif

    return ioerr ? CDK_E_IO : rv;
}
#endif /* CDK_CONFIG_INCLUDE_BCM56840_A0 */
