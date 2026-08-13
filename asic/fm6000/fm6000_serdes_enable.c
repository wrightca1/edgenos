/* fm6000_serdes_enable.c - bring a SerDes lane up by running EOS's algorithm,
 * not by replaying a capture of it.
 *
 * Every previous attempt on port 3 replayed captured register writes and failed
 * for a structural reason: fm6000EnableSerDes is read-modify-write plus two
 * blocking polls, and a trace records only the values that came out the far end.
 * It carries neither the read half of an RMW nor any notion of waiting. See
 * docs/EOS-SOURCES.md and docs/PORT3-BRINGUP.md.
 *
 * WHY THIS CAN WORK WHEN A REPLAY CANNOT
 *
 * The obstacle to expressing RMW was that a SerDes register's read view is
 * different silicon from its write view -- reading 0x22 returns rx_prbs_data,
 * not rx_en/tx_en -- so a register cannot be read back in order to modify it.
 *
 * The per-device SBus reset (op 0x20) removes that. After it, the write
 * registers are at their reset defaults, which is a KNOWN state, so the shadow
 * copy below can track them and every step writes a whole register built from
 * intent. The values the disassembly never yielded stop mattering: only the bits
 * each step sets or clears do, and those it did give.
 *
 * ⚠ The reset defaults are ASSUMED TO BE ZERO. Nothing has verified that. If a
 * lane misbehaves in a way that suggests a bit should have been set and was not,
 * this is the first assumption to doubt.
 *
 * THE SEQUENCE, from fm6000EnableSerDes @0x48131e
 *
 *   1  0x22  clear rx_en, tx_en            8  0x22  SET rx_en, tx_en
 *   2  --    KR training off  (not impl)   9  poll  0x0f b0 rx_rdy  == PLL lock
 *   3  ??    set b0, mask 0x3f (unknown)  10  0x06  set sig_strength_en
 *   4  0x1d  value unknown                11  0x03  set rx_data_gate
 *   5  0x36  value unknown                12  0x1f  mask 0x3f (unknown)
 *   6  0x3b  value unknown                13  0x26  set rx_dfe_gate
 *   7  0x17  set b4                       14  --    SetTxConfig (EPL side)
 *                                         15  0x0d  set tx_output_en, pre_emph
 *                                         16  poll  0x14 b6 signal detect
 *                                         17  DFE tuning (0x17/0x2a/0x2b)
 *
 * Steps 3, 4, 5, 6 and 12 are NOT implemented -- the disassembly gave their
 * registers but their values come from mode-dependent computation that has not
 * been decoded. Everything else is implemented from intent. That this is a
 * partial sequence is the point: it is honest about which steps are understood.
 *
 * ⚠ It also does not do step 17. DFE adapts a receiver that is already running;
 * get the lane transmitting first, then use fm6000_lanelink's DFE table, whose
 * ops are now known to target the right registers (0x17/0x2a/0x2b).
 *
 * JUDGE IT ON: PORT_STATUS bit 11 (SerXmit), 0 on a dark lane and 1 on both
 * working ports, and on the SPICO lane state (fm6000_sbus irq <dev> 0x20),
 * which reads 2 on a working lane, 1 on Et3 and 0 on an unused one.
 *
 *   fm6000_serdes_enable [-n] [-b <bdf>] <front-panel-port>
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

#define PIN      0x1C021u
#define SB_CMD   0x0F001u
#define SB_REQ   0x0F002u
#define SB_RESP  0x0F003u
#define EPL_BASE 0xE0000u

#define OP_RESET 0x20u
#define OP_WRITE 0x21u
#define OP_READ  0x22u

/* SerDes SBus registers, from fm6000_api_regs_int.h's SERDES_ETH_{WRITE,READ}_n */
#define R_RX_DATA_GATE  0x03u    /* WRITE b0  sbus_rx_data_gate                */
#define R_SIG_STRENGTH  0x06u    /* WRITE b3  sbus_rx_ib_sig_strength_en_cntl  */
#define R_TX_OUTPUT     0x0du    /* WRITE b4  tx_output_en, b0 pre_emphasis    */
#define R_RX_RDY        0x0fu    /* READ  b0  sbus_rx_rdy_obs                  */
#define R_SIG_OBS       0x14u    /* READ  b6  sbus_rx_ib_sig_strength_obs      */
#define R_ANALOG_GATE   0x17u    /* WRITE b4                                   */
#define R_ENABLES       0x22u    /* WRITE b0 rx_en, b1 tx_en                   */
#define R_DFE_GATE      0x26u    /* WRITE b0  sbus_rx_dfe_gate                 */
#define R_1D            0x1du    /* written 0 (immediate, step 4)              */
#define R_36            0x36u    /* [6:0] rate-dependent field (step 5)        */
#define R_3B            0x3bu    /* [6:0] rate-dependent field (step 6)        */

/* fm6000EnableSerDes selects these by line rate, from a pointer argument it
 * dereferences and compares (0x48192f onwards):
 *
 *     rate <= 1250   -> 0x13 / 0x63     1.25G
 *     rate <= 3125   -> 0x01 / 0x06     3.125G
 *     rate <= 6250   -> 0x01 / 0x09
 *     otherwise      -> 0x1b / 0x40     10.3125G   <-- every port on this box
 *
 * The second of each pair is the field written to 0x36 and 0x3b; the first goes
 * to the step-3 register, whose address comes from a local this analysis did not
 * resolve. Both are inserted as bitfields, value = read ^ ((read ^ new) & mask),
 * which is why the shadow matters.
 *
 * ⚠ Hard-coded for 10G. Every front-panel port here is 10.3125 Gbps, so this is
 * correct for this platform and wrong for a 1G SFP. */
#define RATE_FIELD      0x40u

static volatile uint32_t *M;
static int dry;
static uint8_t shadow[256];      /* our copy of the write registers */

static uint32_t rd(uint32_t w) { uint32_t v = M[w]; __sync_synchronize(); return v; }
static void     wr(uint32_t w, uint32_t v) { M[w] = v; __sync_synchronize(); }

static long sbus(unsigned op, unsigned dev, unsigned reg, uint32_t data)
{
	long i;

	if (dry) {
		printf("      SBUS op=%02x dev=%02x reg=%02x data=%02x\n", op, dev, reg, data);
		return 0;
	}
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

/* Read-modify-write against the shadow: set some bits, clear others, write the
 * whole register. This is the operation a capture cannot record. */
static int rmw(unsigned dev, unsigned reg, uint8_t set, uint8_t clr, const char *what)
{
	shadow[reg] = (uint8_t)((shadow[reg] & (uint8_t)~clr) | set);
	printf("    %-28s reg 0x%02x <- 0x%02x\n", what, reg, shadow[reg]);
	return sbus(OP_WRITE, dev, reg, shadow[reg]) < 0 ? -1 : 0;
}

/* Poll a READ-view bit, as fm6000WaitForSerDesPllLock / WaitForSignalDetection do. */
static int wait_bit(unsigned dev, unsigned reg, unsigned bit, const char *what)
{
	int i;

	if (dry) { printf("    wait %-23s reg 0x%02x bit %u\n", what, reg, bit); return 0; }
	for (i = 0; i < 200; i++) {
		long v = sbus(OP_READ, dev, reg, 0);
		if (v < 0) return -1;
		if ((v >> bit) & 1) {
			printf("    %-28s OK after %d ms (0x%02lx)\n", what, i * 5, v & 0xff);
			return 0;
		}
		usleep(5000);
	}
	printf("    %-28s TIMEOUT after 1s\n", what);
	return -1;
}

static const struct fm6000_serdes_port *find_port(unsigned intf)
{
	size_t i, n = sizeof FM6000_SERDES_PORTS / sizeof FM6000_SERDES_PORTS[0];
	for (i = 0; i < n; i++)
		if (FM6000_SERDES_PORTS[i].intf == intf) return &FM6000_SERDES_PORTS[i];
	return NULL;
}

/* Same mapping and same refusal as fm6000_lanelink: two observed points only. */
static int sbus_dev(unsigned epl, unsigned lane, int *observed)
{
	*observed = (epl == 14 && lane <= 1) || (epl == 16 && lane == 0);
	return 0x49 + (int)lane - 2 * ((int)epl - 14);
}

static void status(const char *when, uint32_t base, unsigned dev)
{
	uint32_t st;
	long rdy, sig;

	if (dry) return;
	st  = rd(base + 0x00);
	rdy = sbus(OP_READ, dev, R_RX_RDY, 0);
	sig = sbus(OP_READ, dev, R_SIG_OBS, 0);
	printf("  %-8s PORT_STATUS=%08x SerXmit=%u pcsRx=%08x  rx_rdy=%ld sig=%ld\n",
	       when, st, (st >> 11) & 1, rd(base + 0x26),
	       rdy < 0 ? -1 : (rdy & 1), sig < 0 ? -1 : ((sig >> 6) & 3));
}

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	char path[256];
	int fd, i, observed, dev;
	long intf = -1;
	const struct fm6000_serdes_port *p;
	uint32_t base;

	for (i = 1; i < argc; i++) {
		if      (!strcmp(argv[i], "-n")) dry = 1;
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) bdf = argv[++i];
		else if (argv[i][0] == '-') {
			fprintf(stderr, "usage: fm6000_serdes_enable [-n] [-b bdf] <port>\n");
			return 2;
		} else intf = strtol(argv[i], NULL, 0);
	}
	if (intf < 0 || !(p = find_port((unsigned)intf))) {
		fprintf(stderr, "unknown front-panel port %ld\n", intf); return 2;
	}

	base = EPL_BASE + 0x400u * p->epl + 0x80u * p->lane;
	dev  = sbus_dev(p->epl, p->lane, &observed);
	printf("port %u: EPL%u lane %u -> MMIO base 0x%05x, SBus dev 0x%02x%s\n",
	       p->intf, p->epl, p->lane, base, dev,
	       observed ? "" : "  (EXTRAPOLATED)");
	if (!observed && !dry) {
		fprintf(stderr, "  refusing an unobserved SBus mapping; use -n\n"); return 1;
	}

	if (!dry) {
		snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
		fd = open(path, O_RDWR | O_SYNC);
		if (fd < 0) { perror("open"); return 1; }
		M = mmap(NULL, 32u*1024*1024, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
		if (M == MAP_FAILED) { perror("mmap"); return 1; }
		if (rd(PIN) != 0x208u) {
			fprintf(stderr, "chip off-bus (PIN=%08x)\n", rd(PIN)); return 1;
		}
	}

	status("before", base, dev);

	/* The reset is what makes the shadow valid -- see the header. */
	printf("  reset device 0x%02x  (shadow <- reset defaults, assumed 0)\n", dev);
	if (sbus(OP_RESET, dev, 0x00, 0) < 0) { fprintf(stderr, "  reset failed\n"); return 1; }
	memset(shadow, 0, sizeof shadow);

	printf("  --- enable sequence ---\n");
	rmw(dev, R_ENABLES,      0x00, 0x03, "1  rx_en/tx_en off");
	/* 2 KR training off, and 3 (register unresolved, field 0x1b at 10G) */
	rmw(dev, R_1D,           0x00, 0xff, "4  reg 0x1d <- 0");
	rmw(dev, R_36,     RATE_FIELD, 0x7f, "5  divider field [6:0]");
	rmw(dev, R_3B,     RATE_FIELD, 0x7f, "6  divider field [6:0]");
	rmw(dev, R_ANALOG_GATE,  0x10, 0x1f, "7  analog gate [4:0]<-0x10");
	rmw(dev, R_ENABLES,      0x03, 0x00, "8  rx_en/tx_en ON");
	if (wait_bit(dev, R_RX_RDY, 0, "9  PLL lock (rx_rdy)") < 0)
		printf("    (continuing anyway -- the SDK would abort here)\n");
	rmw(dev, R_SIG_STRENGTH, 0x08, 0x00, "10 sig-strength detect on");
	rmw(dev, R_RX_DATA_GATE, 0x01, 0x00, "11 rx data gate");
	/* 12 0x1f mask 0x3f -- unknown */
	rmw(dev, R_DFE_GATE,     0x01, 0x00, "13 rx dfe gate");
	/* 14 SetTxConfig is EPL-side and already programmed */
	rmw(dev, R_TX_OUTPUT,    0x11, 0x00, "15 tx_output_en|pre_emph");
	if (wait_bit(dev, R_SIG_OBS, 6, "16 signal detect") < 0)
		printf("    (no signal detected -- nothing on the fibre, or RX not adapting)\n");

	status("after", base, dev);
	printf("  done -- 12 of 18 steps; 2 (KR training), 3 (register unresolved),"
	       " 12 (0x1f field), 17 (DFE) not implemented\n");
	return 0;
}
