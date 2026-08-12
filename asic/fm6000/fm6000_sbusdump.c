/* fm6000_sbusdump.c - read SerDes core registers over the JSS SBus master.
 *
 * The EPL register block describes what the MAC/PCS is doing; it says nothing
 * about the SerDes core, which is where RX equalisation and lock actually live.
 * When port 3 refused to link with every EPL register provably identical to a
 * working lane, this is the only place left to look.
 *
 * SBus transaction (see fm6000_spico.c for the same protocol):
 *   0xF002 <- data, 0xF001 <- 0, 0xF001 <- cmd, poll Busy(bit25), 0xF003 = result
 *   cmd = Register[7:0] | Address[15:8] | Op[23:16] | Exec[24]
 *   op 0x22 = read, 0x21 = write
 *
 * Device addresses are per lane, +1 within an EPL: EPL14 lane0 = 0x49,
 * lane1 = 0x4a, EPL16 lane0 = 0x45 (all confirmed on hardware).
 *
 *   fm6000_sbusdump [-b <bdf>] <dev>[,<dev>...] [<lo> [<hi>]]
 *
 * Reads are side-effect-free on every register touched so far, but this is a
 * debug tool: it does not write, and it should stay that way.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define SB_CMD  0x0F001u
#define SB_REQ  0x0F002u
#define SB_RESP 0x0F003u
#define OP_READ 0x22u

static volatile uint32_t *M;

static uint32_t rd(uint32_t w) { uint32_t v = M[w]; __sync_synchronize(); return v; }
static void     wr(uint32_t w, uint32_t v) { M[w] = v; __sync_synchronize(); }

/* returns -1 on bus timeout / chip off-bus */
static long sbus_read(unsigned dev, unsigned reg)
{
	long i;
	wr(SB_REQ, 0);
	wr(SB_CMD, 0);
	wr(SB_CMD, (OP_READ << 16) | ((dev & 0xff) << 8) | (reg & 0xff) | (1u << 24));
	for (i = 0; i < 200000; i++) {
		uint32_t s = rd(SB_CMD);
		if (s == 0xffffffffu) return -1;
		if (!(s & (1u << 25))) return (long)rd(SB_RESP);
	}
	return -1;
}

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	unsigned devs[8]; int ndev = 0;
	unsigned lo = 0x00, hi = 0xff, reg;
	char path[256], *spec = NULL, *tok;
	int fd, i, pos = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-b") && i + 1 < argc) { bdf = argv[++i]; continue; }
		if (pos == 0) spec = argv[i];
		else if (pos == 1) lo = strtoul(argv[i], NULL, 0);
		else if (pos == 2) hi = strtoul(argv[i], NULL, 0);
		pos++;
	}
	if (!spec) {
		fprintf(stderr, "usage: fm6000_sbusdump [-b bdf] <dev>[,<dev>...] [lo [hi]]\n"
		                "  e.g. fm6000_sbusdump 0x4a,0x49 0x00 0xff\n");
		return 2;
	}
	for (tok = strtok(spec, ","); tok && ndev < 8; tok = strtok(NULL, ","))
		devs[ndev++] = strtoul(tok, NULL, 0);
	if (lo > 0xff || hi > 0xff || lo > hi) { fprintf(stderr, "bad range\n"); return 2; }

	snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
	fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0) { perror("open"); return 1; }
	M = mmap(NULL, 32u*1024*1024, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (M == MAP_FAILED) { perror("mmap"); return 1; }

	for (reg = lo; reg <= hi; reg++) {
		printf("%02x", reg);
		for (i = 0; i < ndev; i++) {
			long v = sbus_read(devs[i], reg);
			if (v < 0) printf(" --------"); else printf(" %08lx", (unsigned long)v);
		}
		putchar('\n');
	}
	return 0;
}
