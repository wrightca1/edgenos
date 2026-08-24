/* fm6000_slice.c - apply a contiguous SLICE of another generator's output.
 *
 * WHY
 *
 * A generator's writes do not always land in the bring-up as one block. The
 * clearest case is fm6000_l2arseq: the sequence splices its first 25,426 writes
 * before the port loop and leaves the remaining 3,684 -- the in-loop bursts --
 * at their original positions inside it. build_schedule.py can only place a
 * generator it finds contiguously, so those 3,684 stayed in the residual as
 * vendor data even though our own generator already produces them, byte for
 * byte, at l2arseq[25426..29109].
 *
 * Rather than teach thirty generators a new flag, this runs any of them in
 * dry-run mode and applies one slice of the result. The values are still
 * computed by our code at run time; only the delivery window changes.
 *
 *   fm6000_slice <tool> <first> <count> [-b <bdf>] [-n]
 *
 * ⚠ The child is run with -n, so it must not touch the chip. Every generator in
 * this tree honours that; anything that does not must never be sliced.
 *
 * ⚠ This does NOT interpret SBus. fm6000_fullreplay stashes 0xF002, skips a zero
 * 0xF001 and drives a real transaction with a completion poll, and a slice that
 * replayed those addresses literally would corrupt the bus -- that is exactly
 * what took Et2 down 5 boots out of 5 before fm6000_sbusseq grew sbus_wait().
 * So slicing refuses any window containing an SBus address; use the block's own
 * generator, which knows the handshake.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

#define SBUS_LO 0x00f000u
#define SBUS_HI 0x00f00fu

static volatile uint32_t *M;

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0", *tool = NULL;
	long first = -1, count = -1;
	int dry = 0, i, fd, n = 0, applied = 0;
	char cmd[512], line[128];
	FILE *pp;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n")) dry = 1;
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) bdf = argv[++i];
		else if (!tool) tool = argv[i];
		else if (first < 0) first = strtol(argv[i], NULL, 0);
		else if (count < 0) count = strtol(argv[i], NULL, 0);
	}
	if (!tool || first < 0 || count < 0) {
		fprintf(stderr, "usage: %s <tool> <first> <count> [-b bdf] [-n]\n", argv[0]);
		return 2;
	}

	if (!dry) {
		char path[256];
		snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
		fd = open(path, O_RDWR | O_SYNC);
		if (fd < 0) { perror("open resource0"); return 1; }
		M = mmap(NULL, 32u * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (M == MAP_FAILED) { perror("mmap"); return 1; }
	}

	snprintf(cmd, sizeof cmd, "%s%s -n 2>/dev/null",
	         strchr(tool, '/') ? "" : "/usr/bin/", tool);
	pp = popen(cmd, "r");
	if (!pp) { perror("popen"); return 1; }

	while (fgets(line, sizeof line, pp)) {
		uint32_t a, v;
		if (sscanf(line, "%x %x", &a, &v) != 2) continue;
		if (n >= first && n < first + count) {
			if (a >= SBUS_LO && a <= SBUS_HI) {
				fprintf(stderr, "fm6000_slice: %s[%ld..%ld] contains SBus address "
				        "%06x -- refusing, use the block's own generator\n",
				        tool, first, first + count - 1, a);
				pclose(pp);
				return 1;
			}
			if (dry) printf("%08x %08x\n", a, v);
			else { M[a] = v; __sync_synchronize(); }
			applied++;
		}
		n++;
	}
	pclose(pp);

	if (applied != count) {
		fprintf(stderr, "fm6000_slice: %s produced %d writes, wanted [%ld..%ld] "
		        "and got %d -- NOT applying a partial slice\n",
		        tool, n, first, first + count - 1, applied);
		return 1;
	}
	if (!dry) fprintf(stderr, "fm6000_slice: %s[%ld..%ld] %d writes\n",
	                  tool, first, first + count - 1, applied);
	return 0;
}
