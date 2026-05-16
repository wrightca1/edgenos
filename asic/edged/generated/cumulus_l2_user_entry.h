/*
 * Auto-generated from Cumulus capture by
 * asic/edged/scripts/gen_cumulus_tables.py — do not edit by hand.
 *
 * Source: /home/smiley/edgecore/edgecore-5610-reverse-engineering/cumulus_baseline_2013_run2/streamed_20260513_162341/soc/dump_socmem_diff.txt
 */
#ifndef _CUMULUS_L2_USER_ENTRY_H_
#define _CUMULUS_L2_USER_ENTRY_H_

#include <stdint.h>

struct cumulus_l2_user_entry_row {
    uint32_t index;
    uint32_t valid;
    uint32_t key_type;
    uint64_t mac_addr;
    uint64_t key;
    uint32_t l2_protocol_pkt;
    uint32_t do_not_learn_macsa;
    uint32_t cpu;
    uint32_t bpdu;
};

static const struct cumulus_l2_user_entry_row cumulus_l2_user_entry_rows[] = {
    { 0, 1, 0, 0x0180c2000000ULL, 0x00000180c2000000ULL, 0, 0, 1, 1 },
    { 1, 1, 1, 0x0180c2000000ULL, 0x10000180c2000000ULL, 1, 0, 1, 1 },
    { 2, 1, 0, 0x0180c2000010ULL, 0x00000180c2000010ULL, 0, 0, 1, 1 },
    { 3, 1, 1, 0x0180c2000010ULL, 0x10000180c2000010ULL, 1, 0, 1, 1 },
    { 4, 1, 0, 0x0180c200000eULL, 0x00000180c200000eULL, 1, 1, 1, 1 },
    { 5, 1, 0, 0x01000cccccccULL, 0x000001000cccccccULL, 1, 1, 1, 1 },
    { 6, 1, 0, 0x01000ccccccdULL, 0x000001000ccccccdULL, 1, 1, 1, 1 },
    { 7, 1, 0, 0x80a23581caafULL, 0x000080a23581caafULL, 0, 0, 0, 0 },
    { 8, 1, 0, 0x80a23581cab0ULL, 0x000080a23581cab0ULL, 0, 0, 0, 0 },
    { 9, 1, 0, 0x80a23581cab1ULL, 0x000080a23581cab1ULL, 0, 0, 0, 0 },
    { 10, 1, 0, 0x80a23581cab2ULL, 0x000080a23581cab2ULL, 0, 0, 0, 0 },
    { 11, 1, 0, 0x80a23581cab3ULL, 0x000080a23581cab3ULL, 0, 0, 0, 0 },
    { 12, 1, 0, 0x80a23581cab4ULL, 0x000080a23581cab4ULL, 0, 0, 0, 0 },
    { 13, 1, 0, 0x80a23581cab5ULL, 0x000080a23581cab5ULL, 0, 0, 0, 0 },
    { 14, 1, 0, 0x80a23581cab6ULL, 0x000080a23581cab6ULL, 0, 0, 0, 0 },
    { 15, 1, 0, 0x80a23581cab7ULL, 0x000080a23581cab7ULL, 0, 0, 0, 0 },
    { 16, 1, 0, 0x80a23581cab8ULL, 0x000080a23581cab8ULL, 0, 0, 0, 0 },
    { 17, 1, 0, 0x80a23581cab9ULL, 0x000080a23581cab9ULL, 0, 0, 0, 0 },
    { 18, 1, 0, 0x80a23581cabaULL, 0x000080a23581cabaULL, 0, 0, 0, 0 },
    { 19, 1, 0, 0x80a23581cabbULL, 0x000080a23581cabbULL, 0, 0, 0, 0 },
    { 20, 1, 0, 0x80a23581cabcULL, 0x000080a23581cabcULL, 0, 0, 0, 0 },
    { 21, 1, 0, 0x80a23581cabdULL, 0x000080a23581cabdULL, 0, 0, 0, 0 },
    { 22, 1, 0, 0x80a23581cabeULL, 0x000080a23581cabeULL, 0, 0, 0, 0 },
    { 23, 1, 0, 0x80a23581cabfULL, 0x000080a23581cabfULL, 0, 0, 0, 0 },
    { 24, 1, 0, 0x80a23581cac0ULL, 0x000080a23581cac0ULL, 0, 0, 0, 0 },
    { 25, 1, 0, 0x80a23581cac1ULL, 0x000080a23581cac1ULL, 0, 0, 0, 0 },
    { 26, 1, 0, 0x80a23581cac2ULL, 0x000080a23581cac2ULL, 0, 0, 0, 0 },
    { 27, 1, 0, 0x80a23581cac3ULL, 0x000080a23581cac3ULL, 0, 0, 0, 0 },
    { 28, 1, 0, 0x80a23581cac4ULL, 0x000080a23581cac4ULL, 0, 0, 0, 0 },
    { 29, 1, 0, 0x80a23581cac5ULL, 0x000080a23581cac5ULL, 0, 0, 0, 0 },
    { 30, 1, 0, 0x80a23581cac6ULL, 0x000080a23581cac6ULL, 0, 0, 0, 0 },
    { 31, 1, 0, 0x80a23581cac7ULL, 0x000080a23581cac7ULL, 0, 0, 0, 0 },
    { 32, 1, 0, 0x80a23581cac8ULL, 0x000080a23581cac8ULL, 0, 0, 0, 0 },
    { 33, 1, 0, 0x80a23581cac9ULL, 0x000080a23581cac9ULL, 0, 0, 0, 0 },
    { 34, 1, 0, 0x80a23581cacaULL, 0x000080a23581cacaULL, 0, 0, 0, 0 },
    { 35, 1, 0, 0x80a23581cacbULL, 0x000080a23581cacbULL, 0, 0, 0, 0 },
    { 36, 1, 0, 0x80a23581caccULL, 0x000080a23581caccULL, 0, 0, 0, 0 },
    { 37, 1, 0, 0x80a23581cacdULL, 0x000080a23581cacdULL, 0, 0, 0, 0 },
    { 38, 1, 0, 0x80a23581caceULL, 0x000080a23581caceULL, 0, 0, 0, 0 },
    { 39, 1, 0, 0x80a23581cacfULL, 0x000080a23581cacfULL, 0, 0, 0, 0 },
    { 40, 1, 0, 0x80a23581cad0ULL, 0x000080a23581cad0ULL, 0, 0, 0, 0 },
    { 41, 1, 0, 0x80a23581cad1ULL, 0x000080a23581cad1ULL, 0, 0, 0, 0 },
    { 42, 1, 0, 0x80a23581cad2ULL, 0x000080a23581cad2ULL, 0, 0, 0, 0 },
    { 43, 1, 0, 0x80a23581cad3ULL, 0x000080a23581cad3ULL, 0, 0, 0, 0 },
    { 44, 1, 0, 0x80a23581cad4ULL, 0x000080a23581cad4ULL, 0, 0, 0, 0 },
    { 45, 1, 0, 0x80a23581cad5ULL, 0x000080a23581cad5ULL, 0, 0, 0, 0 },
    { 46, 1, 0, 0x80a23581cad6ULL, 0x000080a23581cad6ULL, 0, 0, 0, 0 },
    { 47, 1, 0, 0x80a23581cad7ULL, 0x000080a23581cad7ULL, 0, 0, 0, 0 },
    { 48, 1, 0, 0x80a23581cad8ULL, 0x000080a23581cad8ULL, 0, 0, 0, 0 },
    { 49, 1, 0, 0x80a23581cad9ULL, 0x000080a23581cad9ULL, 0, 0, 0, 0 },
    { 50, 1, 0, 0x80a23581cadaULL, 0x000080a23581cadaULL, 0, 0, 0, 0 },
    { 51, 1, 0, 0x80a23581cadbULL, 0x000080a23581cadbULL, 0, 0, 0, 0 },
    { 52, 1, 0, 0x80a23581cadcULL, 0x000080a23581cadcULL, 0, 0, 0, 0 },
    { 53, 1, 0, 0x80a23581caddULL, 0x000080a23581caddULL, 0, 0, 0, 0 },
    { 54, 1, 0, 0x80a23581cadeULL, 0x000080a23581cadeULL, 0, 0, 0, 0 },
    { 55, 1, 0, 0x80a23581cadfULL, 0x000080a23581cadfULL, 0, 0, 0, 0 },
    { 56, 1, 0, 0x80a23581cae3ULL, 0x000080a23581cae3ULL, 0, 0, 0, 0 },
    { 57, 1, 0, 0x80a23581cae7ULL, 0x000080a23581cae7ULL, 0, 0, 0, 0 },
    { 58, 1, 0, 0x80a23581caebULL, 0x000080a23581caebULL, 0, 0, 0, 0 },
    { 508, 1, 1, 0x0180c2000020ULL, 0x10000180c2000020ULL, 1, 0, 1, 1 },
    { 509, 1, 0, 0x0180c2000020ULL, 0x00000180c2000020ULL, 0, 0, 1, 1 },
    { 510, 1, 1, 0x0180c2000000ULL, 0x10000180c2000000ULL, 1, 0, 1, 1 },
    { 511, 1, 0, 0x0180c2000000ULL, 0x00000180c2000000ULL, 0, 0, 1, 1 },
};

#define CUMULUS_L2_USER_ENTRY_COUNT (sizeof(cumulus_l2_user_entry_rows)/sizeof(cumulus_l2_user_entry_rows[0]))

#endif
