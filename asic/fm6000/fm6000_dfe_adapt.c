/* fm6000_dfe_adapt.c - host-driven RX equaliser adaptation for one SerDes lane.
 *
 * ★ WHY. The FM6000's RX equaliser is normally adapted by SPICO, Intel's
 * firmware, which we have no permission to redistribute. Without it a fiber
 * (10GBASE-SR) lane still links -- the channel is clean enough -- but a copper
 * DAC (10GBASE-CR) lane never acquires: measured, LANE_STATUS stays 0x000
 * (docs/EDGENOS-7150.md (was SPICO-RE)).
 *
 * ⚠ AND THE "TUNE LOOP" WE INHERITED IS NOT TUNING. fm6000_serdes_enable.c
 * writes 0x2a and polls 0x2b, which sdk_fieldmap.py --sbus names
 * `sbus_dfe_scratch_pad_cntl` -- a MAILBOX to SPICO, not a hardware engine. It
 * "completes" only because SPICO answers it. So replacing SPICO means running
 * the adaptation ourselves against the real controls, which is what this does.
 *
 * ★ THE PARAMETERS, recovered by sdk_fieldmap.py --sbus. Note the `[3]` fields:
 * a 4-bit value whose top bit lives in a DIFFERENT register from its low three.
 * Getting that wrong silently searches a quarter of the space.
 *
 *     ctl_1  4b   0x2e[7:5] (low 3) + 0x21[0] (bit 3)
 *     ctl_2  4b   0x21[4:1]
 *     ctl_3  4b   0x21[7:5] (low 3) + 0x20[0] (bit 3)
 *     ctl_4  4b   0x20[4:1]
 *     adv    4b   0x20[7:5] (low 3) + 0x1f[0] (bit 3)
 *     dac    6b   0x1f[6:1]
 *     ctl_i  4b   0x2e[1:4]
 *
 * ★ THE METRIC. The SerDes has its own error monitor, so we do not have to
 * infer link quality from PCS counters:
 *
 *     0x04  bit7 enable, bit6 reset, bits1-4 source select, bit0 gate
 *     0x08  sbus_rx_error_count_obs (8 bits)
 *     0x01  sbus_rx_error_occured_obs, 8b10b disparity / out-of-band flags
 *
 * Measured 0 on both healthy lanes, which is the necessary sanity check; the
 * tool prints the metric so a lane that reads 0 while DOWN is visible as a
 * USELESS metric rather than a good score. Read the output, do not trust the
 * search blindly.
 *
 * ⚠ SerDes control registers are writable only on a LIVE (clocked) lane. A
 * sweep on a cold lane silently does nothing and looks like "read-only".
 *
 * ⚠⚠ STATUS 2026-08-21: THIS DOES NOT YET WORK, AND THE BLOCKER IS NOT THE
 * SEARCH. Two measured facts stop it:
 *
 *   1. THE DFE CONTROLS REJECT WRITES ON A COLD LANE. Injecting a known-good
 *      profile captured from the same DAC cable while it was up
 *      (ctl_2=12 ctl_3=1 ctl_4=4 adv=14 dac=18) into the cold lane changed
 *      NOTHING -- every value that needed to move read back unchanged; only the
 *      two already at their target "stuck". The DFE power-down bits
 *      (0x26 offset_pd/data_pd) are already clear on both lanes, so that is not
 *      the gate. On a LIVE lane a write does land (confirmed by accidentally
 *      destabilising et2 with one), so writability depends on some lane state we
 *      have not identified -- plausibly something SPICO itself establishes.
 *
 *   2. THE ERROR COUNTER IS THE WRONG METRIC FOR ACQUISITION. 0x08 reads 0 on a
 *      DEAD lane exactly as it does on a healthy one, because with no lock there
 *      is no decoded data to count errors in. The signal that actually separates
 *      them is 0x01: 0x25 on a working lane vs 0x00 on the dead one, i.e.
 *      `sbus_rx_8b10b_comma_det_obs`. Any future search must score on comma
 *      detect first and use the error count only as a tiebreaker.
 *
 * So the ordering is: make the DFE block writable on a cold lane FIRST. Until
 * that is solved a search has nothing to actuate, and this tool can only report.
 * --show and -n are useful today; the descent is not.
 *
 * usage: fm6000_dfe_adapt [-b bdf] -d <dev> [-p passes] [-m ms] [-n] [--show]
 *   -d   SBus device: 0x49 = et1, 0x45 = et2, 0x4a = port 3
 *   -n   dry run: measure and report, change nothing
 *   --show  print the current parameter values and exit
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

#define SB_CMD  0x0F001u
#define SB_REQ  0x0F002u
#define SB_RESP 0x0F003u
#define OP_READ  0x22u
#define OP_WRITE 0x21u

static volatile uint32_t *M;
static unsigned DEV;

static uint32_t rd(uint32_t a) { return M[a]; }
static void wr(uint32_t a, uint32_t v) { M[a] = v; __sync_synchronize(); }

static long sbus(unsigned op, unsigned reg, uint32_t data)
{
	long i;
	wr(SB_REQ, data);
	wr(SB_CMD, 0);
	wr(SB_CMD, (op << 16) | ((DEV & 0xff) << 8) | (reg & 0xff) | (1u << 24));
	for (i = 0; i < 200000; i++) {
		uint32_t s = rd(SB_CMD);
		if (s == 0xffffffffu) return -1;
		if (!(s & (1u << 25))) return (long)rd(SB_RESP);
	}
	return -1;
}

static unsigned rreg(unsigned r) { long v = sbus(OP_READ, r, 0); return v < 0 ? 0 : (unsigned)(v & 0xff); }
static void wreg(unsigned r, unsigned v) { sbus(OP_WRITE, r, v & 0xff); }

static void ms(unsigned n)
{
	struct timespec t = { n / 1000, (long)(n % 1000) * 1000000L };
	nanosleep(&t, NULL);
}

/* ★ THE COMMIT. Writing coefficients alone changes nothing the hardware uses:
 * `_gate`/`_sel` bits in 0x26 arbitrate between the hardware's own values and
 * the host-supplied `_cntl` ones. fm6000SetSerDesDfeParams @0x48c29b writes its
 * eight coefficient registers and THEN read-modify-writes 0x26, forcing bit 2
 * (`sbus_sel_dfe_a_data_cntl`) set with a literal `or eax,0x4`, and putting the
 * caller's flag in bit 0 (`sbus_rx_dfe_gate`).
 *
 * Not doing this is the most likely reason every earlier injection looked like
 * it "did not stick": the write may well have landed, while the readback kept
 * reporting the value actually in use. */
static void dfe_commit(int gate)
{
	unsigned v = rreg(0x26);
	v = (v & ~1u) | (gate ? 1u : 0u);
	v |= 0x4u;                     /* sbus_sel_dfe_a_data_cntl */
	wreg(0x26, v);
}

/* read-modify-write a bitfield inside one SBus register */
static void putf(unsigned reg, unsigned lsb, unsigned width, unsigned val)
{
	unsigned mask = ((1u << width) - 1u) << lsb;
	unsigned cur = rreg(reg);
	wreg(reg, (cur & ~mask) | ((val << lsb) & mask));
}
static unsigned getf(unsigned reg, unsigned lsb, unsigned width)
{
	return (rreg(reg) >> lsb) & ((1u << width) - 1u);
}

/* The seven parameters. `hi_reg` != 0 marks a value split across registers:
 * low three bits in (reg,lsb), top bit in (hi_reg,hi_lsb). */
struct param {
	const char *name;
	unsigned reg, lsb, width;   /* low part */
	unsigned hi_reg, hi_lsb;    /* top bit, 0 = none */
	unsigned bits;              /* total width */
};
static const struct param P[] = {
	{ "ctl_1", 0x2e, 5, 3, 0x21, 0, 4 },
	{ "ctl_2", 0x21, 1, 4, 0,    0, 4 },
	{ "ctl_3", 0x21, 5, 3, 0x20, 0, 4 },
	{ "ctl_4", 0x20, 1, 4, 0,    0, 4 },
	{ "adv",   0x20, 5, 3, 0x1f, 0, 4 },
	{ "dac",   0x1f, 1, 6, 0,    0, 6 },
	{ "ctl_i", 0x2e, 1, 4, 0,    0, 4 },
};
#define NP ((int)(sizeof P / sizeof P[0]))

static unsigned pget(const struct param *p)
{
	unsigned v = getf(p->reg, p->lsb, p->width);
	if (p->hi_reg)
		v |= getf(p->hi_reg, p->hi_lsb, 1) << p->width;
	return v;
}
static void pset(const struct param *p, unsigned v)
{
	putf(p->reg, p->lsb, p->width, v & ((1u << p->width) - 1u));
	if (p->hi_reg)
		putf(p->hi_reg, p->hi_lsb, 1, (v >> p->width) & 1u);
}

/* Lower is better. Counter is 8-bit and saturates, so a long window cannot
 * distinguish "bad" from "terrible" -- keep it short and repeat. */
/* Acquisition score, lower is better. The error counter is USELESS before lock
 * -- it reads 0 on a dead lane exactly as on a healthy one, because there is no
 * decoded data to count. What separates them is 0x01 bit 5,
 * `sbus_rx_8b10b_comma_det_obs`: 0x25 on a working lane, 0x00 on a dead one.
 * (The eye score would be better still, but fm6000GetSerDesEyeScore goes through
 * fm6000InterruptSpico -- it is a SPICO service, so it is not available to us.) */
static unsigned score(unsigned window_ms);

static unsigned measure(unsigned window_ms)
{
	unsigned sel = (rreg(0x04) >> 1) & 0xf;
	wreg(0x04, 0x80u | 0x40u | (sel << 1));   /* enable + reset */
	wreg(0x04, 0x80u | (sel << 1));           /* release, count */
	ms(window_ms);
	return rreg(0x08);
}

static unsigned score(unsigned window_ms)
{
	unsigned st = rreg(0x01);
	unsigned comma = (st >> 5) & 1u;
	return (comma ? 0u : 1000u) + measure(window_ms);
}

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	int passes = 3, window = 20, dry = 0, show = 0, i, fd;
	const char *setspec = NULL;
	char path[256];

	DEV = 0;
	for (i = 1; i < argc; i++) {
		if      (!strcmp(argv[i], "-b") && i + 1 < argc) bdf = argv[++i];
		else if (!strcmp(argv[i], "-d") && i + 1 < argc) DEV = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-p") && i + 1 < argc) passes = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-m") && i + 1 < argc) window = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-n")) dry = 1;
		else if (!strcmp(argv[i], "--show")) show = 1;
		else if (!strcmp(argv[i], "--set") && i + 1 < argc) setspec = argv[++i];
		else { fprintf(stderr, "usage: %s [-b bdf] -d <dev> [-p passes] [-m ms] [-n] [--show]\n", argv[0]); return 2; }
	}
	if (!DEV) { fprintf(stderr, "need -d <sbus dev>\n"); return 2; }

	snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
	fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0) { perror("open resource0"); return 1; }
	M = mmap(NULL, 32u * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (M == MAP_FAILED) { perror("mmap"); return 1; }

	printf("dev 0x%02x  regs 1f=%02x 20=%02x 21=%02x 2e=%02x  status01=%02x\n",
	       DEV, rreg(0x1f), rreg(0x20), rreg(0x21), rreg(0x2e), rreg(0x01));
	for (i = 0; i < NP; i++)
		printf("  %-6s = %u\n", P[i].name, pget(&P[i]));
	if (show) return 0;

	/* --set name=val,... : apply a known-good profile directly. This is the
	 * fast test of "can the host place the coefficients at all", separate
	 * from whether a search can FIND them. */
	if (setspec) {
		char buf[256], *t;
		snprintf(buf, sizeof buf, "%s", setspec);
		for (t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
			char *eq = strchr(t, '=');
			if (!eq) continue;
			*eq = 0;
			for (i = 0; i < NP; i++)
				if (!strcmp(P[i].name, t)) {
					unsigned want = (unsigned)strtoul(eq + 1, NULL, 0);
					pset(&P[i], want);
					printf("  set %-6s = %u (readback %u)%s\n", P[i].name, want,
					       pget(&P[i]), pget(&P[i]) == want ? "" : "   <-- DID NOT STICK");
				}
		}
		dfe_commit(1);
		ms(5);
		printf("after set+commit: 1f=%02x 20=%02x 21=%02x 2e=%02x 26=%02x"
		       "  status01=%02x err=%u\n",
		       rreg(0x1f), rreg(0x20), rreg(0x21), rreg(0x2e), rreg(0x26),
		       rreg(0x01), measure(window));
		return 0;
	}

	unsigned base = score(window);
	printf("baseline score = %u (err=%u comma=%u, window %u ms)\n",
	       base, measure(window), (rreg(0x01) >> 5) & 1u, window);
	if (dry) return 0;

	/* Coordinate descent. Not gradient descent: the metric is an 8-bit
	 * saturating counter, so it is noisy and flat-topped, and a greedy sweep
	 * per parameter is the honest thing to run against it. */
	unsigned bestv[NP], best = base;
	for (i = 0; i < NP; i++) bestv[i] = pget(&P[i]);

	int pass;
	for (pass = 0; pass < passes; pass++) {
		int improved = 0;
		for (i = 0; i < NP; i++) {
			unsigned n = 1u << P[i].bits, v, keep = bestv[i], localbest = best;
			for (v = 0; v < n; v++) {
				pset(&P[i], v);
				ms(2);
				unsigned e = score(window);
				if (e < localbest) { localbest = e; keep = v; improved = 1; }
			}
			pset(&P[i], keep);
			bestv[i] = keep;
			best = localbest;
			printf("  pass %d %-6s -> %2u  err=%u\n", pass, P[i].name, keep, best);
		}
		if (!improved) { printf("  converged after pass %d\n", pass); break; }
	}

	printf("final: ");
	for (i = 0; i < NP; i++) printf("%s=%u ", P[i].name, bestv[i]);
	printf(" err=%u  status01=%02x\n", best, rreg(0x01));
	return 0;
}
