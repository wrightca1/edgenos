/* fm6000_bist.c - clean-room port of fm6000BistMemoryInit (libFocalpointSDK.so @0x34bb94):
 * the BM (built-in memory-BIST) march that configures the memory CONTROLLERS and runs the defect/repair
 * march. Boot order: BistMemoryInit -> MrlRegisterFix -> (InitSBus/sched) -> CRM bank fills. The CRM fill
 * off-buses without the memory-controller config this routine programs (phase90).
 *
 * Sequence decoded from the byte-identical SDK (md5 c0fc7562); march-op table at VA 0x6060c0. See
 * notes/COLD-INIT-MASTER-STATUS.md §13 and the phase90 SDK-mining report. Register model: word addr,
 * MMIO byte offset = word<<2.
 *
 *   fm6000_bist <BDF>
 * Build: gcc -O2 -o fm6000_bist fm6000_bist.c
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

/* scan-config + BM/SRBM march-engine registers (word addresses) */
#define R_CFG_DIN   0x1C03A   /* SCAN_CONFIG_DATA_IN (scan preamble)     */
#define R_SCAN_C    0x1C03C   /* scan data-out (read to advance)         */
#define R_BM_STATUS 0x1D08E   /* BM_ENGINE_STATUS (poll == 0 = done)     */
#define R_BM_IP     0x1D08C   /* BM interrupt-pending (result: want 0)   */

static volatile uint32_t *M;
static int g_pace_us = 0;   /* usleep between mem-controller writes (0 = none); set from env */
static inline void wr(uint32_t w, uint32_t v) { M[w] = v; __sync_synchronize(); }
static inline uint32_t rd(uint32_t w) { uint32_t v = M[w]; __sync_synchronize(); return v; }
/* paced write for the mem-controller block (0x1D2xx-0x1D6xx) — unpaced writes there hard-hang the host */
static inline void wrp(uint32_t w, uint32_t v) { M[w] = v; __sync_synchronize(); if (g_pace_us) usleep(g_pace_us); }

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

	g_pace_us = getenv("BIST_PACE_US") ? atoi(getenv("BIST_PACE_US")) : 0;
	int no_trigger = (getenv("BIST_NOTRIGGER") != NULL);   /* config controllers but do NOT run the BIST march */
	fprintf(stderr, "[bist] start (pace=%dus%s)\n", g_pace_us, no_trigger ? ", NOTRIGGER" : "");

	/* --- A. scan-config preamble (0x34bc45..0x34be13) --- */
	wr(R_CFG_DIN, 0x00000063); usleep(1);   /* fmDelay 640ns  */
	(void)rd(R_SCAN_C);
	wr(R_CFG_DIN, 0x80000063); usleep(1);
	wr(R_CFG_DIN, 0x88D55555); usleep(1);
	wr(R_CFG_DIN, 0x88009555); usleep(2);   /* fmDelay 1640ns */
	uint32_t st = rd(R_BM_STATUS);
	if (st != 0) { fprintf(stderr, "[bist] ABORT: BM engine busy pre-march (0x1D08E=0x%08x)\n", st); return 1; }

	/* --- B. load BM + SRBM march sequences (decoded march-op table, packed) --- */
	static const uint32_t march[4] = { 0x6529EDA9, 0x9B8ED9B1, 0xEFCA952B, 0x000FCA99 };
	for (int k = 0; k < 4; k++) wr(0x1D080 + k, march[k]);   /* BM_MARCH_SEQUENCE   */
	for (int k = 0; k < 4; k++) wr(0x1D708 + k, march[k]);   /* SRBM_MARCH_SEQUENCE */

	/* --- C. per-memory-controller config (PACED). Liveness-gated: read a SAFE control reg (PIN_STRAP
	 * 0x1C021, always readable, =0x208) after each group and BAIL on off-bus so we localize the culprit
	 * write WITHOUT then MMIO-touching a dead chip (which hard-hangs the host). --- */
#define LV(tag) do { uint32_t _p = rd(0x1C021); fprintf(stderr, "[bist] after %s: PIN_STRAP=0x%08x\n", tag, _p); \
	if (_p == 0xffffffff) { fprintf(stderr, "[bist] *** OFF-BUS after %s ***\n", tag); munmap((void*)M,len); close(fd); return 1; } } while (0)
	/* COMPLETE per-controller config — the full non-zero mem-controller register set read live from the
	 * WARM chip (fm6000-golden-sbus-crm-bm-warm.txt). The GEOMETRY regs (0x1Dxx1/2/3/4/5/9/c = address
	 * ranges + sizes) were MISSING from the earlier partial port -> incomplete BIST coverage (phase90). */
	static const uint32_t cfg[][2] = {
		/* Group A controllers @0x1D200 stride 0x80 */
		{0x1D210,0x00200000},{0x1D218,0x000000B4},{0x1D219,0x00000100},{0x1D221,0x0000017F},{0x1D222,0x0002BFF7},{0x1D223,0x0003BFE3},{0x1D224,0x20000000},{0x1D225,0x000203FF},{0x1D241,0x4},{0x1D242,0x300},{0x1D261,0x4},{0x1D262,0x300},{0x1D281,0x4},
		{0x1D290,0x00200000},{0x1D298,0x000000B4},{0x1D299,0x00000100},{0x1D2A1,0x0000007F},{0x1D2A2,0x00023FF7},{0x1D2A3,0x00033FE3},{0x1D2A4,0x20000000},{0x1D2A5,0x000203FF},{0x1D2C1,0x4},{0x1D2C2,0x300},
		{0x1D310,0x00200000},{0x1D318,0x000000B4},{0x1D319,0x00000100},{0x1D321,0x0000007F},{0x1D322,0x00023FF7},{0x1D323,0x00033FDF},{0x1D324,0x20000000},{0x1D325,0x000203FF},{0x1D341,0x2},{0x1D342,0x300},
		{0x1D390,0x00200000},{0x1D398,0x000000B4},{0x1D399,0x00000100},{0x1D3A1,0x0000007F},{0x1D3A2,0x000203FE},{0x1D3A3,0x000303FB},{0x1D3A4,0x20000000},{0x1D3A5,0x000203FF},{0x1D3C1,0x2},{0x1D3C2,0x300},
		/* Group B controllers @0x1D400 stride 0x80 */
		{0x1D400,0x00200000},{0x1D404,0x0000000C},{0x1D409,0x00000FFF},{0x1D40C,0x00000FFF},{0x1D440,0x1},{0x1D441,0x4},{0x1D442,0x300},
		{0x1D480,0x00200000},{0x1D484,0x0000000C},{0x1D489,0x00007FFF},{0x1D48C,0x00007FFF},{0x1D4C0,0x1},{0x1D4C1,0x4},{0x1D4C2,0x3F8},{0x1D4E0,0x1},{0x1D4E1,0x4},{0x1D4E2,0x380},
		{0x1D500,0x00200000},{0x1D504,0x0000000C},{0x1D509,0x00003FFF},{0x1D50C,0x00003FFF},{0x1D540,0x1},{0x1D541,0x4},{0x1D542,0x300},
		{0x1D580,0x00200000},{0x1D584,0x0000000C},{0x1D589,0x00000FFF},{0x1D58C,0x00000FFF},{0x1D5C0,0x1},{0x1D5C1,0x6},{0x1D5C2,0x3A0},{0x1D5E0,0x1},{0x1D5E1,0x6},{0x1D5E2,0x3FA},
		{0x1D600,0x00200000},{0x1D604,0x00000004},{0x1D609,0x000003FF},
	};
	int ncfg = (int)(sizeof(cfg)/sizeof(cfg[0]));
	if (getenv("BIST_FULLCFG")) {
		/* full warm geometry config — off-buses after BM march WITHOUT InitSBus first (phase90); revisit
		 * once InitSBus is ported. */
		for (int j = 0; j < ncfg; j++) { wrp(cfg[j][0], cfg[j][1]); if ((j & 0xf) == 0xf) LV("cfg-chunk"); }
		LV("full-config");
	} else {
		/* KNOWN-GOOD partial config (exp5/exp8: bank reads valid ECC). Subset of the warm set. */
		for (int n = 0; n < 4; n++) wrp(0x1D210 + n*0x80, 0x00200000);
		for (int n = 0; n < 5; n++) wrp(0x1D400 + n*0x80, 0x00200000);
		for (int n = 0; n < 4; n++) wrp(0x1D218 + n*0x80, 0x000000B4);
		wrp(0x1D241,4); wrp(0x1D2C1,4); wrp(0x1D261,4); wrp(0x1D281,4); wrp(0x1D2A1,4);
		for (int n = 0; n < 4; n++) wrp(0x1D404 + n*0x80, 0x0000000C);
		wrp(0x1D604,0x4);
		wrp(0x1D440,1); wrp(0x1D4C0,1); wrp(0x1D4E0,1); wrp(0x1D540,1); wrp(0x1D5C0,1); wrp(0x1D5E0,1); wrp(0x1D640,1); wrp(0x1D660,1);
		wrp(0x1D409,0xFFF); wrp(0x1D489,0x7FFF); wrp(0x1D509,0x3FFF); wrp(0x1D589,0xFFF); wrp(0x1D609,0x3FF);
		wrp(0x1D441,4); wrp(0x1D4C1,4); wrp(0x1D4E1,4); wrp(0x1D541,4); wrp(0x1D5C1,6); wrp(0x1D5E1,6); wrp(0x1D641,0xA); wrp(0x1D661,0xA);
		LV("partial-config");
	}
	/* run/kick bit (0x1D220 group, self-clears; warm reads 0) */
	for (int n = 0; n < 4; n++) wrp(0x1D220 + n*0x80, 3);   LV("0x1D220x4");

	if (no_trigger) { fprintf(stderr, "[bist] controllers configured; BIST march NOT triggered (NOTRIGGER)\n");
		munmap((void *)M, len); close(fd); return 0; }

	/* --- D. trigger (per-controller run/start writes; last before poll) --- */
	wrp(0x1D40B, 0); wrp(0x1D48B, 2); wrp(0x1D50B, 2); wrp(0x1D58B, 2); wrp(0x1D60B, 0);

	/* --- E. poll BM_ENGINE_STATUS == 0 (up to 5000 x 1ms = 5s) --- */
	int i;
	for (i = 0; i < 5000; i++) {
		uint32_t v = rd(R_BM_STATUS);
		if (v == 0) break;
		if (v == 0xffffffff) { fprintf(stderr, "[bist] OFF-BUS during march poll at %d\n", i); return 1; }
		usleep(1000);
	}
	if (i >= 5000) { fprintf(stderr, "[bist] TIMEOUT: BM march did not complete (0x1D08E!=0)\n"); return 1; }
	fprintf(stderr, "[bist] march complete after %d ms\n", i);

	/* --- F. harvest results (all want 0) --- */
	uint32_t bad = 0, v;
	v = rd(R_BM_IP);            if (v) { bad++; fprintf(stderr, "[bist] BM_IP 0x1D08C=0x%08x\n", v); }
	for (int n = 0; n < 4; n++) { v = rd(0x1D21B + n*0x80); if (v) { bad++; fprintf(stderr, "[bist] 0x%05x=0x%08x\n", 0x1D21B+n*0x80, v); } }
	for (int n = 0; n < 5; n++) { v = rd(0x1D407 + n*0x80); if (v) { bad++; fprintf(stderr, "[bist] 0x%05x=0x%08x\n", 0x1D407+n*0x80, v); } }
	v = rd(0x1D70E);           if (v) { bad++; fprintf(stderr, "[bist] SRBM 0x1D70E=0x%08x\n", v); }

	fprintf(stderr, "[bist] done: %s (%u non-zero result regs)\n", bad ? "MARCH DEFECTS/INCOMPLETE" : "clean", bad);
	munmap((void *)M, len); close(fd);
	return 0;   /* return 0 even with defects: repair table handles them; caller continues to MRL+CRM */
}
