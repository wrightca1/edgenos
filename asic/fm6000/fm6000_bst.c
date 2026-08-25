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
 * removal. See docs/EDGENOS-7150.md (was ROUTED-PORT-ANATOMY).
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
 * LPM[31:24] is 32 - prefix length -- the host-bit count of the route that owns
 * the interval (/32 -> 0, /29 -> 3, /24 -> 8, /22 -> 10). Confirmed against all
 * 49 boundaries of a live EOS table. An interval can run past the end of its own
 * prefix, so the hardware rechecks containment; that is what the field is for.
 *
 * Precedence is 2 for ordinary entries and 3 for the box's own addresses.
 * So: a0 = (lpm << 24) | nexthop, a1 = (prec << 15) | (1 << 14) [Route].
 *
 * Modes:
 *   -d  dump the live block
 *   -c  copy live -> standby verbatim and flip (content-identical; exercises
 *       the commit path without changing a single forwarding decision)
 *   -p  build a block from a boundary list and publish it
 *
 * *** SCOPE ***
 * -p writes the table it is given. Turning a kernel RIB into that boundary list
 * -- allocating NEXTHOP entries, emitting glean/local/broadcast boundaries per
 * connected subnet -- is policy and belongs in fm6000_fibd. This is the
 * mechanism underneath it.
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

static uint32_t rd(uint32_t a) { return M ? M[a] : 0; }
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

/* ---- building a block from a boundary list --------------------------------
 *
 * Input is one boundary per line, which is what the hardware actually stores:
 *
 *     <prefix>/<len> <nexthop> <precedence>
 *
 * lpm is derived as 32-len, so a boundary is written with the prefix length of
 * the route that owns it. A connected subnet contributes /32 boundaries for its
 * network address, its broadcast and each local/resolved host -- that is what
 * EOS emits, and it is why 10.101.101.24 (a /29 connected) carries lpm 0.
 *
 * 0.0.0.0/0 is special: it is not a boundary. Its action goes in the slot
 * immediately BELOW the lowest boundary, whose key is 0, because a binary
 * search for anything under the first boundary lands there.
 *
 * The whole block is written -- all 1024 slots, zeros included. Writing only
 * the populated entries is what leaves a right-aligned array holding the tail
 * of an older, larger generation, which is exactly the corruption measured on
 * EdgeNOS (10.101.101.32 followed by 10.3.1.0). */

#define ACT_EMPTY_W0 0x00700000u   /* what EOS leaves in never-searched slots */

struct bnd { uint32_t key; uint8_t len, prec; uint16_t nh; };

static int bnd_cmp(const void *a, const void *b)
{
	uint32_t x = ((const struct bnd *)a)->key, y = ((const struct bnd *)b)->key;
	return x < y ? -1 : x > y ? 1 : 0;
}

static uint32_t act_w0(const struct bnd *e) { return ((uint32_t)(32 - e->len) << 24) | e->nh; }
static uint32_t act_w1(const struct bnd *e) { return ((uint32_t)e->prec << 15) | (1u << 14); }

/* returns count, or -1; *dflt receives the 0.0.0.0/0 action if present */
static int load_boundaries(const char *path, struct bnd *o, int max, struct bnd *dflt, int *has_d)
{
	char line[256];
	FILE *f = fopen(path, "r");
	int n = 0;
	if (!f) { perror(path); return -1; }
	*has_d = 0;
	while (fgets(line, sizeof line, f)) {
		unsigned a, b, c, d, len, nh, prec;
		if (line[0] == '#' || line[0] == '\n') continue;
		if (sscanf(line, "%u.%u.%u.%u/%u %u %u", &a, &b, &c, &d, &len, &nh, &prec) != 7) {
			fprintf(stderr, "bad line: %s", line); fclose(f); return -1;
		}
		if (len > 32 || a > 255 || b > 255 || c > 255 || d > 255) {
			fprintf(stderr, "out of range: %s", line); fclose(f); return -1;
		}
		struct bnd e = { (a << 24) | (b << 16) | (c << 8) | d,
				 (uint8_t)len, (uint8_t)prec, (uint16_t)nh };
		if (len == 0 && e.key == 0) { *dflt = e; *has_d = 1; continue; }
		if (n >= max) { fprintf(stderr, "too many boundaries (max %d)\n", max); fclose(f); return -1; }
		o[n++] = e;
	}
	fclose(f);
	qsort(o, n, sizeof *o, bnd_cmp);
	return n;
}

static int program(unsigned e, const char *path)
{
	static struct bnd b[BST_ENTRIES];
	struct bnd dflt; int has_d, n, i;
	unsigned p = live_partition(e), q = p ^ 1u, base;
	uint32_t r1;
	char s[20];

	n = load_boundaries(path, b, BST_ENTRIES - 1, &dflt, &has_d);
	if (n < 0) return 1;
	base = BST_ENTRIES - n;               /* first boundary index */
	if (has_d && base == 0) { fprintf(stderr, "no room for the default slot\n"); return 1; }

	printf("engine %u: %d boundaries at [%u..%u]%s, live partition %u -> writing %u\n",
	       e, n, base, BST_ENTRIES - 1,
	       has_d ? ", default in the slot below" : "", p, q);

	for (i = 0; i < BST_ENTRIES; i++) {
		uint32_t k = 0, a0 = ACT_EMPTY_W0, a1 = 0;
		if (i >= (int)base) {
			const struct bnd *x = &b[i - base];
			k = x->key; a0 = act_w0(x); a1 = act_w1(x);
		} else if (has_d && i == (int)base - 1) {
			a0 = act_w0(&dflt); a1 = act_w1(&dflt);   /* key stays 0 */
		}
		wr(BST_KEY(e, q, i), k);
		wr(BST_ACT(e, q, i, 0), a0);
		wr(BST_ACT(e, q, i, 1), a1);
		if (dry && (k || i == (int)base - 1))
			printf("    [%4d] %-16s nh=%-5u lpm=%-3u prec=%u\n", i,
			       k ? ip4(k, s) : "(default)",
			       a0 & 0xffff, (a0 >> 24) & 0xff, (a1 >> 15) & 7);
	}

	r1 = rd(BST_ROOT(e, ROOT_SLOT, 1));
	r1 = (r1 & ~(ROOT_PART_MASK << ROOT_PART_SHIFT)) | (q << ROOT_PART_SHIFT);
	printf("  publish: root w1 -> %08x (partition %u -> %u)\n", r1, p, q);
	wr(BST_ROOT(e, ROOT_SLOT, 1), r1);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: fm6000_bst [-b <bdf>] [-e <engine>] [-n] {-d | -c | -p <file>}\n"
		"  -p  program from a boundary list: '<prefix>/<len> <nexthop> <prec>'\n"
		"      writes ALL 1024 slots of the standby, then flips the partition\n"
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
	const char *pfile = NULL;

	for (i = 1; i < argc; i++) {
		if      (!strcmp(argv[i], "-n")) dry = 1;
		else if (!strcmp(argv[i], "-d")) mode = 'd';
		else if (!strcmp(argv[i], "-c")) mode = 'c';
		else if (!strcmp(argv[i], "-p") && i + 1 < argc) { mode = 'p'; pfile = argv[++i]; }
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) bdf = argv[++i];
		else if (!strcmp(argv[i], "-e") && i + 1 < argc) e = strtoul(argv[++i], NULL, 0);
		else { usage(); return 2; }
	}
	if (!mode || e >= BST_ENGINES) { usage(); return 2; }

	/* A dry run touches no register, so it does not need the device -- that
	 * is what lets the generated array be diffed against a capture offline. */
	if (!dry) {
		snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/resource0", bdf);
		fd = open(p, O_RDWR | O_SYNC);
		if (fd < 0) { perror("open"); return 1; }
		M = mmap(NULL, 32u*1024*1024, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
		if (M == MAP_FAILED) { perror("mmap"); return 1; }
	} else if (mode != 'p') {
		fprintf(stderr, "-n is only meaningful with -p\n"); return 2;
	}

	if (mode == 'd') { dump(e); return 0; }
	if (mode == 'p') return program(e, pfile);
	return copy_and_flip(e, 1);
}
