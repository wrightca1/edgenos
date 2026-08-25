/* fm6000_rport.c - make a front-panel port a ROUTED port.
 *
 * A routed FM6000 port is five registers. Not the GLORT CAM, not the L2F port
 * masks, not SAF_MATRIX or LBS_CAM -- configuring a port routed on EOS and
 * diffing the live chip against its access-port state changes none of those by
 * a single word. See docs/EDGENOS-7150.md (was ROUTED-PORT-ANATOMY) for the derivation.
 *
 * The recipe, validated structurally against three independent routed ports on
 * a live EOS (Et2/V=1006, Et1/V=1007, Et3/V=1008), which hold it identically:
 *
 *   MAPPER_SRC_PORT_TABLE[port] w0 = 0x02000000   w1 = 0x00000049
 *   MAPPER_VID1_TABLE[V]           = 0x00010000   (VID1_Flag, bit 16)
 *   MAPPER_VID2_TABLE[V]           = 0x0000001b   (MAP_VID2 = 27)
 *   L2L_EVID1_TABLE[V]         w0  = 0x02000000|V (ET_IDX=1, MA1_FID1=V)
 *   MOD_L2_VLAN1_TX_TAGGED[V]  w0  = 0x00000001   (PortMask[75:0] bit 0)
 *
 * V is an *allocated* internal VLAN, not derived from the port number. EOS
 * counts downward in configuration order (Et2=0x3ee, Et1=0x3ef, Et3=0x3f0), so
 * the caller passes it explicitly rather than having it guessed here -- an
 * earlier attempt inferred 0x3ed for port 3 by analogy and was simply wrong.
 *
 * This programs the static per-port state only. The dynamic half -- the
 * NEXTHOP_TABLE entry per resolved neighbour and the FFU BST route entries --
 * belongs to fm6000_fibd, which learns them from the kernel.
 *
 * Address arithmetic is the header's, and note the strides: EVID1 is 2 words
 * per VLAN and TX_TAGGED is 4. Reading either at stride 1 produces a wrong and
 * very plausible answer.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define MAPPER_BASE 0x120000u
#define L2L_BASE    0x030000u
#define MOD_BASE    0x150000u

#define MAPPER_VID1_TABLE(v)          (MAPPER_BASE + 0x1000u + (v))
#define MAPPER_VID2_TABLE(v)          (MAPPER_BASE + 0x2000u + (v))
#define MAPPER_SRC_PORT_TABLE(p, w)   (MAPPER_BASE + 0x3000u + 2u*(p) + (w))
#define L2L_EVID1_TABLE(v, w)         (L2L_BASE    + 0x2000u + 2u*(v) + (w))
#define MOD_L2_VLAN1_TX_TAGGED(v, w)  (MOD_BASE             + 4u*(v) + (w))

#define SRC_PORT_W0_ROUTED  0x02000000u
#define SRC_PORT_W1_ROUTED  0x00000049u   /* QOS_TAG[38:34] = 18 */
#define VID1_FLAG           0x00010000u   /* bit 16: "this VLAN is routed"   */
#define VID2_MAP_ROUTED     0x0000001bu   /* MAP_VID2 = 27, same on all      */
#define EVID1_ET_IDX1       0x02000000u   /* ET_IDX[32:25] = 1               */
#define TX_TAGGED_CPU       0x00000001u   /* PortMask bit 0                  */

#define MAX_W 16

struct w { uint32_t addr, val; const char *name; };

static int build(struct w *o, unsigned port, unsigned vlan)
{
	int n = 0;
	o[n++] = (struct w){ MAPPER_SRC_PORT_TABLE(port, 0), SRC_PORT_W0_ROUTED,
			     "MAPPER_SRC_PORT_TABLE w0" };
	o[n++] = (struct w){ MAPPER_SRC_PORT_TABLE(port, 1), SRC_PORT_W1_ROUTED,
			     "MAPPER_SRC_PORT_TABLE w1" };
	o[n++] = (struct w){ MAPPER_VID1_TABLE(vlan),        VID1_FLAG,
			     "MAPPER_VID1_TABLE" };
	o[n++] = (struct w){ MAPPER_VID2_TABLE(vlan),        VID2_MAP_ROUTED,
			     "MAPPER_VID2_TABLE" };
	o[n++] = (struct w){ L2L_EVID1_TABLE(vlan, 0),       EVID1_ET_IDX1 | vlan,
			     "L2L_EVID1_TABLE w0" };
	o[n++] = (struct w){ L2L_EVID1_TABLE(vlan, 1),       0,
			     "L2L_EVID1_TABLE w1" };
	o[n++] = (struct w){ MOD_L2_VLAN1_TX_TAGGED(vlan, 0), TX_TAGGED_CPU,
			     "MOD_L2_VLAN1_TX_TAGGED w0" };
	o[n++] = (struct w){ MOD_L2_VLAN1_TX_TAGGED(vlan, 1), 0,
			     "MOD_L2_VLAN1_TX_TAGGED w1" };
	o[n++] = (struct w){ MOD_L2_VLAN1_TX_TAGGED(vlan, 2), 0,
			     "MOD_L2_VLAN1_TX_TAGGED w2" };
	return n;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: fm6000_rport [-n] [-v] [-b <bdf>] <port> <vlan>\n"
		"  <port>  alta port number   (front-panel 1 = 40, port 3 = 41)\n"
		"  <vlan>  internal VLAN id   (Et1 = 0x3ef, Et3 = 0x3f0)\n"
		"  -n      dry run: print the writes, touch nothing\n"
		"  -v      verify by readback after writing\n"
		"  -b      PCI bdf (default 0000:02:00.0)\n");
}

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	char p[256];
	int fd, i, n, dry = 0, verify = 0, bad = 0, pos = 0;
	long port = -1, vlan = -1;
	volatile uint32_t *M = NULL;
	struct w ops[MAX_W];

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n")) dry = 1;
		else if (!strcmp(argv[i], "-v")) verify = 1;
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) bdf = argv[++i];
		else if (argv[i][0] == '-') { usage(); return 2; }
		else if (pos++ == 0) port = strtol(argv[i], NULL, 0);
		else vlan = strtol(argv[i], NULL, 0);
	}
	if (port < 0 || port > 75 || vlan < 0 || vlan > 4095) { usage(); return 2; }

	n = build(ops, (unsigned)port, (unsigned)vlan);

	if (dry) {
		printf("fm6000_rport: port %ld, internal VLAN %ld (0x%lx) -- DRY RUN\n",
		       port, vlan, vlan);
		for (i = 0; i < n; i++)
			printf("  %08x <- %08x   %s\n",
			       ops[i].addr, ops[i].val, ops[i].name);
		return 0;
	}

	snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/resource0", bdf);
	fd = open(p, O_RDWR | O_SYNC);
	if (fd < 0) { perror("open"); return 1; }
	M = mmap(NULL, 32u*1024*1024, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (M == MAP_FAILED) { perror("mmap"); return 1; }

	printf("fm6000_rport: port %ld, internal VLAN %ld (0x%lx)\n",
	       port, vlan, vlan);
	for (i = 0; i < n; i++) {
		uint32_t before = M[ops[i].addr];
		M[ops[i].addr] = ops[i].val;
		__sync_synchronize();
		printf("  %08x %08x -> %08x   %s\n",
		       ops[i].addr, before, ops[i].val, ops[i].name);
	}

	if (verify) {
		for (i = 0; i < n; i++) {
			uint32_t got = M[ops[i].addr];
			if (got != ops[i].val) {
				printf("  VERIFY FAIL %08x want=%08x got=%08x  %s\n",
				       ops[i].addr, ops[i].val, got, ops[i].name);
				bad++;
			}
		}
		/* A verify that checks nothing must not print PASS -- that
		 * vacuous-invariant bug has been shipped five times here. */
		if (n == 0) { printf("  VERIFY: nothing checked\n"); return 1; }
		printf("  VERIFY %s (%d words)\n", bad ? "FAIL" : "PASS", n);
	}
	return bad ? 1 : 0;
}
