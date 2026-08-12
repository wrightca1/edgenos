/* fm6000_bst.c - read and rewrite the FM6000 hardware FIB (the FFU BST).
 *
 * The FIB is interval-based LPM held in FFU_BST_KEY: sorted ascending IPv4
 * boundaries, right-aligned against i = 1023, each paired 1:1 with an entry in
 * FFU_BST_ACTION_ROUTE. Entry i owns the range [key[i], key[i+1]).
 *
 * Updates are DOUBLE-BUFFERED. Each engine has an active/standby pair of 1024-
 * entry blocks; FFU_BST_ROOT_KEYS[engine][15].Partition[43:40] names the live
 * one. EOS rebuilds the standby in full and then flips that one field, so the
 * live block is never written under traffic. Verified on hardware by adding a
 * route and watching partition 14 -> 15 with block 14 untouched, then back on
 * removal. See docs/ROUTED-PORT-ANATOMY.md.
 *
 * Addressing (register header):
 *   FFU_BST_KEY(e,b,i)          = 0x10000*e + 0x400*b +   i + 0x308000
 *   FFU_BST_ACTION_ROUTE(e,b,i,w)= 0x10000*e + 0x800*b + 2*i + 0x300000 + w
 *   FFU_BST_ROOT_KEYS(e,r,w)    = 0x10000*e + 2*r + 0x30c080 + w
 *
 * ACTION fields:
 *   NextHopBaseIndex[15:0] NextHopRange[22:16] NextHopEntryType[23] LPM[31:24]
 *   TagData[43:32] TagCmd[45:44] Route[46] Precedence[49:47]
 *
 * *** SCOPE -- read this before extending it ***
 * This tool implements the COMMIT MECHANISM, not route synthesis. `-c` copies
 * the live block to the standby verbatim and flips the partition: the table is
 * unchanged, so forwarding must be unaffected, which makes it a safe end-to-end
 * test of exactly the write path a real FIB needs.
 *
 * It deliberately does NOT synthesise new actions. The LPM[31:24] byte is not
 * understood: it is 0x00 on connected/local entries (nexthop 5 and 7) and 0x08
 * on OSPF-learned ones (nexthop 16) -- both /24s, so it is not a prefix length.
 * Guessing it would put wrong actions in the forwarding path. Decode it first.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define FFU_BASE      0x300000u
#define BST_ENTRIES   1024
#define BST_ENGINES   4
#define ROOT_SLOT     15          /* the only populated root slot observed */

#define BST_KEY(e, b, i)       (FFU_BASE + 0x8000u + 0x10000u*(e) + 0x400u*(b) + (i))
#define BST_ACT(e, b, i, w)    (FFU_BASE + 0x10000u*(e) + 0x800u*(b) + 2u*(i) + (w))
#define BST_ROOT(e, r, w)      (FFU_BASE + 0xc080u + 0x10000u*(e) + 2u*(r) + (w))

/* Partition[43:40] lives in root word 1, bits 11:8 */
#define ROOT_PART_SHIFT 8
#define ROOT_PART_MASK  0xfu

static volatile uint32_t *M;
static int dry;

static uint32_t rd(uint32_t a) { return M[a]; }
static void wr(uint32_t a, uint32_t v)
{
	if (dry) { printf("    %08x <- %08x\n", a, v); return; }
	M[a] = v; __sync_synchronize();
}

static const char *ip4(uint32_t v, char *b)
{
	sprintf(b, "%u.%u.%u.%u", v >> 24, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
	return b;
}

static unsigned live_partition(unsigned e)
{
	return (rd(BST_ROOT(e, ROOT_SLOT, 1)) >> ROOT_PART_SHIFT) & ROOT_PART_MASK;
}

static void dump(unsigned e)
{
	unsigned p = live_partition(e), i, n = 0;
	char b[20];

	printf("engine %u: root=%08x %08x  live partition=%u\n",
	       e, rd(BST_ROOT(e, ROOT_SLOT, 0)), rd(BST_ROOT(e, ROOT_SLOT, 1)), p);
	for (i = 0; i < BST_ENTRIES; i++) {
		uint32_t k = rd(BST_KEY(e, p, i));
		uint32_t a0, a1;
		if (!k) continue;
		a0 = rd(BST_ACT(e, p, i, 0));
		a1 = rd(BST_ACT(e, p, i, 1));
		printf("  [%4u] %-16s nh=%-5u range=%-3u lpm=0x%02x route=%u prec=%u  (%08x %08x)\n",
		       i, ip4(k, b),
		       a0 & 0xffff, (a0 >> 16) & 0x7f, (a0 >> 24) & 0xff,
		       (a1 >> 14) & 1, (a1 >> 15) & 7, a0, a1);
		n++;
	}
	printf("  %u live boundaries\n", n);
}

/* Copy the live block to its standby twin verbatim, then publish the twin.
 * The table content is identical, so this changes no forwarding decision --
 * it exercises the rebuild-and-flip path and nothing else. */
static int copy_and_flip(unsigned e, int verify)
{
	/* ASSUMPTION: the standby twin is p^1. Only the pair (14,15) has ever
	 * been observed on this box, in both directions, so "even/odd twin" fits
	 * every data point we have -- but two data points are not a rule. If a
	 * live partition outside {14,15} ever shows up, check this before
	 * trusting it. */
	unsigned p = live_partition(e), q = p ^ 1u, i, bad = 0, n = 0;
	uint32_t r1;

	printf("engine %u: live partition %u -> rebuilding standby %u\n", e, p, q);

	for (i = 0; i < BST_ENTRIES; i++) {
		uint32_t k  = rd(BST_KEY(e, p, i));
		uint32_t a0 = rd(BST_ACT(e, p, i, 0));
		uint32_t a1 = rd(BST_ACT(e, p, i, 1));
		wr(BST_KEY(e, q, i), k);
		wr(BST_ACT(e, q, i, 0), a0);
		wr(BST_ACT(e, q, i, 1), a1);
		if (k) n++;
	}
	printf("  wrote %d entries (%u live boundaries)\n", BST_ENTRIES, n);

	if (verify && !dry) {
		for (i = 0; i < BST_ENTRIES; i++) {
			if (rd(BST_KEY(e, q, i))    != rd(BST_KEY(e, p, i))    ||
			    rd(BST_ACT(e, q, i, 0)) != rd(BST_ACT(e, p, i, 0)) ||
			    rd(BST_ACT(e, q, i, 1)) != rd(BST_ACT(e, p, i, 1))) {
				printf("  VERIFY FAIL at [%u]\n", i);
				if (++bad > 8) break;
			}
		}
		if (bad) { printf("  VERIFY FAIL -- NOT flipping partition\n"); return 1; }
		printf("  VERIFY PASS (%d entries compared)\n", BST_ENTRIES);
	}

	/* publish: flip Partition, leave every other field of the root alone */
	r1 = rd(BST_ROOT(e, ROOT_SLOT, 1));
	r1 = (r1 & ~(ROOT_PART_MASK << ROOT_PART_SHIFT)) | (q << ROOT_PART_SHIFT);
	printf("  publish: root w1 %08x -> %08x (partition %u -> %u)\n",
	       rd(BST_ROOT(e, ROOT_SLOT, 1)), r1, p, q);
	wr(BST_ROOT(e, ROOT_SLOT, 1), r1);

	if (!dry && live_partition(e) != q) {
		printf("  FAIL: partition did not take (still %u)\n", live_partition(e));
		return 1;
	}
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: fm6000_bst [-b <bdf>] [-e <engine>] [-n] {-d | -c}\n"
		"  -d  dump the live block (keys + decoded actions)\n"
		"  -c  copy live -> standby verbatim and flip the partition\n"
		"      (content-identical: exercises the commit path, changes no route)\n"
		"  -e  engine 0..3 (default 3, the one holding the IPv4 unicast table)\n"
		"  -n  dry run\n");
}

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	char p[256];
	int fd, i, mode = 0;
	unsigned e = 3;

	for (i = 1; i < argc; i++) {
		if      (!strcmp(argv[i], "-n")) dry = 1;
		else if (!strcmp(argv[i], "-d")) mode = 'd';
		else if (!strcmp(argv[i], "-c")) mode = 'c';
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) bdf = argv[++i];
		else if (!strcmp(argv[i], "-e") && i + 1 < argc) e = strtoul(argv[++i], NULL, 0);
		else { usage(); return 2; }
	}
	if (!mode || e >= BST_ENGINES) { usage(); return 2; }

	snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/resource0", bdf);
	fd = open(p, O_RDWR | O_SYNC);
	if (fd < 0) { perror("open"); return 1; }
	M = mmap(NULL, 32u*1024*1024, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (M == MAP_FAILED) { perror("mmap"); return 1; }

	if (mode == 'd') { dump(e); return 0; }
	return copy_and_flip(e, 1);
}
