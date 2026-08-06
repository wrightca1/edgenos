/* fm6000_sched_std.c - standalone (direct-mmap) FM6000 scheduler init for the cold clean-room sequence.
 * Ports the phase61/82 SSCHED ring-init + scheduler config to a self-contained tool (like fm6000_bist/mrl/
 * initsbus) with PIN_STRAP liveness checks. Runs AFTER BM-march + MRL + InitSBus, BEFORE the CRM fills —
 * fm6000ValidateSchedulerToken / the scheduler bring-up is the confirmed next CRM-fill gate (phase90).
 *
 * Phases (env-gated so ESCHED — which touches a block that off-buses on read — can be added incrementally):
 *   always:            TICK_CFG(0xF010)=2, SWEEPER_CFG(0x1C048..C) golden, ring init (INIT_TOKEN ->
 *                      NEXT_PORT table -> SLOW_PORT -> INIT_COMPLETE strobes, straight-line, NO poll)
 *   FM6000_SCHED_ESCHED: + ESCHED_CFG_1/2 (0x2000/0x2080 x76), DRR (0x3800 x76), replace tokens (0x8022/0x8062)
 *
 *   fm6000_sched_std <BDF>
 * Build: gcc -O2 -o fm6000_sched_std fm6000_sched_std.c
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

/* REPLACE_TOKEN find-probe (SDK fm6000ValidateSchedulerToken @0x3a7c9a): write OldPort=port (Rw=0),
 * wait ~50ms (fmDelay 0xc350), read back. RX 0x8062 -> Found bit21; TX 0x8022 -> FoundTok bit30.
 * Found=1 means the running engine walked the ring and located the token = the tick IS advancing.
 * Entirely within SSCHED (on-bus cold) -> SAFE, no ESCHED/MCAST/MOD off-bus read. */

static volatile uint32_t *M;
static inline void wr(uint32_t w, uint32_t v) { M[w] = v; __sync_synchronize(); }
static inline uint32_t rd(uint32_t w) { uint32_t v = M[w]; __sync_synchronize(); return v; }
#define PIN 0x1C021
#define TOKEN(port,locked,sync) (((uint32_t)(port)&0x7Fu) | (((uint32_t)(locked)&1u)<<9) | (((uint32_t)(sync)&1u)<<10))

static int live(const char *tag) {
	uint32_t p = rd(PIN);
	fprintf(stderr, "[sched] %s PIN_STRAP=0x%08x\n", tag, p);
	return p != 0xffffffff;
}

/* find-probe one port on a REPLACE_TOKEN reg. reg=0x8062(RX,foundbit=21) or 0x8022(TX,foundbit=30).
 * returns 1 if the engine located the token (ring circulating). SSCHED-only = safe cold. */
static int probe(uint32_t reg, unsigned port, int foundbit, const char *tag) {
	wr(reg, port & 0x7Fu);          /* OldPort=port, Rw=0 (find only) */
	usleep(50000);                  /* fmDelay 0xc350 */
	uint32_t v = rd(reg);
	int found = (v >> foundbit) & 1;
	fprintf(stderr, "[sched] PROBE %s reg=0x%04x port=%-2u -> 0x%08x FOUND=%d\n",
		tag, reg, port, v, found);
	return found;
}

/* write the egress-scheduler (ESCHED) per-port config + DRR. golden: port0 special, rest default.
 * returns 1 if chip stayed alive, 0 on off-bus. Checks liveness after the FIRST write and each 16. */
static int esched_cfg(void) {
	for (unsigned p = 0; p < 76; p++) {
		wr(0x2000 + p, p == 0 ? 0x00fff800u : 0x00ffffffu);   /* ESCHED_CFG_1 */
		wr(0x2080 + p, p == 0 ? 0x00fff000u : 0x00ffffffu);   /* ESCHED_CFG_2 */
		wr(0x2100 + p, 0x0u);                                  /* ESCHED_CFG_3 */
		if ((p == 0 || (p & 0xf) == 0xf) && !live("  esched cfg chunk")) return 0;
	}
	for (unsigned p = 0; p < 76; p++)
		wr(0x3800 + p, (p & 1) ? 0x14ffffffu : 0x00ffffffu);  /* ESCHED_DRR (MONITOR 0x3800) */
	return live("  after ESCHED cfg+DRR");
}

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
	int esched = (getenv("FM6000_SCHED_ESCHED") != NULL);
	int sr0 = (getenv("FM6000_SR0") != NULL);          /* SOFT_RESET(0x9)=0: full module enable */
	int esched_pre = (getenv("FM6000_ESCHED_PRE") != NULL); /* ECC-init ESCHED before INIT_COMPLETE */

	if (!live("start")) return 1;

	if (sr0) { wr(0x00009, 0x0); if (!live("after SOFT_RESET=0 (0x9=0)")) return 1; }

	/* tick + sweeper (scheduler-engine clock domain) */
	wr(0xF010, 0x2);                                   /* SSCHED_TICK_CFG period 2 */
	wr(0x1C048, 0x0008bb2c); wr(0x1C049, 0x2); wr(0x1C04A, 0x0);
	wr(0x1C04B, 0x0030a2c3); wr(0x1C04C, 0x2000);      /* SWEEPER_CFG 5 words */
	if (!live("after tick+sweeper")) return 1;

	/* ring init — straight-line, NO poll. GOLDEN tokens (SDK builder 0x3aac24 verified):
	 * ports 0-3 Locked=1,Sync=0; mgmt port 78 Locked=1,Sync=0 (NOT sync=1 — no golden token sets Sync).
	 * -> RX/TX INIT_TOKEN = 0x200..0x203 for ports 0-3, 0x24e for port 78. */
	/* FM6000_RING: NULL=golden(0-3,78); "mgmt"=port78 only; "empty"=no ports (idle bootstrap).
	 * Hypothesis: a ring with NO real data ports lets the engine circulate WITHOUT reading uninit
	 * ESCHED -> no off-bus -> Scheduler Manager comes on-bus -> MOD/MCAST/ESCHED become accessible. */
	const char *ring = getenv("FM6000_RING");
	static const struct tokrec { unsigned port, locked, sync; } tok_g[] = { {0,1,0},{1,1,0},{2,1,0},{3,1,0},{78,1,0} };
	static const struct tokrec tok_m[] = { {78,1,0} };
	const struct tokrec *tok = tok_g;
	unsigned ntok = 5;
	uint32_t np[20] = {0};
	if (ring && !strcmp(ring, "mgmt")) {
		tok = tok_m; ntok = 1;
		for (unsigned i = 0; i < 20; i++) np[i] = 0x4e4e4e4eu;   /* every slot = mgmt port 78 */
		fprintf(stderr, "[sched] RING=mgmt (port 78 only)\n");
	} else if (ring && !strcmp(ring, "empty")) {
		ntok = 0;                                                /* np all 0, no tokens */
		fprintf(stderr, "[sched] RING=empty (no ports)\n");
	} else {
		np[0] = 0x03020100u; np[19] = 0x004e0000u;               /* golden */
	}
	for (unsigned i = 0; i < ntok; i++) {
		uint32_t t = TOKEN(tok[i].port, tok[i].locked, tok[i].sync);
		wr(0x8060, t);   /* RX_INIT_TOKEN */
		wr(0x8020, t);   /* TX_INIT_TOKEN */
	}
	if (!live("after INIT_TOKENs")) return 1;
	/* NEXT_PORT visit table (20 words, TX 0x8000+i / RX 0x8040+i) */
	for (unsigned i = 0; i < 20; i++) { wr(0x8040 + i, np[i]); wr(0x8000 + i, np[i]); }
	if (!live("after NEXT_PORT")) return 1;
	/* SLOW_PORT (RX 0x8070+i) */
	static const uint32_t slow[] = { 0x0000000fu, 0x0000ffe0u, 0x0000feffu, 0x0000fff0u, 0x00000fffu };
	for (unsigned i = 0; i < 5; i++) wr(0x8070 + i, slow[i]);
	if (!live("after SLOW_PORT")) return 1;
	/* ECC-init the egress scheduler config BEFORE starting the engine, so the circulating engine
	 * reads clean ECC instead of off-busing (the SR0 TX_INIT_COMPLETE off-bus hypothesis). */
	if (esched_pre) {
		fprintf(stderr, "[sched] ESCHED pre-init (before INIT_COMPLETE)\n");
		if (!esched_cfg()) return 1;
	}
	/* COMMIT: write-1 strobes, RX then TX — split liveness to pinpoint which strobe engages/off-buses */
	wr(0x8061, 1);   /* RX_INIT_COMPLETE */
	if (!live("after RX_INIT_COMPLETE(0x8061)")) return 1;
	wr(0x8021, 1);   /* TX_INIT_COMPLETE */
	if (!live("after TX_INIT_COMPLETE(0x8021)")) return 1;

	/* ESCHED-accessibility probe: if the engine now circulates, the Scheduler Manager should be on-bus
	 * and ESCHED 0x2000 readable (not 0xffffffff). This tests the idle/mgmt-ring bootstrap. */
	if (getenv("FM6000_ESCHED_RD")) {
		uint32_t e = rd(0x2000);
		fprintf(stderr, "[sched] ESCHED 0x2000 read = 0x%08x -> %s\n",
			e, (e == 0xffffffffu) ? "OFF-BUS (mgr not circulating)" : "ON-BUS! (mgr circulating)");
		if (!live("after ESCHED read")) return 1;
	}

	/* CIRCULATION DIAGNOSTIC (default, SSCHED-only, SAFE): does the ring actually advance cold?
	 * Probe the ports we enrolled. FOUND=1 on any = engine is ticking (ring circulates) -> the
	 * remaining ESCHED off-bus is a separate issue. FOUND=0 on all = tick/clock NOT advancing the
	 * ring = the true wall (pivot to the scheduler clock/tick, not the token programming). */
	int found_any = 0;
	static const unsigned pp[] = {0,1,2,3,78};
	for (unsigned i = 0; i < sizeof(pp)/sizeof(pp[0]); i++) {
		found_any |= probe(0x8062, pp[i], 21, "RX");
		found_any |= probe(0x8022, pp[i], 30, "TX");
	}
	fprintf(stderr, "[sched] CIRCULATION: %s\n",
		found_any ? "FOUND -> ring is advancing (engine ticking)"
		          : "NOT FOUND -> ring NOT advancing (tick/clock wall)");
	if (!live("after probe")) return 1;

	if (esched && !esched_pre) {
		/* off-bus-RISKY post-init variant (writes ESCHED after the engine started). */
		if (!esched_cfg()) return 1;
	}

	uint32_t pin = rd(PIN);
	fprintf(stderr, "[sched] done%s: TX_NEXT_PORT[0]=0x%08x TICK=0x%08x PIN=0x%08x\n",
		esched ? " (+esched)" : " (ring only)", rd(0x8000), rd(0xF010), pin);
	munmap((void *)M, len); close(fd);
	return (pin == 0xffffffff) ? 1 : 0;
}
