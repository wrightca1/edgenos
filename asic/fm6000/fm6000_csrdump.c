/* fm6000_csrdump.c - snapshot a list of FM6000 CSRs, for differential capture.
 *
 * ★ WHY. A standalone boot (no vendor replay) brings both ports to clean lock
 * and forwards nothing -- et1 rx=0, no packets punted to the CPU. Bisecting the
 * residual to find "the missing writes" is void, because applying the COMPLETE
 * residual fails too. The question that is actually answerable is:
 *
 *     does the chip's END STATE differ between a working boot and a standalone
 *     one, and if so where?
 *
 * If it differs, the diff names the missing state by address and the fix is
 * ordinary work. If it does NOT differ, then nothing is missing statically and
 * the difference is dynamic -- edge-triggered, or a strobe that had to fire
 * while other state held -- which is a much harder result but stops us hunting
 * for content that does not exist.
 *
 * ⚠ NEVER SWEEP THE ADDRESS SPACE BLIND. A blind register sweep wedged the
 * 7050TX-64 (see the diag-getreg note). This reads only an explicit address
 * list, generated from the SDK's own register descriptor table -- 677 known
 * registers expanded through their geometry, with the bulk tables (L2L MAC
 * table, NEXTHOP, ...) excluded because they are content, not configuration.
 *
 * ⚠ Some of what it reads is inherently volatile -- counters, interrupt-pending
 * bits, link status. Those WILL differ between any two boots and are noise in
 * the diff, not signal. Take two snapshots of the SAME boot to measure that
 * noise floor before trusting a cross-boot diff.
 *
 * usage: fm6000_csrdump [-b bdf] <addr-list-file>
 *        prints "AAAAAAAA VVVVVVVV" per line, in list order
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0", *list = NULL;
	char path[256], line[64];
	volatile uint32_t *M;
	FILE *f;
	int i, fd, n = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-b") && i + 1 < argc) bdf = argv[++i];
		else if (argv[i][0] != '-') list = argv[i];
		else { fprintf(stderr, "usage: %s [-b bdf] <addr-list>\n", argv[0]); return 2; }
	}
	if (!list) { fprintf(stderr, "usage: %s [-b bdf] <addr-list>\n", argv[0]); return 2; }

	f = fopen(list, "r");
	if (!f) { perror(list); return 1; }

	snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
	fd = open(path, O_RDONLY | O_SYNC);
	if (fd < 0) { perror("open resource0"); return 1; }
	M = mmap(NULL, 32u * 1024 * 1024, PROT_READ, MAP_SHARED, fd, 0);
	if (M == MAP_FAILED) { perror("mmap"); return 1; }

	while (fgets(line, sizeof line, f)) {
		unsigned long a = strtoul(line, NULL, 16);
		if (a >= 8u * 1024 * 1024) continue;      /* 32MB BAR / 4 bytes */
		printf("%08lx %08x\n", a, M[a]);
		n++;
	}
	fclose(f);
	fprintf(stderr, "fm6000_csrdump: %d registers\n", n);
	return 0;
}
