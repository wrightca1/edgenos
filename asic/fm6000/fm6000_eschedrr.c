/* fm6000_eschedrr.c - egress scheduler deficit-round-robin quanta.
 *
 * ESCHED_DRR_Q is twelve registers, one per traffic class, each holding a single
 * field: q[24], the DRR deficit quantum. Twelve numbers.
 *
 * The vendor replay spends 444 writes on them -- 37 per address -- and every one
 * of those writes lands on the same twelve registers. Read in order they are not
 * configuration but CONVERGENCE: EOS walks the quanta one traffic class at a
 * time, TC11 down to TC0 dropping each to 0x100, then back up raising each to
 * 0x1450, then a few final adjustments, and finally rewrites the settled state
 * thirteen more times.
 *
 *     round  1      all twelve 0xa00
 *     round  2      TC11 -> 0x7d0
 *     round  3      TC10 -> 0x3e8
 *     ...           one class changes per round
 *     round 25      TC11 -> 0x39d0        <- settled
 *     rounds 26-37  the settled state, rewritten unchanged
 *
 * Nothing observes the intermediate states: they are the by-product of a control
 * plane applying a queue configuration incrementally as it discovers ports. What
 * the chip needs is the end state, so that is what this writes -- twelve writes
 * instead of 444.
 *
 * ⚠ A DRR quantum controls how much each class may send per scheduling round.
 * Getting one wrong shows up as UNFAIRNESS UNDER LOAD, not as a failed ping, so a
 * transit test says nothing about whether these values are right. The check that
 * matters is tools/load-test.sh with traffic on several classes.
 *
 * ⚠ The quanta themselves are still the vendor's numbers. What is authored here
 * is the claim that only the settled state matters -- the 432 writes of
 * convergence are not.
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

#define ESCHED_DRR_Q  0x003000u   /* [12] w=1, one per traffic class */
#define NTC           12

/* Settled quantum per traffic class, in bytes of credit per round.
 * TC9/TC10 get five times TC3/TC5; the eight default classes sit in between. */
static const uint32_t Q[NTC] = {
	0x1450, 0x1450, 0x1450, 0x05c8, 0x1450, 0x05c8,
	0x1450, 0x1450, 0x1450, 0x6590, 0x6590, 0x39d0,
};

static volatile uint32_t *M;

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	int dry = 0, list = 0, i;

	for (i = 1; i < argc; i++) {
		if      (!strcmp(argv[i], "-n")) dry = 1;
		else if (!strcmp(argv[i], "-a")) list = 1;
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) bdf = argv[++i];
		else { fprintf(stderr, "usage: %s [-n|-a] [-b bdf]\n", argv[0]); return 2; }
	}

	if (!dry && !list) {
		char path[256];
		int fd;
		snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
		fd = open(path, O_RDWR | O_SYNC);
		if (fd < 0) { perror("open resource0"); return 1; }
		M = mmap(NULL, 32u * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (M == MAP_FAILED) { perror("mmap"); return 1; }
	}

	for (i = 0; i < NTC; i++) {
		uint32_t a = ESCHED_DRR_Q + i;
		if      (list) printf("%08x\n", a);
		else if (dry)  printf("%08x %08x\n", a, Q[i]);
		else         { M[a] = Q[i]; __sync_synchronize(); }
	}
	if (!dry && !list)
		fprintf(stderr, "fm6000_eschedrr: %d DRR quanta\n", NTC);
	return 0;
}
