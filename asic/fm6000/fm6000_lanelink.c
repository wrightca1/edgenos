/* fm6000_lanelink.c - bring up ONE EPL lane's SerDes, parameterised by front-panel port.
 *
 * fm6000_linkup.c replays a captured Et1 bring-up window verbatim: every SBus op
 * hardcoded to device 0x49, every EPL address to 0xe38xx (EPL14 lane 0), and 316
 * L2L-sweeper writes plus forwarding-table writes for logical id 0x3ee mixed in.
 * It brings up port 1 and nothing else, which is why port 3 has never linked
 * under EdgeNOS -- nothing in the boot touches lane 1 at all.
 *
 * The op table is the LIVE Ethernet3 down->up capture, segmented to just this
 * lane: 79 EPL-lane MMIO writes, 43 SBus ops to the lane's SerDes, and the 46
 * SPICO ops that belong to it. Segmentation uses the SPICO broadcast's reg-0x03
 * payload, which names the device the following block targets -- that is how the
 * 66 SPICO ops for other ports in the same window are excluded.
 *
 * It previously used fm6000_linkup's COLD Et1 window transposed to lane 1. That
 * ran cleanly on hardware and left the lane dark: the cold window holds 52 EPL
 * writes, 21 lane SBus ops and 16 SPICO ops, where a live bring-up does 79/43/46.
 * Parameterising the wrong sequence faithfully still gives the wrong sequence.
 *
 * Everything port-dependent is still derived, not transcribed:
 *
 *   MMIO address  = EPL_BASE + 0x400*epl + 0x80*lane + offset
 *   SBus device   = see sbus_dev() below
 *   TX equalisation = FM6000_SERDES_PORTS[].pre / .post, patched into
 *                     SERDES_TX_CFG TxOutputEqPre[14:12] / TxOutputEqPost[11:8]
 *   TX polarity     = FM6000_SERDES_PORTS[].txpol, patched into
 *                     SERDES_TX_CFG TxPolarityInvEn[30]. See patch_tx_cfg().
 *
 * The equalisation derivation is not a guess. The captured lane-0 sequence writes
 * SERDES_TX_CFG = 0xc0000581, and port 1's table entry is pre=0 post=5. Port 3's
 * entry is pre=1 post=5, which yields 0xc0001581 -- exactly the value measured on
 * a live EOS chip whose lane 1 was up. The one value that differs between the two
 * lanes falls straight out of the tuning table we already had.
 *
 * A second table follows the link ops: DFE[], the RX adaptation procedure, taken
 * from a LIVE capture of one fm6000StartSerDesDfeTuning() call on EOS rather than
 * from a boot window. See the comment on DFE[] and docs/PORT3-BRINGUP.md. Pass
 * -l to run the link ops alone, which is what this tool did before.
 *
 * ⚠ PROVENANCE. The 89-op sequence is derived from an EOS capture, the same class
 * of artifact as fwd4.txt, and it was already in this tree inside fm6000_linkup.c.
 * This does not make it clean -- it makes it smaller and parameterised instead of
 * transcribed once per port. Generating it from first principles is still open.
 * DFE[] is the same class of artifact and carries the same debt.
 *
 *   fm6000_lanelink [-n] [-l] [-b <bdf>] [-d <us>] <front-panel-port>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "fm6000_serdes_ports.h"

#define PIN     0x1C021u
#define SB_CMD  0x0F001u
#define SB_REQ  0x0F002u
#define EPL_BASE 0xE0000u        /* word address; EPL n lane l = +0x400*n +0x80*l */

#define SERDES_TX_CFG_OFF 0x3a   /* TxOutputEqPost[11:8], TxOutputEqPre[14:12],
                                  * TxPolarityInvEn[30] */
#define SEQ_DEV  0x4au           /* device the captured table was recorded from */
#define SPICO_BC 0xfdu           /* SPICO broadcast; its reg 3 payload names the target */

enum { OP_MMIO, OP_SBUS };
struct op { uint8_t kind; uint8_t off_or_sbop; uint32_t val; uint8_t reg; uint8_t dev; };

/* ★ THE OP SEQUENCES ARE NOT IN THIS FILE, AND MUST NOT BE.
 *
 * INIT[], SEQ[] and DFE[] were segmented captures of the VENDOR OS bringing a
 * lane up -- 963 ops of somebody else's program's behaviour, transcribed. That
 * is exactly what docs/PROVENANCE.md forbids shipping: "no verbatim
 * transcription of a proprietary program's data tables".
 *
 * They now load at runtime from an operator-supplied file, the same "bring your
 * own, from a licensed EOS" model the register replay already uses. Regenerate
 * it on your own switch with tools/seg_lane_trace.py.
 *
 *   default path:  /mnt/flash/fm6000_lanelink.ops   (override with -f)
 *   format:        <section> <kind> <off_or_sbop> <val> <reg> <dev>
 *
 * ⚠ WITHOUT THE FILE THIS TOOL DOES NOTHING AND SAYS SO. It must degrade to
 * "retrain unavailable", never to a failed boot -- fm6000-fullseq.sh calls it
 * only as a fallback when a lane comes up dirty, and on a healthy boot it is
 * never called at all (measured: et2 retrain attempts=0).
 */
#define LANELINK_OPS_DEFAULT "/mnt/flash/fm6000_lanelink.ops"

static struct op *INIT, *SEQ, *DFE;
static int N_INIT, N_SEQ, N_DFE;

static int op_kind(const char *s)
{
	if (!strcmp(s, "MMIO"))  return OP_MMIO;
	if (!strcmp(s, "SBUS"))  return OP_SBUS;
	return -1;
}

/* Reads the three sections into freshly allocated arrays. Returns 0 on success,
 * -1 if the file is absent or unusable (the caller must treat that as "no
 * retrain available", not as an error worth failing a boot over). */
static int load_ops(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[256];
	int cap_i = 0, cap_s = 0, cap_d = 0, lineno = 0;

	if (!f)
		return -1;
	while (fgets(line, sizeof line, f)) {
		char sec[16], kind[16];
		unsigned off, val, reg, dev;
		struct op o;
		struct op **arr;
		int *n, *cap;

		lineno++;
		if (line[0] == '#' || line[0] == '\n')
			continue;
		if (sscanf(line, "%15s %15s %x %x %x %x",
		           sec, kind, &off, &val, &reg, &dev) != 6) {
			fprintf(stderr, "%s:%d: malformed, ignored\n", path, lineno);
			continue;
		}
		if (op_kind(kind) < 0) {
			fprintf(stderr, "%s:%d: unknown kind %s\n", path, lineno, kind);
			continue;
		}
		o.kind = (uint8_t)op_kind(kind);
		o.off_or_sbop = (uint8_t)off;
		o.val = (uint32_t)val;
		o.reg = (uint8_t)reg;
		o.dev = (uint8_t)dev;

		if      (!strcmp(sec, "INIT")) { arr = &INIT; n = &N_INIT; cap = &cap_i; }
		else if (!strcmp(sec, "SEQ"))  { arr = &SEQ;  n = &N_SEQ;  cap = &cap_s; }
		else if (!strcmp(sec, "DFE"))  { arr = &DFE;  n = &N_DFE;  cap = &cap_d; }
		else { fprintf(stderr, "%s:%d: unknown section %s\n", path, lineno, sec); continue; }

		if (*n == *cap) {
			int nc = *cap ? *cap * 2 : 128;
			struct op *t = realloc(*arr, (size_t)nc * sizeof *t);
			if (!t) { fclose(f); return -1; }
			*arr = t; *cap = nc;
		}
		(*arr)[(*n)++] = o;
	}
	fclose(f);
	return (N_INIT || N_SEQ || N_DFE) ? 0 : -1;
}

/* LANE PROVISIONING, segmented out of the REPLAY (not out of a no-shut capture).
 *
 * The replay provisions Et3's lane exactly as it provisions the lanes with
 * nothing plugged into them -- byte-identical to EPL14 lane2 -- leaving
 * SERDES_TX_CFG TxEn=0 and SERDES_RX_CFG RxEn=0, and never issuing a single SBus
 * op to that lane's SerDes. Lane 0 gets 459 MMIO writes, 44 SBus ops and 200
 * SPICO ops; lane 1 gets 391 MMIO writes and NOTHING on the SBus.
 *
 * So SEQ[] below was always being applied to the wrong initial state: it is the
 * delta EOS applies to an already-provisioned lane when you `no shut` it. This
 * table is the provisioning itself, taken from Et1's lane (the working fibre
 * port) with tools/seg_lane_trace.py, and it runs BEFORE SEQ[].
 *
 * ⚠ CAPTURED FROM LANE 0, so unlike SEQ[]/DFE[] its source device is 0x49.
 *
 * ⚠ UNPROVEN THAT THIS RETARGETS. Every INACTIVE lane in the replay is
 * byte-identical (EPL14 lanes 1/2/3 and EPL16 lanes 1/2/3 all match), so the
 * baseline carries no lane-dependent values. But the capture contains only TWO
 * activated lanes, EPL14 lane0 and EPL16 lane0, and BOTH are lane 0 -- there is
 * no example anywhere of a non-zero lane being brought up. If any value in the
 * activation encodes the lane index, nothing in this data would reveal it.
 * Judge the result on PORT_STATUS bit 11 (SerXmit), which is 0 today and 1 on
 * both working ports. */
#define NINIT N_INIT
#define INIT_DEV 0x49u           /* device INIT[] was captured from (lane 0) */

#define NSEQ N_SEQ

/* RX adaptation (DFE tuning), segmented from the LIVE capture of one
 * fm6000StartSerDesDfeTuning(0,69,0) call on EOS -- see docs/PORT3-BRINGUP.md.
 * Same segmentation rules as SEQ: 29 EPL lane writes, 35 SBus ops to the lane's
 * SerDes, 30 SPICO ops claimed by the reg-0x03 payload rule. The 46 SPICO ops
 * and 6 SBus ops for lanes 0x45/0x49 in the same window are excluded.
 *
 * The capture was disarmed mid-block, so its last two ops -- a SPICO interrupt
 * opened with reg 0x01/0x02 and never given its reg-0x03 target -- are dropped
 * rather than replayed half-formed. The table ends on the last complete block.
 *
 * ⚠ The SPICO firmware performs the adaptation; these ops start it and poll it.
 * The polls are replayed as fixed reads, so this does NOT wait for convergence
 * the way fm6000CheckSerDesDfeTuningState does -- it issues the procedure. And
 * it is inert on an image built without SPICO (the fibre-only C1 option), since
 * there is then no firmware to run it. Neither point is yet tested on hardware. */
#define NDFE N_DFE

static volatile uint32_t *M;
static unsigned DLY = 20;
static int dry;
static int link_only;

static void wr(uint32_t w, uint32_t v)
{
	if (dry) { printf("    %06x <- %08x\n", w, v); return; }
	M[w] = v; __sync_synchronize(); if (DLY) usleep(DLY);
}
static uint32_t rd(uint32_t w) { uint32_t v = M[w]; __sync_synchronize(); return v; }

static int sbus(uint32_t cmd, uint32_t data)
{
	long i;
	if (dry) { printf("    SBUS cmd=%08x data=%08x\n", cmd, data); return 0; }
	wr(SB_REQ, data); wr(SB_CMD, 0); wr(SB_CMD, cmd);
	for (i = 0; i < 2000000L; i++) {
		uint32_t s = rd(SB_CMD);
		if (s == 0xffffffffu) return -2;
		if (!(s & (1u << 25))) return (int)((s >> 26) & 7);
	}
	return -3;
}

/* Observed SerDes SBus device addresses. Lane step is +1, confirmed by capturing
 * Ethernet3 down->up with fmPlatformTraceRegOps armed: EPL14 lane 1 is 0x4a.
 * The EPL step is inferred from two points only (EPL14 lane0 = 0x49, EPL16
 * lane0 = 0x45), so anything outside those is flagged rather than trusted. */
static int sbus_dev(unsigned epl, unsigned lane, int *observed)
{
	*observed = (epl == 14 && lane <= 1) || (epl == 16 && lane == 0);
	return 0x49 + (int)lane - 2 * ((int)epl - 14);
}

static const struct fm6000_serdes_port *find_port(unsigned intf)
{
	size_t i, n = sizeof FM6000_SERDES_PORTS / sizeof FM6000_SERDES_PORTS[0];
	for (i = 0; i < n; i++)
		if (FM6000_SERDES_PORTS[i].intf == intf) return &FM6000_SERDES_PORTS[i];
	return NULL;
}

/* TxOutputEqPre[14:12] and TxOutputEqPost[11:8] come from the port's tuning row. */
/* TxPolarityInvEn[30] compensates a differential pair routed P/N-swapped on the
 * PCB. It is a property of the board, not of the link, so it must come from the
 * port table -- NOT from the captured template, which was recorded on Et1.
 *
 * Et1 is swapped (txpol=1) and Et2 is not (txpol=0). Leaving bit 30 to pass
 * through unpatched gave every port Et1's inversion, so driving Et2 inverted a
 * lane that must not be inverted. Under 64b/66b that still locks -- inverting
 * the sync header turns 01 into 10, which is also legal -- but the descrambler
 * then sees garbage, which is a HiBer lock, and HiBer is exactly what a driven
 * Et2 reported (PORT_STATUS 0x9d5, bit 8 set).
 *
 * Two independent sources agree on the map: the final SERDES_TX_CFG values in
 * the EOS capture (Et1 0xc0000581 bit30=1, Et2 0x80001581 bit30=0) and EOS's
 * own CotatiP4.fdl altaSfpPorts, which is where the table came from.
 *
 * ⚠ Correcting this did NOT measurably improve Et2's lock rate: 0/10 against a
 * 2/10 baseline, Fisher p = 0.47 -- inconclusive either way at that power. It is
 * fixed because it is demonstrably wrong versus EOS, not because it is the cure.
 */
static uint32_t patch_tx_cfg(uint32_t v, const struct fm6000_serdes_port *p)
{
	v &= ~((0x7u << 12) | (0xfu << 8) | (1u << 30));
	v |= ((uint32_t)(p->pre & 0x7) << 12) | ((uint32_t)(p->post & 0xf) << 8);
	v |= (uint32_t)(p->txpol ? 1u : 0u) << 30;
	return v;
}

/* Run one op table, retargeting every port-dependent field to this lane. */
static int run_seq(const char *what, const struct op *seq, int n, uint32_t src_dev,
		   uint32_t base, int dev, const struct fm6000_serdes_port *p)
{
	int i;

	printf("  %s: %d ops\n", what, n);
	for (i = 0; i < n; i++) {
		const struct op *o = &seq[i];
		if (o->kind == OP_MMIO) {
			uint32_t v = o->val;
			if (o->off_or_sbop == SERDES_TX_CFG_OFF) v = patch_tx_cfg(v, p);
			wr(base + o->off_or_sbop, v);
		} else {
			/* Two device classes, and they are NOT interchangeable:
			 *   0x49 -- the lane's own SerDes; retarget to this lane's device
			 *   0xfd -- the SPICO broadcast; stays 0xfd, but its reg 0x03 write
			 *           carries the TARGET device as DATA, so that is what moves.
			 * Retargeting the broadcast device instead of its payload silently
			 * corrupted 16 of the 89 ops until a full-sequence diff caught it.
			 * The payload rule is confirmed against the lane-1 capture, which
			 * writes dev=0xfd reg=0x03 data=0x4a. */
			uint32_t d = o->dev == src_dev ? (uint32_t)dev : o->dev;
			uint32_t data = o->val;
			if (o->dev == SPICO_BC && o->reg == 0x03u && data == src_dev)
				data = (uint32_t)dev;
			uint32_t cmd = ((uint32_t)o->off_or_sbop << 16) |
				       ((d & 0xff) << 8) | o->reg | (1u << 24);
			int r = sbus(cmd, data);
			if (r < 0) {
				fprintf(stderr, "  %s: SBus op %d failed rc=%d\n", what, i, r);
				return 1;
			}
		}
		if (!dry && (i & 7) == 0 && rd(PIN) != 0x208u) {
			fprintf(stderr, "  %s: chip went off-bus at op %d\n", what, i);
			return 1;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *opspath = LANELINK_OPS_DEFAULT;
	const char *bdf = "0000:02:00.0";
	char path[256];
	int fd, i, observed, dev, rc = 0;
	long intf = -1;
	const struct fm6000_serdes_port *p;
	uint32_t base;

	for (i = 1; i < argc; i++) {
		if      (!strcmp(argv[i], "-n")) dry = 1;
		else if (!strcmp(argv[i], "-l")) link_only = 1;
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) bdf = argv[++i];
		else if (!strcmp(argv[i], "-d") && i + 1 < argc) DLY = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-f") && i + 1 < argc) opspath = argv[++i];
		else if (argv[i][0] == '-') { fprintf(stderr,
			"usage: fm6000_lanelink [-n] [-l] [-b bdf] [-d us] [-f ops] <front-panel-port>\n"
			"  -f  captured op sequences (default " LANELINK_OPS_DEFAULT ")\n"
			"  -l  link ops only, skip the DFE (RX adaptation) sequence\n"); return 2; }
		else intf = strtol(argv[i], NULL, 0);
	}

	/* No captured sequences -> nothing this tool can legitimately do. Say so
	 * plainly and exit 0: fm6000-fullseq.sh calls this only as a retrain
	 * fallback, and a missing operator file must never fail a boot. */
	if (load_ops(opspath) < 0) {
		fprintf(stderr,
			"fm6000_lanelink: no op sequences at %s -- lane retrain unavailable.\n"
			"  These are a segmented capture of the VENDOR OS bringing a lane up,\n"
			"  so they are not shipped. Regenerate on your own switch with\n"
			"  tools/seg_lane_trace.py and place the result there.\n", opspath);
		return 0;
	}
	fprintf(stderr, "fm6000_lanelink: loaded %d INIT + %d SEQ + %d DFE ops from %s\n",
	        N_INIT, N_SEQ, N_DFE, opspath);
	if (intf < 0 || !(p = find_port((unsigned)intf))) {
		fprintf(stderr, "unknown front-panel port %ld\n", intf); return 2;
	}

	base = EPL_BASE + 0x400u * p->epl + 0x80u * p->lane;
	dev  = sbus_dev(p->epl, p->lane, &observed);

	printf("port %u: alta %u, EPL%u lane %u -> MMIO base 0x%05x, SBus dev 0x%02x%s\n",
	       p->intf, p->alta, p->epl, p->lane, base, dev,
	       observed ? "" : "  (EXTRAPOLATED -- not an observed mapping)");
	printf("  tx eq: pre=%u post=%u (drive=%u)\n", p->pre, p->post, p->drive);
	if (!observed && !dry)
		fprintf(stderr, "  refusing to drive an unobserved SBus mapping; use -n to inspect\n");

	if (!dry) {
		if (!observed) return 1;
		snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
		fd = open(path, O_RDWR | O_SYNC);
		if (fd < 0) { perror("open"); return 1; }
		M = mmap(NULL, 32u*1024*1024, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
		if (M == MAP_FAILED) { perror("mmap"); return 1; }
		if (rd(PIN) != 0x208u) { fprintf(stderr, "chip off-bus (PIN=%08x)\n", rd(PIN)); return 1; }
	}

	if (!link_only && !dry) {
		uint32_t tx = rd(base + SERDES_TX_CFG_OFF), rx = rd(base + 0x39);
		printf("  before: TX_CFG=%08x (TxEn=%u)  RX_CFG=%08x (RxEn=%u)\n",
		       tx, tx & 1, rx, rx & 1);
	}
	if (!link_only)
		rc = run_seq("provision", INIT, NINIT, INIT_DEV, base, dev, p);
	if (!rc)
		rc = run_seq("link", SEQ, NSEQ, SEQ_DEV, base, dev, p);

	/* RX adaptation follows the link ops, and only if they completed -- tuning a
	 * lane whose bring-up aborted would adapt to a link that is not there. */
	if (!rc && !link_only) {
		if (!dry) {
			uint32_t st = rd(base + 0x00);
			printf("  after link: PORT_STATUS = %08x  pcsRx = %08x\n",
			       st, rd(base + 0x26));
		}
		rc = run_seq("dfe", DFE, NDFE, SEQ_DEV, base, dev, p);
	}

	if (!dry) {
		uint32_t st = rd(base + 0x00);
		printf("  PORT_STATUS = %08x  pcsRx = %08x\n", st, rd(base + 0x26));
	}
	printf("  %d ops %s\n", link_only ? NSEQ : NINIT + NSEQ + NDFE,
	       dry ? "(dry run)" : (rc ? "-- ABORTED" : "done"));
	return rc;
}
