/* fm6000_parserseed.c - PARSER_INIT_FIELDS entry 0, the per-port parser seed.
 *
 * The parser starts every frame from a per-port seed held in PARSER_INIT_FIELDS,
 * two entries of two words per port. fm6000_parserfields already writes entry 1
 * -- zero for all 76 ports, disabling it -- and entry 0 for the 21 ports this
 * board does not use. Entry 0 for the 55 ports it DOES use came from the replay:
 * 970 writes, because the control plane rebuilds the seed each time it brings a
 * port up, converging on a final value per port.
 *
 * Read as an end state, entry 0 is two words and one rule:
 *
 *     word0 = glort << 16 | (front_panel ? 0x0100 | lane : 0)
 *     word1 = lane  << 16 | 0x0001
 *
 * Verified against the replay's settled values: **55 of 55 ports reproduce
 * exactly**, so the 970 writes carry 55 ports x 3 numbers of actual information.
 *
 *   glort  0x0001 for every port except the two configured links, which carry
 *          their own -- 0x03ee on port 20 (et2) and 0x03ef on port 40 (et1).
 *          Those match what portd assigns, and docs say port 20 = et2,
 *          port 40 = et1.
 *   lane   the board's port-to-SerDes-lane index. This is BOARD data, not vendor
 *          microcode: it describes how this switch is wired, and the same
 *          mapping is what /etc/prefdl's board description encodes. Deriving it
 *          from there at run time instead of holding it here would be better and
 *          is not done.
 *   ports 0, 1 and 3 are not front-panel and take 0 in word0's low half.
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

#define PARSER_INIT_FIELDS 0x108200u   /* [2 entries x 2 words] x [76 ports] */

/* { port, glort, lane, front_panel } */
static const uint16_t SEED[][4] = {
	{   0, 0x0001,   0, 0 },
	{   1, 0x0001,  54, 0 },
	{   3, 0x0001,  53, 0 },
	{  20, 0x03ee,   2, 1 },
	{  21, 0x0001,   4, 1 },
	{  22, 0x0001,   6, 1 },
	{  23, 0x0001,   8, 1 },
	{  24, 0x0001,  26, 1 },
	{  25, 0x0001,  28, 1 },
	{  26, 0x0001,  30, 1 },
	{  27, 0x0001,  32, 1 },
	{  28, 0x0001,  18, 1 },
	{  29, 0x0001,  20, 1 },
	{  30, 0x0001,  22, 1 },
	{  31, 0x0001,  24, 1 },
	{  32, 0x0001,  33, 1 },
	{  33, 0x0001,  35, 1 },
	{  34, 0x0001,  37, 1 },
	{  35, 0x0001,  39, 1 },
	{  36, 0x0001,   9, 1 },
	{  37, 0x0001,  11, 1 },
	{  38, 0x0001,  13, 1 },
	{  39, 0x0001,  15, 1 },
	{  40, 0x03ef,   1, 1 },
	{  41, 0x0001,   3, 1 },
	{  42, 0x0001,   5, 1 },
	{  43, 0x0001,   7, 1 },
	{  44, 0x0001,  49, 1 },
	{  45, 0x0001,  50, 1 },
	{  46, 0x0001,  51, 1 },
	{  47, 0x0001,  52, 1 },
	{  52, 0x0001,  41, 1 },
	{  53, 0x0001,  43, 1 },
	{  54, 0x0001,  45, 1 },
	{  55, 0x0001,  47, 1 },
	{  56, 0x0001,  42, 1 },
	{  57, 0x0001,  44, 1 },
	{  58, 0x0001,  46, 1 },
	{  59, 0x0001,  48, 1 },
	{  60, 0x0001,  34, 1 },
	{  61, 0x0001,  36, 1 },
	{  62, 0x0001,  38, 1 },
	{  63, 0x0001,  40, 1 },
	{  64, 0x0001,  10, 1 },
	{  65, 0x0001,  12, 1 },
	{  66, 0x0001,  14, 1 },
	{  67, 0x0001,  16, 1 },
	{  68, 0x0001,  25, 1 },
	{  69, 0x0001,  27, 1 },
	{  70, 0x0001,  29, 1 },
	{  71, 0x0001,  31, 1 },
	{  72, 0x0001,  17, 1 },
	{  73, 0x0001,  19, 1 },
	{  74, 0x0001,  21, 1 },
	{  75, 0x0001,  23, 1 },
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

	for (i = 0; i < (int)(sizeof SEED / sizeof SEED[0]); i++) {
		uint32_t port = SEED[i][0], glort = SEED[i][1];
		uint32_t lane = SEED[i][2], fp = SEED[i][3];
		uint32_t a = PARSER_INIT_FIELDS + port * 4;
		uint32_t w0 = (glort << 16) | (fp ? (0x0100u | lane) : 0u);
		uint32_t w1 = (lane << 16) | 1u;

		if (list)      printf("%08x\n%08x\n", a, a + 1);
		else if (dry)  printf("%08x %08x\n%08x %08x\n", a, w0, a + 1, w1);
		else { M[a] = w0; M[a + 1] = w1; __sync_synchronize(); }
	}
	if (!dry && !list)
		fprintf(stderr, "fm6000_parserseed: %d ports seeded\n",
		        (int)(sizeof SEED / sizeof SEED[0]));
	return 0;
}
