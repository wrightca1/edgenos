/* fm6000_wr128.c - atomic 128-bit (4-word) write to an FM6000 wide-table entry.
 *
 * FM6000 tables wider than 32 bits are ATOMIC (datasheet 8.3): write the entry's
 * words LEAST-significant first, terminating with the MOST-significant word which
 * commits the whole entry with HW-computed ECC. There is only ONE temp-cache per
 * bus master and ANY interleaved read disturbs it -> a single-word write, or a
 * per-word read-back tool (fm6000reg), corrupts the ECC and a later read off-buses.
 * This tool mmaps BAR0 ONCE and writes all 4 words with NO reads in between.
 *
 * Usage: fm6000_wr128 <base_word_hex> <w0> <w1> <w2> <w3>   (w0=LSW @base, w3=MSW commits)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include "fm6000_hw.h"

int main(int argc, char **argv)
{
    struct fm6000_dev dev;
    if (argc != 6) { fprintf(stderr, "usage: %s <base_word> <w0> <w1> <w2> <w3>\n", argv[0]); return 2; }
    if (fm6000_hw_open(&dev) < 0) { fprintf(stderr, "fm6000_wr128: cannot open BAR0\n"); return 1; }
    uint32_t base = (uint32_t)strtoul(argv[1], NULL, 0);
    uint32_t w0 = (uint32_t)strtoul(argv[2], NULL, 0);
    uint32_t w1 = (uint32_t)strtoul(argv[3], NULL, 0);
    uint32_t w2 = (uint32_t)strtoul(argv[4], NULL, 0);
    uint32_t w3 = (uint32_t)strtoul(argv[5], NULL, 0);
    /* LSW -> MSW, no interleaved reads; base+3 (MSW) commits the 128-bit entry + ECC */
    fm6000_csr_write(&dev, base + 0, w0);
    fm6000_csr_write(&dev, base + 1, w1);
    fm6000_csr_write(&dev, base + 2, w2);
    fm6000_csr_write(&dev, base + 3, w3);
    fprintf(stderr, "fm6000_wr128: wrote 0x%05x = %08x %08x %08x %08x (MSW committed)\n",
            base, w0, w1, w2, w3);
    fm6000_hw_close(&dev);
    return 0;
}
