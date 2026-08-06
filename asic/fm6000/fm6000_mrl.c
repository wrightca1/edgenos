/* fm6000_mrl.c - faithful port of fm6000MrlRegisterFix (libFocalpointSDK.so @0x47a4bc):
 * the SCAN-CHAIN memory-block configuration that makes the FM6000 banked memories writable.
 * Without it, the CRM ECC-fill and every direct bank write off-bus the chip (phase78/79).
 *
 * This is a LIVE read-write handshake (not a static replay): it writes scan-select + scan-data
 * per block, then reads 0x1C03D/0x1C03C back (the reads clock/advance the scan chain) and branches
 * on the readback. Procedure reimplemented from the disassembly. The scan-config VALUES are
 * third-party data and are NOT in this tree -- they are loaded at runtime from a data file
 * (FM6000_MRL_TABLE=<file>, see fm6000_mrl_table.h).
 *
 *   fm6000_mrl <BDF>
 * Run AFTER PCIe enum + BistMemoryInit, BEFORE the CRM bank fill.
 * Build: gcc -O2 -o fm6000_mrl fm6000_mrl.c
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include "fm6000_mrl_table.h"

/* scan-config registers (word addresses; vendor header) */
#define R_SCAN_SEL   0x1C039   /* scan select/enable                       */
#define R_CFG_DIN    0x1C03A   /* SCAN_CONFIG_DATA_IN                       */
#define R_CHAIN_DIN  0x1C03B   /* SCAN_CHAIN_DATA_IN                        */
#define R_SCAN_C     0x1C03C   /* scan data-out (read to advance)          */
#define R_SCAN_D     0x1C03D   /* scan status/data-out (read + poll)       */
#define R_CAM0       0x0E000

static volatile uint32_t *M;

static inline void wr(uint32_t w, uint32_t v) { M[w] = v; __sync_synchronize(); }
static inline uint32_t rd(uint32_t w) { uint32_t v = M[w]; __sync_synchronize(); return v; }

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s <BDF>\n", argv[0]); return 2; }
	char path[256];
	snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", argv[1]);
	int fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0) { perror("open resource0"); return 1; }
	size_t len = 32u * 1024 * 1024;
	M = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (M == MAP_FAILED) { perror("mmap"); return 1; }

	/* NOTE (phase90, disasm re-verify of 0x47a4bc): fm6000MrlRegisterFix issues ZERO accesses outside
	 * 0x1C039-0x1C03D for the whole routine. Reading a BANKED memory (CAM0 0x0E000) mid-scan off-buses the
	 * chip — the scan shift holds those banks inaccessible → PCIe completion-timeout. The old in-loop CAM0
	 * "liveness" watchpoint was SELF-INFLICTING the block-1024 off-bus. Only touch scan regs during the loop;
	 * check CAM0 (banks accessible again) AFTER the post-loop commit + settle. */
	if (fm6000_mrl_table_load() < 0)
		return 1;

	fprintf(stderr, "[mrl] start: %d blocks (scan-only; no bank reads mid-loop)\n", FM6000_MRL_ENTRIES);

	/* pre-loop: write32(0x1C039, 0x10) */
	wr(R_SCAN_SEL, 0x10);

	int first_ff = -1;               /* first block where SCAN_D reads all-ones (safe off-bus proxy) */
	uint32_t first_ff_t1 = 0, first_ff_t2 = 0;
	for (int i = 0; i < FM6000_MRL_ENTRIES; i++) {
		uint32_t t1 = fm6000_mrl_table[i][0];
		uint32_t t2 = fm6000_mrl_table[i][1];

		wr(R_SCAN_SEL, t1 & 0x1f);
		if (t1 & 0x80) wr(R_CFG_DIN, t2);
		else           wr(R_CHAIN_DIN, t2);

		uint32_t s = rd(R_SCAN_D);
		if ((s & 0x300) != 0x100) s = rd(R_SCAN_D);   /* vendor: single optional re-read */
		(void)rd(R_SCAN_C);                            /* read to advance the scan chain */

		if (s == 0xffffffff && first_ff < 0) { first_ff = i; first_ff_t1 = t1; first_ff_t2 = t2; }
	}
	fprintf(stderr, "[mrl] scan loop done; first SCAN_D=0xffffffff at block %d (t1=0x%x t2=0x%x)\n",
		first_ff, first_ff_t1, first_ff_t2);

	/* final commit + 20ms settle. NOTE: this write sets 0x1C03A=0x80000040 = block clocks OFF. Do NOT read
	 * any BANKED memory (CAM0/MCAST/...) after this until the caller re-enables block clocks (0x1C03A=
	 * 0xffffffff) — a bank read with clocks off times out → PCIe off-bus (phase90; the scan itself is clean). */
	wr(R_CFG_DIN, 0x80000040);
	usleep(20000);
	uint32_t scan_post_commit = rd(R_SCAN_D);  /* scan reg only — safe liveness */
	fprintf(stderr, "[mrl] done: scan complete, SCAN_D post-commit=0x%08x %s"
		" (clocks now OFF via 0x1C03A=0x80000040; caller must re-enable before bank access)\n",
		scan_post_commit, scan_post_commit == 0xffffffff ? "*** off-bus ***" : "alive");
	munmap((void *)M, len); close(fd);
	return scan_post_commit == 0xffffffff ? 1 : 0;
}
