/*
 * Auto-generated from Cumulus capture by
 * asic/edged/scripts/gen_cumulus_tables.py — do not edit by hand.
 *
 * Source: /home/smiley/edgecore/edgecore-5610-reverse-engineering/cumulus_baseline_2013_run2/streamed_20260513_162341/soc/dump_socmem_diff.txt
 */
#ifndef _CUMULUS_EGR_VLAN_STG_H_
#define _CUMULUS_EGR_VLAN_STG_H_

#include <stdint.h>

struct cumulus_egr_vlan_stg_row {
    uint32_t index;
    uint32_t sp_tree_port1;
    uint32_t sp_tree_port2;
};

static const struct cumulus_egr_vlan_stg_row cumulus_egr_vlan_stg_rows[] = {
    { 1, 3, 3 },
};

#define CUMULUS_EGR_VLAN_STG_COUNT (sizeof(cumulus_egr_vlan_stg_rows)/sizeof(cumulus_egr_vlan_stg_rows[0]))

#endif
