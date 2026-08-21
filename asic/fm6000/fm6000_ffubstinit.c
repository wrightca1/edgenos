/* fm6000_ffubstinit.c - initialise the FFU BST_ACTION default entries.
 *
 * 63% of the FFU writes still replayed from fwd4.txt are the BST -- the route
 * table -- and 8,144 of them are not a program at all, they are a default fill.
 * Measured on the executed replay: 8,144 writes carrying exactly two values,
 * 0x00700000 on every EVEN word and 0x00000000 on every ODD word, 8144 of 8144
 * with no exceptions, across four contiguous runs.
 *
 *   BST_ACTION(grp, sub, entry, w) = 0x300000 + 0x10000*grp + 0x800*sub + 2*entry
 *
 *   0x327002 - 0x3277f9   1020 entries   grp 2 sub 14
 *   0x327802 - 0x327ffb   1021 entries   grp 2 sub 15
 *   0x337002 - 0x3377ed   1014 entries   grp 3 sub 14
 *   0x337802 - 0x337ff3   1017 entries   grp 3 sub 15
 *
 * An entry is two words; the pair (0x00700000, 0x00000000) is the default/empty
 * action. Writing a table's default value over its own address range is a memset,
 * not Intel's program -- which is why this one is worth taking: it serves BOTH
 * goals in docs/BLOB-REMOVAL-PLAN.md, where merely re-encoding EOS's rules would
 * remove the file while moving their program into our source.
 *
 * ⚠ SCOPE. This deliberately generates ONLY the default-fill addresses, not the
 * whole sub-block. The remaining ~200 BST_ACTION writes carry real route content
 * (19 distinct values in the block, of which these are two) and stay in the
 * replay: filling ranges EOS did not fill risks overwriting content whose write
 * order relative to ours is not established. Widening this needs that ordering
 * settled first.
 *
 * usage: fm6000_ffubstinit [-n | -a] [-b bdf]
 *   -n  dry run: print "ADDR VALUE" pairs for splicing into the replay
 *   -a  print only the addresses, for the replay filter
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

#define BST_DEFAULT_HI 0x00700000u      /* even word of an empty BST action */
#define BST_DEFAULT_LO 0x00000000u      /* odd  word */

/* ⚠ These runs are TRIMMED, and the trim is the whole point. FULLSEQ's gen_list
 * filters the replay by ADDRESS, not by (address,value). 98 addresses inside the
 * measured default-fill ranges ALSO receive a real route-content write later
 * (0x8000006, 0x14000, 0x3000008, ...). Generating those addresses would make
 * gen_list strip their content writes too -- 8,276 lines removed against 8,144
 * replaced -- and the routes would come up as empty entries.
 *
 * Caught by simulating gen_list offline before building an image. The runs below
 * are restricted to addresses that receive ONLY the two default values, so the
 * address filter removes exactly what this tool puts back. */
static const struct { uint32_t first, last; } RUN[] = {
	{ 0x327002, 0x3277f9 },
	{ 0x327802, 0x327ff7 },
	{ 0x337002, 0x3377e1 },
	{ 0x337802, 0x337fa1 },
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
		snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
		int fd = open(path, O_RDWR | O_SYNC);
		if (fd < 0) { perror("open resource0"); return 1; }
		M = mmap(NULL, 32u * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (M == MAP_FAILED) { perror("mmap"); return 1; }
	}

	long n = 0;
	for (i = 0; i < (int)(sizeof RUN / sizeof RUN[0]); i++) {
		uint32_t a;
		for (a = RUN[i].first; a <= RUN[i].last; a++) {
			/* the value is chosen by the address's parity, exactly as measured */
			uint32_t v = (a & 1u) ? BST_DEFAULT_LO : BST_DEFAULT_HI;
			if (list)      printf("%08x\n", a);
			else if (dry)  printf("%08x %08x\n", a, v);
			else         { M[a] = v; __sync_synchronize(); }
			n++;
		}
	}
	if (!dry && !list)
		fprintf(stderr, "fm6000_ffubstinit: %ld writes\n", n);
	return 0;
}
