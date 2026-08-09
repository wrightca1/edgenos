/* fm6000_l2scan.c - dump the non-zero entries of the FM6000 L2 MAC table.
 *
 * Diagnostic for the 1-in-6 alpha8 failure, where unicast egress stops while
 * broadcast and unicast ingress keep working -- the signature of the peer's MAC
 * going missing from the L2 forwarding table.
 *
 * L2L_MAC is 0x280000-0x300000 (262,144 words). Reading it is safe: probed
 * word-at-a-time with PIN checked after each, no off-bus. Several other ranges
 * on this chip DO off-bus when read, so do not generalise that.
 *
 *   fm6000_l2scan <BDF> [--mac aabbccddeeff]
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define LO 0x280000u
#define HI 0x300000u

int main(int argc, char **argv)
{
	const char *bdf = argc > 1 ? argv[1] : "0000:02:00.0";
	uint64_t want = 0;
	int i, nz = 0;
	char p[256];

	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--mac") && i + 1 < argc)
			want = strtoull(argv[i + 1], 0, 16);

	snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/resource0", bdf);
	int fd = open(p, O_RDONLY | O_SYNC);
	if (fd < 0) { perror("open"); return 1; }
	volatile uint32_t *M = mmap(NULL, 32u*1024*1024, PROT_READ, MAP_SHARED, fd, 0);
	if (M == MAP_FAILED) { perror("mmap"); return 1; }

	/* Bail out immediately if the chip is off-bus rather than printing 128K
	 * lines of 0xffffffff. */
	if (M[0x1c021] == 0xffffffffu) { fprintf(stderr, "chip off-bus\n"); return 2; }

	for (uint32_t a = LO; a < HI; a += 4) {
		uint32_t w0 = M[a], w1 = M[a+1], w2 = M[a+2], w3 = M[a+3];
		if (!(w0 | w1 | w2 | w3))
			continue;
		nz++;
		/* A MAC could be packed several ways; print the words and let the
		 * caller match rather than guessing a layout. */
		if (want) {
			uint64_t lo6 = ((uint64_t)w1 << 32) | w0;
			uint64_t alt = ((uint64_t)w0 << 32) | w1;
			if ((lo6 & 0xffffffffffffULL) == want ||
			    (alt & 0xffffffffffffULL) == want)
				printf("MATCH @%06x: %08x %08x %08x %08x\n", a, w0, w1, w2, w3);
		} else if (nz <= 40) {
			printf("%06x: %08x %08x %08x %08x\n", a, w0, w1, w2, w3);
		}
	}
	printf("non-zero 4-word entries: %d of %d\n", nz, (HI - LO) / 4);
	return 0;
}
