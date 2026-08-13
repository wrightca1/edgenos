/* fm6000_sbus.c - issue single SBus transactions, and SPICO interrupts, by hand.
 *
 * fm6000_sbusdump reads and is deliberately kept read-only. This is its writing
 * counterpart, split out so that "dump a lane" stays a safe thing to run.
 *
 * It exists for one question. The replay never issues a single SBus op to the
 * SerDes of EPL14 lane 1 (Et3) -- 391 MMIO writes and nothing on the bus -- and
 * replaying lane 0's 44 SBus + 198 SPICO ops onto it does not bring the lane up
 * (docs/PORT3-BRINGUP.md). Two explanations remain: the SerDes bring-up is not
 * relocatable out of chip init, or the SPICO micro-controller simply never
 * answers for that lane, which would make all 198 interrupt-style ops no-ops.
 * The second is measurable and this tool measures it:
 *
 *     fm6000_sbus irq 0x49 0x20        # a lane whose port works
 *     fm6000_sbus irq 0x4a 0x20        # the dark lane -- same answer or not?
 *
 * The SPICO interrupt block is taken from EOS's own captures, where every one
 * looks like:
 *     write fd:0x01 = arg     write fd:0x02 = code    write fd:0x03 = target
 *     write fd:0x0c = 0x18    write fd:0x0c = 0x08    (strobe)
 *     read  fd:0x01           read  fd:0x00           read  fd:0x02
 * The broadcast device 0xfd never changes; reg 0x03 names the target, which is
 * why retargeting works by rewriting that payload rather than the device.
 *
 *   fm6000_sbus [-b <bdf>] read  <dev> <reg>
 *   fm6000_sbus [-b <bdf>] write <dev> <reg> <data>
 *   fm6000_sbus [-b <bdf>] irq   <target-dev> <code> [arg]
 *
 * ⚠ This writes to a live SerDes. It refuses to run if the chip is off-bus, and
 * it takes the device as an explicit argument -- there is no sweep mode and
 * there should not be one.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define PIN     0x1C021u
#define SB_CMD  0x0F001u
#define SB_REQ  0x0F002u
#define SB_RESP 0x0F003u
#define OP_READ  0x22u
#define OP_WRITE 0x21u
#define SPICO_BC 0xfdu

static volatile uint32_t *M;

/* The command register's own verdict on the last transaction. Bit 25 is Busy;
 * bits 28:26 are a result code that fm6000_lanelink has always treated as
 * "non-zero means the op failed" -- and which this tool used to discard. If the
 * bus is refusing writes to a device, this is where it says so. */
static uint32_t last_status;

static uint32_t rd(uint32_t w) { uint32_t v = M[w]; __sync_synchronize(); return v; }
static void     wr(uint32_t w, uint32_t v) { M[w] = v; __sync_synchronize(); }

/* One transaction. Returns the response word, or -1 on timeout / off-bus. */
static long sbus(unsigned op, unsigned dev, unsigned reg, uint32_t data)
{
	long i;

	last_status = 0xffffffffu;
	wr(SB_REQ, data);
	wr(SB_CMD, 0);
	wr(SB_CMD, (op << 16) | ((dev & 0xff) << 8) | (reg & 0xff) | (1u << 24));
	for (i = 0; i < 200000; i++) {
		uint32_t s = rd(SB_CMD);
		if (s == 0xffffffffu) return -1;
		if (!(s & (1u << 25))) { last_status = s; return (long)rd(SB_RESP); }
	}
	return -1;
}

static void show(const char *what, long v)
{
	if (v < 0) { printf("  %-24s TIMEOUT\n", what); return; }
	printf("  %-24s resp=%08lx  cmd=%08x  result=%u\n", what, (unsigned long)v,
	       last_status, (last_status >> 26) & 7);
}

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	char path[256];
	int fd, i, pos = 0;
	const char *cmd = NULL;
	unsigned a[3] = { 0, 0, 0 };
	int na = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-b") && i + 1 < argc) { bdf = argv[++i]; continue; }
		if (pos++ == 0) cmd = argv[i];
		else if (na < 3) a[na++] = (unsigned)strtoul(argv[i], NULL, 0);
	}
	if (!cmd || (!strcmp(cmd, "read")  && na < 2)
	         || (!strcmp(cmd, "write") && na < 3)
	         || (!strcmp(cmd, "irq")   && na < 2)) {
		fprintf(stderr,
		    "usage: fm6000_sbus [-b bdf] read  <dev> <reg>\n"
		    "       fm6000_sbus [-b bdf] write <dev> <reg> <data>\n"
		    "       fm6000_sbus [-b bdf] irq   <target-dev> <code> [arg]\n");
		return 2;
	}

	snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
	fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0) { perror("open"); return 1; }
	M = mmap(NULL, 32u*1024*1024, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (M == MAP_FAILED) { perror("mmap"); return 1; }
	if (rd(PIN) != 0x208u) {
		fprintf(stderr, "chip off-bus (PIN=%08x)\n", rd(PIN));
		return 1;
	}

	if (!strcmp(cmd, "read")) {
		show("read", sbus(OP_READ, a[0], a[1], 0));
	} else if (!strcmp(cmd, "write")) {
		show("write", sbus(OP_WRITE, a[0], a[1], a[2]));
		show("readback", sbus(OP_READ, a[0], a[1], 0));
	} else if (!strcmp(cmd, "irq")) {
		unsigned target = a[0], code = a[1], arg = a[2];

		printf("SPICO interrupt: target 0x%02x code 0x%02x arg 0x%02x\n",
		       target, code, arg);
		sbus(OP_WRITE, SPICO_BC, 0x01, arg);
		sbus(OP_WRITE, SPICO_BC, 0x02, code);
		sbus(OP_WRITE, SPICO_BC, 0x03, target);
		sbus(OP_WRITE, SPICO_BC, 0x0c, 0x18);
		sbus(OP_WRITE, SPICO_BC, 0x0c, 0x08);
		show("resp reg 0x01", sbus(OP_READ, SPICO_BC, 0x01, 0));
		show("resp reg 0x00", sbus(OP_READ, SPICO_BC, 0x00, 0));
		show("resp reg 0x02", sbus(OP_READ, SPICO_BC, 0x02, 0));
	} else {
		fprintf(stderr, "unknown command '%s'\n", cmd);
		return 2;
	}

	if (rd(PIN) != 0x208u) {
		fprintf(stderr, "WARNING chip went off-bus (PIN=%08x)\n", rd(PIN));
		return 1;
	}
	return 0;
}
