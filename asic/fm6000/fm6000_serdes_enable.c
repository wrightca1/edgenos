/* fm6000_serdes_enable.c - cold-enable one SerDes lane, as an ALGORITHM.
 *
 * WHY THIS EXISTS
 *
 * EOS's captured replay contains SerDes bring-up for exactly two devices --
 * 0x49 (front port 1) and 0x45 (port 2), 44 and 45 SBus ops. Device 0x4a
 * (port 3) gets ZERO. Port 3's EPL/MAC/PCS registers ARE written, which is why
 * they compare byte-identical to a working lane; only the SerDes half is
 * missing. See docs/PORT3-BRINGUP.md.
 *
 * fm6000_lanelink replays the captured op list, and that cannot work for a cold
 * lane for two structural reasons (docs/EOS-SOURCES.md):
 *
 *   - the writes are READ-MODIFY-WRITES, so replaying one lane's resulting
 *     values into another writes numbers derived from the wrong lane;
 *   - two of the steps are WAITS, and a capture has no notion of waiting -- it
 *     records the polls that happened to succeed and a replay races past them.
 *
 * This implements the algorithm instead, recovered from fm6000EnableSerDes
 * (libFocalpointSDK.so @0x48131e). Facts about the silicon in our own words;
 * nothing is copied from the SDK.
 *
 * ⚠ CORRECTION 2026-08-21: THE "DFE TUNE" LOOP IS A MAILBOX TO SPICO.
 * The loop below writes 0x2a = 0x16 then 0x0e and polls 0x2b for 0x04, and this
 * file used to describe that as driving a hardware tuning engine. It is not.
 * sdk_fieldmap.py --sbus names both registers `sbus_dfe_scratch_pad_cntl` -- a
 * SCRATCH PAD. The host posts a command, the SPICO firmware reads it, runs the
 * RX equaliser adaptation and posts status back.
 *
 * Consequences, all confirmed on hardware: the loop "completes" only when SPICO
 * is loaded (it is what answers), on a stripped boot it oscillates forever, and
 * driving 0x2b to the value a working lane shows proves nothing because it is a
 * message rather than tuning state. The real DFE controls are 0x1f
 * (sbus_dfe_a_adv_cntl / _dac_cntl), 0x21 (ctl_1/2/3) and 0x2e (ctl_i) -- see
 * docs/SPICO-RE.md. Replacing SPICO means implementing the adaptation against
 * those, not re-triggering a state machine.
 *
 * ADDRESSING. The SDK computes  (lane << 8) + 0xd11RR,  low byte = SBus
 * register. Our SBus transaction takes (op, dev, reg) directly, which is the
 * same thing: dev selects the lane, reg is that low byte.
 *
 * THE TWO WAITS, disassembled exactly (both fmDelay(0, 1000000 ns) = 1 ms):
 *
 *   PLL lock       reg 0x0f, bits 0 and 3   (sbus_rx_rdy_obs)
 *   signal detect  reg 0x14, bit 6          (sbus_rx_ib_sig_strength_obs)
 *                  up to 0x1387 = 4999 retries, i.e. a 5 second budget
 *
 * That budget is the whole point. fm6000_lanelink issues about seven reads and
 * moves on; `-d 20000` was tried and is three orders of magnitude short.
 *
 * ⚠ INCOMPLETE, AND IT SAYS SO AT RUNTIME. Four steps are not implemented:
 * steps 3-6 write values produced by arithmetic in the SDK that has not been
 * decoded, and steps 2/14/17/18 are separate SDK functions (KrTraining,
 * SetTxConfig, StartSerDesDfeTuning, RxDataGate/NearLoopback). What is here is
 * every step whose operation is known exactly. If the lane does not come up,
 * the missing steps are the place to look -- not the ones below.
 *
 * ⚠ READBACK PROVES NOTHING on this bus. A completed write reads back the old
 * value (see fm6000_sbus.c). The acceptance test is behavioural: PORT_STATUS
 * bit 11, SerXmit -- 0 on a dark lane, 1 on a live one.
 *
 * usage: fm6000_serdes_enable [-n] [-b bdf] <front-panel-port>
 *   -n  dry run: print the sequence, touch nothing
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>

#define SB_CMD 0x0F001u
#define SB_REQ 0x0F002u
#define SB_RESP 0x0F003u
#define OP_READ  0x22u
#define OP_WRITE 0x21u
#define EPL_BASE 0xE0000u

/* front port -> EPL, lane, SBus device. From FM6000_SERDES_PORTS[] and the
 * device mapping confirmed on hardware (EPL14 lane0 = 0x49, lane1 = 0x4a,
 * EPL16 lane0 = 0x45); devices are +1 per lane within an EPL. */
static const struct { int port, epl, lane, dev; } PORTS[] = {
	{ 1, 14, 0, 0x49 }, { 2, 16, 0, 0x45 }, { 3, 14, 1, 0x4a },
	{ 4, 16, 1, 0x46 }, { 5, 14, 2, 0x4b }, { 6, 16, 2, 0x47 },
	{ 7, 14, 3, 0x4c }, { 8, 16, 3, 0x48 },
};

static volatile uint32_t *M;
static int dry;

static uint32_t rd(uint32_t w) { uint32_t v = M[w]; __sync_synchronize(); return v; }
static void     wr(uint32_t w, uint32_t v) { M[w] = v; __sync_synchronize(); }

static void ms(long n)
{
	struct timespec t = { n / 1000, (n % 1000) * 1000000L };
	nanosleep(&t, NULL);
}

static long sbus(unsigned op, unsigned dev, unsigned reg, uint32_t data)
{
	long i;
	wr(SB_REQ, data);
	wr(SB_CMD, 0);
	wr(SB_CMD, (op << 16) | ((dev & 0xff) << 8) | (reg & 0xff) | (1u << 24));
	for (i = 0; i < 200000; i++) {
		uint32_t s = rd(SB_CMD);
		if (s == 0xffffffffu) return -1;
		if (!(s & (1u << 25))) return (long)rd(SB_RESP);
	}
	return -1;
}

/* read-modify-write: v = (v & and_m) | or_m, then ^ xor_m */
static int rmw(unsigned dev, unsigned reg, uint32_t and_m, uint32_t or_m,
               uint32_t xor_m, const char *what)
{
	long v;
	uint32_t n;
	if (dry) { printf("  rmw  reg %02x  &%08x |%08x ^%08x   (%s)\n",
	                  reg, and_m, or_m, xor_m, what); return 0; }
	v = sbus(OP_READ, dev, reg, 0);
	if (v < 0) { fprintf(stderr, "  read reg %02x failed\n", reg); return 1; }
	n = (((uint32_t)v & and_m) | or_m) ^ xor_m;
	if (sbus(OP_WRITE, dev, reg, n) < 0) {
		fprintf(stderr, "  write reg %02x failed\n", reg); return 1;
	}
	printf("  rmw  reg %02x  %02lx -> %02x   (%s)\n", reg, v & 0xff, n & 0xff, what);
	return 0;
}

/* Poll until every bit in `bits` is set. 1 ms apart, 5000 tries, exactly as
 * fm6000WaitFor{SerDesPllLock,SignalDetection} do. Returns 0 on success. */
static int wait_bits(unsigned dev, unsigned reg, uint32_t bits, const char *what)
{
	int i;
	if (dry) { printf("  wait reg %02x bits %02x, 1ms x5000 (%s)\n",
	                  reg, bits, what); return 0; }
	for (i = 0; i < 5000; i++) {
		long v;
		ms(1);
		v = sbus(OP_READ, dev, reg, 0);
		if (v < 0) { fprintf(stderr, "  poll reg %02x failed\n", reg); return 1; }
		if (((uint32_t)v & bits) == bits) {
			printf("  wait reg %02x -> %02lx after %d ms   (%s)\n",
			       reg, v & 0xff, i + 1, what);
			return 0;
		}
	}
	printf("  wait reg %02x TIMEOUT after 5000 ms   (%s)\n", reg, what);
	return 1;
}

/* ---- SPICO interrupt path, from DPDK's open fm10k switch driver -----------
 *
 * docs/OPEN-SOURCE-FOCALPOINT.md. DPDK issues a SerDes interrupt by writing the
 * SerDes device's OWN register 0x03 with (code << 16) | param, then polling
 * register 0x04 bits 16-17 until they clear. That is a different mechanism from
 * fm6000_sbus's `irq` subcommand, which drives the SPICO BROADCAST device across
 * regs 0x01/0x02/0x03/0x0c -- that is the firmware-load path, not this.
 *
 * ⚠ FM10000 IS THE SUCCESSOR PART. The codes below are its, not confirmed as the
 * FM6000's. Note in particular that FM6000's own fm6000EnableSerDes uses reg 0x03
 * as an ordinary control register (its step 11 sets bit 0), which does not sit
 * comfortably with reg 0x03 being the interrupt request register. Treat a failure
 * here as "the codes differ", not as "the mechanism is wrong".
 *
 * ⚠ Readback cannot confirm any of this; judge only by PORT_STATUS bit 11. */
static int spico_int(unsigned dev, unsigned code, uint32_t param, const char *what)
{
	int i;
	if (dry) { printf("  int  %02x arg %04x   (%s)\n", code, param, what); return 0; }
	if (sbus(OP_WRITE, dev, 0x03, ((uint32_t)code << 16) | (param & 0xffff)) < 0) {
		fprintf(stderr, "  int %02x: write failed\n", code); return 1;
	}
	for (i = 0; i < 3000; i++) {          /* DPDK timeout is 3000 ms */
		long v = sbus(OP_READ, dev, 0x04, 0);
		if (v < 0) return 1;
		if (!((uint32_t)v & (3u << 16))) {
			printf("  int  %02x arg %04x -> r04=%08lx after %d ms  (%s)\n",
			       code, param, v, i, what);
			return 0;
		}
		ms(1);
	}
	printf("  int  %02x arg %04x TIMEOUT   (%s)\n", code, param, what);
	return 1;
}

/* The bring-up order fm10k_epl_serdes_start_bringup() uses. tx eq comes from the
 * port table (port 3 is drive 4, pre 1, post 5); DPDK's own defaults are 0/0/15. */
static int irq_bringup(unsigned dev, int drive, int pre, int post)
{
	int bad = 0;
	bad |= spico_int(dev, 0x05, (0x42u & 0x7ff) | (1u << 12) | (1u << 15),
	                 "bit rate, 10G divider 0x42");
	bad |= spico_int(dev, 0x02, 0x1ff, "tx data select");
	bad |= spico_int(dev, 0x15, ((uint32_t)drive & 0xff) | (1u << 14), "tx eq attenuation");
	bad |= spico_int(dev, 0x15, ((uint32_t)pre   & 0xff) | (0u << 14), "tx eq precursor");
	bad |= spico_int(dev, 0x15, ((uint32_t)post  & 0xff) | (2u << 14), "tx eq postcursor");
	bad |= spico_int(dev, 0x11, 3, "PLL calibration");
	bad |= spico_int(dev, 0x2b, 1, "rx termination");
	bad |= spico_int(dev, 0x13, 0x0300, "polarity");
	bad |= spico_int(dev, 0x01, 0x03, "enable TX and RX");
	return bad;
}

/* ---- DFE: the RX equaliser ------------------------------------------------
 *
 * At 10 Gbps the channel smears each symbol into its successors. The decision
 * feedback equaliser subtracts the estimated ISI of already-decided bits from
 * the current sample, reopening the eye so the slicer can decide. Its taps are
 * specific to the physical channel, so they must be TRAINED -- and an untrained
 * DFE can leave the eye shut even with plenty of signal, which is exactly Et3:
 * RxSigStrength saturated, PLL locked, -2.76 dBm arriving, no block lock.
 *
 * From fm6000StartSerDesDfeTuning (0x4877a1): read reg 0x17, clear its low 5
 * bits, write back; then write 0x2a; then 0x2b. Registers confirmed by the
 * address setup at 0x487921-0x48796e, and they are the same three the EOS
 * capture writes.
 *
 * From fm6000CheckSerDesDfeTuningState (0x48d9e2): the state is read from
 * register 0x1f, fields extracted with `shr 4` and `shr 2`. Measured on
 * hardware, reg 0x1f bits 3:2 read 2 on BOTH working lanes and 1 on the dark
 * one -- so 2 is the converged state and that is the completion test.
 *
 * DPDK's open fm10k driver budgets 3000 ms for iCal, which is the timeout used
 * here. ⚠ The 0x2a/0x2b values are the capture's (0x0e, 0x02); the SDK computes
 * them, and that arithmetic is not decoded. */
static int dfe_tune(unsigned dev)
{
	long v;
	int i;

	if (dry) {
		printf("  dfe  rmw 0x17 &= ~0x1f; 0x2a <- 0e; 0x2b <- 02;"
		       " poll 0x1f b3:2 == 2\n");
		return 0;
	}
	v = sbus(OP_READ, dev, 0x17, 0);
	if (v < 0) return 1;
	if (sbus(OP_WRITE, dev, 0x17, (uint32_t)v & ~0x1fu) < 0) return 1;
	printf("  dfe  reg 17  %02lx -> %02lx   (clear low 5 bits)\n", v & 0xff,
	       (v & ~0x1fL) & 0xff);
	if (sbus(OP_WRITE, dev, 0x2a, 0x0e) < 0) return 1;
	if (sbus(OP_WRITE, dev, 0x2b, 0x02) < 0) return 1;
	printf("  dfe  reg 2a <- 0e, reg 2b <- 02   (start tuning)\n");

	for (i = 0; i < 3000; i++) {
		v = sbus(OP_READ, dev, 0x1f, 0);
		if (v < 0) return 1;
		if ((((uint32_t)v >> 2) & 3u) == 2u) {
			printf("  dfe  reg 1f -> %02lx, state=2 CONVERGED after %d ms\n",
			       v & 0xff, i);
			break;
		}
		ms(1);
	}
	if (i == 3000) {
		printf("  dfe  reg 1f -> %02lx, state=%lu TIMEOUT after 3000 ms\n",
		       v & 0xff, ((unsigned long)v >> 2) & 3);
		return 1;
	}

	/* ---- the iteration a capture cannot carry -------------------------
	 *
	 * EOS's captured bring-up does not stop after starting tuning. It then
	 * alternates reg 0x2a between 0x16 and 0x0e -- eleven times in the trace we
	 * have. A replay reproduces that count; what it cannot reproduce is the
	 * TERMINATION CONDITION, because a trace records the iterations that
	 * happened to suffice on a warm lane.
	 *
	 * Reg 0x2b looks like the progress counter: the capture WRITES it 0x02, a
	 * working lane ENDS at 0x04, and a single-shot start leaves ours at 0x03 --
	 * one step short. So toggle and poll 0x2b until it reaches the working
	 * lanes' value instead of counting to eleven. */
	for (i = 0; i < 256; i++) {
		long b = sbus(OP_READ, dev, 0x2b, 0);
		if (b < 0) return 1;
		if (((uint32_t)b & 0xff) == 0x04u) {
			printf("  dfe  reg 2b -> 04 after %d iterations\n", i);
			return 0;
		}
		if (sbus(OP_WRITE, dev, 0x2a, 0x16) < 0) return 1;
		if (sbus(OP_WRITE, dev, 0x2a, 0x0e) < 0) return 1;
		ms(2);
	}
	v = sbus(OP_READ, dev, 0x2b, 0);
	printf("  dfe  reg 2b stuck at %02lx after 256 iterations (want 04)\n",
	       v & 0xff);
	return 1;
}

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	int i, port = 0, dev = -1, epl = 0, lane = 0, bad = 0, irq = 0, dfeonly = 0;
	uint32_t base, ps;

	for (i = 1; i < argc; i++) {
		if      (!strcmp(argv[i], "-n")) dry = 1;
		else if (!strcmp(argv[i], "-i")) irq = 1;
		else if (!strcmp(argv[i], "-D")) dfeonly = 1;
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) bdf = argv[++i];
		else if (argv[i][0] != '-') port = atoi(argv[i]);
		else { fprintf(stderr, "usage: %s [-n] [-i] [-b bdf] <port>\n", argv[0]); return 2; }
	}
	for (i = 0; i < (int)(sizeof PORTS / sizeof PORTS[0]); i++)
		if (PORTS[i].port == port) { dev = PORTS[i].dev; epl = PORTS[i].epl;
		                             lane = PORTS[i].lane; }
	if (dev < 0) { fprintf(stderr, "usage: %s [-n] [-b bdf] <port 1-8>\n", argv[0]); return 2; }

	base = EPL_BASE + 0x400u * epl + 0x80u * lane;
	printf("port %d: EPL%d lane %d, SBus dev 0x%02x, PORT_STATUS at 0x%05x\n",
	       port, epl, lane, dev, base);

	if (!dry) {
		char path[256];
		int fd;
		snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
		fd = open(path, O_RDWR | O_SYNC);
		if (fd < 0) { perror("open resource0"); return 1; }
		M = mmap(NULL, 32u * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (M == MAP_FAILED) { perror("mmap"); return 1; }
		ps = rd(base);
		printf("  before: PORT_STATUS=%08x  SerXmit=%d\n", ps, !!(ps & (1u << 11)));
	}

	/* -D: DFE tuning ONLY.
	 *
	 * ⚠ EOS's boot sequence does NOT run fm6000EnableSerDes. Its captured
	 * bring-up for ports 1 and 2 writes only regs 0x01, 0x02, 0x17, 0x06, 0x2a,
	 * 0x2b (plus a reset) -- it never touches 0x0d, 0x22, 0x03, 0x1f, 0x26,
	 * 0x00, 0x1d, 0x36 or 0x3b. That is why both working lanes sit at
	 * 0x0d = a4 with bit 0 CLEAR while our enable drives it to a5: nothing ever
	 * set it on them. Running the full enable moves the lane into a state EOS
	 * never uses. So: let fm6000_lanelink replay the captured writes, and use
	 * this to supply the one thing a capture cannot carry -- the wait. */
	if (dfeonly) {
		bad = dfe_tune(dev);
		goto done;
	}

	if (irq) {
		/* tx eq from FM6000_SERDES_PORTS[]: port 3 is drive 4, pre 1, post 5 */
		bad = irq_bringup(dev, port == 3 ? 4 : 4, port == 3 ? 1 : 0,
		                  port == 3 ? 5 : 5);
		goto done;
	}

	/* step 1 */
	bad |= rmw(dev, 0x22, ~3u, 0, 0, "clear bits 0,1");
	/* step 2 KrTraining off -- NOT IMPLEMENTED (fm6000SetSerDesKrTraining) */
	printf("  SKIP step 2  KrTraining off        -- not decoded\n");
	/* steps 3-6 write computed values -- NOT IMPLEMENTED */
	printf("  SKIP steps 3-6 regs 00,1d,36,3b    -- values not decoded\n");
	/* step 7: XOR bit 4, not set -- from `xor eax,0x10` at 0x4824fe */
	bad |= rmw(dev, 0x17, ~0u, 0, 0x10u, "xor bit 4");
	/* step 8: the inverse of step 1 */
	bad |= rmw(dev, 0x22, ~0u, 3u, 0, "set bits 0,1");
	/* step 9 */
	bad |= wait_bits(dev, 0x0f, 0x9u, "PLL lock, b0+b3");
	/* steps 10-13 */
	bad |= rmw(dev, 0x06, ~0u, 0x8u, 0, "set bit 3");
	bad |= rmw(dev, 0x03, ~0u, 0x1u, 0, "set bit 0");
	bad |= rmw(dev, 0x1f, 0x3fu, 0, 0, "mask 0x3f");
	bad |= rmw(dev, 0x26, ~0u, 0x1u, 0, "set bit 0");
	/* step 14 SetTxConfig -- NOT IMPLEMENTED */
	printf("  SKIP step 14 SetTxConfig           -- not decoded\n");
	/* step 15 */
	bad |= rmw(dev, 0x0d, ~0u, 0x11u, 0, "set bits 4,0");
	/* step 16 */
	bad |= wait_bits(dev, 0x14, 0x40u, "signal detect, b6");
	/* steps 17-18: DFE tuning */
	bad |= dfe_tune(dev);

done:
	if (!dry) {
		ms(200);
		ps = rd(base);
		printf("  after : PORT_STATUS=%08x  SerXmit=%d  LANE_STATUS=%08x\n",
		       ps, !!(ps & (1u << 11)), rd(base + 0x38));
		printf("%s\n", (ps & (1u << 11)) ? "SerXmit SET -- lane is transmitting"
		                                 : "SerXmit clear -- lane still dark");
	}
	return bad ? 1 : 0;
}
