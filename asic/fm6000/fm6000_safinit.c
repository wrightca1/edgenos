/* fm6000_safinit.c - program the SAF store-and-forward matrix ourselves.
 *
 * EOS builds this matrix incrementally: 34,668 register writes spread over 111
 * passes of its port loop, OR-ing one port's bit into the mask at a time. The
 * end state is four distinct 3-word patterns over 56 ports, so it can simply be
 * written -- 168 writes instead of 34,668.
 *
 * Cold-boot validated 2026-08-07 on a DCS-7150S-52: link 0x8c0 -> 0xcc0, OSPF
 * adjacency, 35 kernel routes, 13 programmed into silicon -- indistinguishable
 * from the recorded accumulation. See docs/SELF-CONTAINED-PLAN.md.
 *
 * Entry layout: SAF_ENTRY(port) = 0x0a0000 + 4*port, three words = a 96-bit
 * port mask. The port set comes from our own board table (fm6000_serdes_ports.h),
 * not from the trace.
 *
 * Used with fm6000-fullseq.sh, which filters the SAF writes out of the replay
 * and calls this instead.
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

#define MAXPORT    128
#define SAF_BASE   0x0a0000u
#define SAF_ENTRY(p) (SAF_BASE + 4u * (unsigned)(p))

/* The sweep visits front-panel 1-48, then the 53-56 uplink group, then two
 * internal ports. Front-panel 49-52 are not swept. */
static const int UPLINK_FP[] = { 53, 54, 55, 56 };
static const int INTERNAL[]  = { 3, 1 };

/* The four end-state patterns, by role. */
static const uint32_t PAT_PORT[3]     = { 0x0010000fu, 0x00000100u, 0x00000000u };
static const uint32_t PAT_EDGE[3]     = { 0x00000007u, 0x00000000u, 0x00000000u };
static const uint32_t PAT_ALL[3]      = { 0xffffffffu, 0xffffffffu, 0xffffffffu };
static const uint32_t PAT_CPU[3]      = { 0xffffffffu, 0xffffffffu, 0x0003ffffu };

/* Three swept ports carry PAT_EDGE rather than PAT_PORT. */
static int is_edge(int alta) { return alta == 3 || alta == 20 || alta == 40; }

static volatile uint32_t *M;
static int nwr, dry;

static void wr3(unsigned word, const uint32_t v[3])
{
	int i;
	for (i = 0; i < 3; i++) {
		if (dry) {
			printf("%08x %08x\n", word + i, v[i]);
		} else {
			M[word + i] = v[i];
			__sync_synchronize();
		}
		nwr++;
	}
}

static unsigned alta_of(int fp)
{
	size_t i;
	for (i = 0; i < sizeof FM6000_SERDES_PORTS / sizeof FM6000_SERDES_PORTS[0]; i++)
		if ((int)FM6000_SERDES_PORTS[i].intf == fp)
			return FM6000_SERDES_PORTS[i].alta;
	fprintf(stderr, "fm6000_safinit: no board entry for front-panel port %d\n", fp);
	exit(2);
}

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	int a;

	for (a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "-n"))   /* dry run: print the writes, touch nothing */
			dry = 1;
		else
			bdf = argv[a];
	}
	char p[256];
	int fd, fp;
	size_t i;
	const uint32_t *pat[MAXPORT];

	if (!dry) {
		snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/resource0", bdf);
		fd = open(p, O_RDWR | O_SYNC);
		if (fd < 0) { perror("open"); return 1; }
		M = mmap(NULL, 32u * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (M == MAP_FAILED) { perror("mmap"); return 1; }
	}

	/* Collect the matrix first, then emit in ASCENDING ADDRESS order -- that is
	 * the order the cold-boot-validated replay used, and ordering within the
	 * block has not been shown to be irrelevant. */
	memset(pat, 0, sizeof pat);
	for (fp = 1; fp <= 48; fp++)
		pat[alta_of(fp)] = is_edge((int)alta_of(fp)) ? PAT_EDGE : PAT_PORT;
	for (i = 0; i < sizeof UPLINK_FP / sizeof UPLINK_FP[0]; i++) {
		unsigned a = alta_of(UPLINK_FP[i]);
		pat[a] = is_edge((int)a) ? PAT_EDGE : PAT_PORT;
	}
	for (i = 0; i < sizeof INTERNAL / sizeof INTERNAL[0]; i++) {
		int a = INTERNAL[i];
		pat[a] = (a == 1) ? PAT_CPU : (is_edge(a) ? PAT_EDGE : PAT_PORT);
	}
	/* ports 0 and 2 are not swept but do carry an all-ports mask */
	pat[0] = PAT_ALL;
	pat[2] = PAT_ALL;

	for (i = 0; i < MAXPORT; i++)
		if (pat[i])
			wr3(SAF_ENTRY(i), pat[i]);

	if (!dry)
		printf("fm6000_safinit: %d SAF writes (replaces 34668 from the replay)\n", nwr);
	return 0;
}
