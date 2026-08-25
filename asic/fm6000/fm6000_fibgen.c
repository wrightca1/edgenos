/* fm6000_fibgen.c - turn the kernel's routing state into an FM6000 boundary list.
 *
 * This is the POLICY half of the hardware FIB. The mechanism half is
 * fm6000_bst, which takes a boundary list, sorts it, right-aligns it and
 * publishes it into the standby partition. The two compose:
 *
 *     fm6000_fibgen --map et1=0x3ef --nh-out /tmp/nh | fm6000_bst -p /dev/stdin
 *     fm6000_bst -N /tmp/nh
 *
 * The rule was read off a converged EOS FIB by correlating all 49 of its
 * boundaries against `show ip route` and `show ip arp` (docs/EDGENOS-7150.md (was ROUTED-PORT-ANATOMY)):
 *
 *   - every RIB route  -> one boundary at its network address, with the route's
 *                         prefix length, pointing at its gateway's nexthop
 *   - connected subnet -> /32 boundaries for the network address and the
 *                         broadcast, both GLEAN
 *   - our own address  -> /32 boundary, LOCAL, precedence 3
 *   - resolved neighbour -> /32 boundary with its own nexthop entry
 *   - 0.0.0.0/0        -> not a boundary; emitted as such so fm6000_bst puts its
 *                         action in the zero-key slot below the lowest boundary
 *
 * lpm is derived by fm6000_bst as 32-len, which is why a connected /29 emits
 * /32 boundaries and not a /29 one.
 *
 * *** WHAT IS SYNTHESISED AND WHAT IS NOT ***
 * Neighbour and gateway NEXTHOP entries are synthesised: the encoding is fully
 * decoded ({MAC[31:0]}, {logical_id << 16 | MAC[47:32]}) and verified against
 * `show ip arp` line for line.
 *
 * The GLEAN, LOCAL and LOOPBACK entries are REFERENCED BY INDEX, not built.
 * They hold a pseudo-MAC with logical id 0x4fff -- 0x0000ff15 for glean/local,
 * 0x0000ff16 for loopback -- and the meaning of those trap codes is not
 * established. Indices 5/7/6 are what EOS allocated on this box; they are
 * options because depending on someone else's allocation is exactly the mistake
 * that produced the wrong 0x3ed logical id for port 3.
 *
 * Note that EOS's glean (5) and local (7) entries are byte-identical. The
 * distinction is carried by the action's Precedence -- 2 for glean, 3 for
 * local -- not by the nexthop.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <net/if.h>

#define MAX_BND   1024
#define MAX_NH    256
#define MAX_IFACE 32

/* kind, highest priority first -- used to break ties on the same key */
enum { K_LOCAL, K_NEIGH, K_GLEAN, K_ROUTE, K_NKINDS };

struct bnd { uint32_t key; uint8_t len, prec, kind; uint16_t nh; };
struct nh  { uint16_t idx; uint32_t w0, w1; };
struct ifent { char name[IFNAMSIZ]; uint16_t lid; };

static struct bnd B[MAX_BND];  static int nb;
static struct nh  NH[MAX_NH];  static int nnh;
static struct ifent IF[MAX_IFACE]; static int nif;

static unsigned nh_glean = 5, nh_local = 7, nh_loop = 6, nh_base = 10;
static int have_default, default_nh;

static uint16_t lid_of(const char *ifname)
{
	int i;
	for (i = 0; i < nif; i++)
		if (!strcmp(IF[i].name, ifname)) return IF[i].lid;
	return 0;
}

static void add_bnd(uint32_t key, unsigned len, unsigned nh, unsigned prec, unsigned kind)
{
	int i;
	for (i = 0; i < nb; i++) {
		if (B[i].key != key) continue;
		if (kind < B[i].kind) {                   /* higher priority wins */
			B[i].len = len; B[i].nh = nh; B[i].prec = prec; B[i].kind = kind;
		}
		return;
	}
	if (nb >= MAX_BND) { fprintf(stderr, "too many boundaries\n"); exit(1); }
	B[nb].key = key; B[nb].len = len; B[nb].nh = nh;
	B[nb].prec = prec; B[nb].kind = kind; nb++;
}

/* one NEXTHOP entry per (mac, logical id); returns its index */
static int add_nh(const uint8_t *mac, uint16_t lid)
{
	uint32_t w0 = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
		      ((uint32_t)mac[4] << 8)  |  (uint32_t)mac[5];
	uint32_t w1 = ((uint32_t)lid << 16) | ((uint32_t)mac[0] << 8) | mac[1];
	int i;
	for (i = 0; i < nnh; i++)
		if (NH[i].w0 == w0 && NH[i].w1 == w1) return NH[i].idx;
	if (nnh >= MAX_NH) { fprintf(stderr, "too many nexthops\n"); exit(1); }
	NH[nnh].idx = (uint16_t)(nh_base + nnh);
	NH[nnh].w0 = w0; NH[nnh].w1 = w1;
	return NH[nnh++].idx;
}

static int parse_mac(const char *s, uint8_t *m)
{
	unsigned a[6];
	if (sscanf(s, "%x:%x:%x:%x:%x:%x", a,a+1,a+2,a+3,a+4,a+5) != 6) return -1;
	for (int i = 0; i < 6; i++) m[i] = (uint8_t)a[i];
	return 0;
}

/* /proc/net/arp: IP  HWtype  Flags  HWaddr  Mask  Device */
static void load_neigh(const char *path)
{
	char line[256], ips[64], hw[64], mask[32], dev[64];
	unsigned type, flags;
	FILE *f = fopen(path, "r");
	if (!f) { perror(path); exit(1); }
	if (!fgets(line, sizeof line, f)) { fclose(f); return; }   /* header */
	while (fgets(line, sizeof line, f)) {
		uint8_t mac[6]; struct in_addr a;
		if (sscanf(line, "%63s %x %x %63s %31s %63s",
			   ips, &type, &flags, hw, mask, dev) != 6) continue;
		if (!(flags & 0x2)) continue;                   /* ATF_COM: resolved */
		if (parse_mac(hw, mac) < 0) continue;
		if (!inet_aton(ips, &a)) continue;
		if (!lid_of(dev)) continue;                     /* not an ASIC port */
		add_bnd(ntohl(a.s_addr), 32, add_nh(mac, lid_of(dev)), 2, K_NEIGH);
	}
	fclose(f);
}

/* /proc/net/route: Iface Dest Gateway Flags RefCnt Use Metric Mask ... (hex LE) */
static void load_routes(const char *path)
{
	char line[512], dev[64];
	unsigned long dst, gw, flags, mask;
	FILE *f = fopen(path, "r");
	if (!f) { perror(path); exit(1); }
	if (!fgets(line, sizeof line, f)) { fclose(f); return; }
	while (fgets(line, sizeof line, f)) {
		unsigned long refc, use, metric;
		if (sscanf(line, "%63s %lx %lx %lx %lu %lu %lu %lx",
			   dev, &dst, &gw, &flags, &refc, &use, &metric, &mask) != 8)
			continue;
		uint32_t d = ntohl((uint32_t)dst), m = ntohl((uint32_t)mask), g = ntohl((uint32_t)gw);
		int len = 0; uint32_t t = m;
		while (t & 0x80000000u) { len++; t <<= 1; }
		if (!lid_of(dev)) continue;

		if (!g) {
			/* connected: network + broadcast are glean; the subnet itself
			 * gets no boundary of its own -- see the doc. */
			if (len < 32) {
				add_bnd(d, 32, nh_glean, 2, K_GLEAN);
				add_bnd(d | ~m, 32, nh_glean, 2, K_GLEAN);
			}
			continue;
		}
		if (!len) { have_default = 1; continue; }    /* nh filled in later */
		add_bnd(d, len, 0, 2, K_ROUTE);              /* nh resolved below */
	}
	fclose(f);
}

/* second pass: point every route boundary at the nexthop for its gateway.
 * All gateways on this box resolve through the neighbour table, so the entry
 * already exists; EOS keeps a separate index for the gateway role, which we
 * reproduce by allocating one rather than reusing the neighbour's. */
static void resolve_routes(const char *rpath)
{
	char line[512], dev[64];
	unsigned long dst, gw, flags, mask, refc, use, metric;
	FILE *f = fopen(rpath, "r");
	if (!f) return;
	if (!fgets(line, sizeof line, f)) { fclose(f); return; }
	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "%63s %lx %lx %lx %lu %lu %lu %lx",
			   dev, &dst, &gw, &flags, &refc, &use, &metric, &mask) != 8)
			continue;
		uint32_t d = ntohl((uint32_t)dst), g = ntohl((uint32_t)gw);
		int i, gnh = -1;
		if (!g || !lid_of(dev)) continue;
		for (i = 0; i < nb; i++)             /* find the gateway's neighbour entry */
			if (B[i].key == g && B[i].kind == K_NEIGH) { gnh = B[i].nh; break; }
		if (gnh < 0) continue;               /* gateway not resolved yet */
		if (!mask) { have_default = 1; default_nh = gnh; continue; }
		for (i = 0; i < nb; i++)
			if (B[i].key == d && B[i].kind == K_ROUTE) B[i].nh = (uint16_t)gnh;
	}
	fclose(f);
}

static void load_addrs_live(void)
{
	struct ifaddrs *ifa, *p;
	if (getifaddrs(&ifa)) { perror("getifaddrs"); exit(1); }
	for (p = ifa; p; p = p->ifa_next) {
		if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
		if (!lid_of(p->ifa_name)) continue;
		add_bnd(ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr),
			32, nh_local, 3, K_LOCAL);
	}
	freeifaddrs(ifa);
}

/* test input: "<ip>/<len> <iface>" per line */
static void load_addrs_file(const char *path)
{
	char line[256], dev[64], ips[64];
	unsigned len;
	FILE *f = fopen(path, "r");
	if (!f) { perror(path); exit(1); }
	while (fgets(line, sizeof line, f)) {
		struct in_addr a;
		if (line[0] == '#' || line[0] == '\n') continue;
		if (sscanf(line, "%63[^/]/%u %63s", ips, &len, dev) != 3) continue;
		if (!inet_aton(ips, &a) || !lid_of(dev)) continue;
		add_bnd(ntohl(a.s_addr), 32, nh_local, 3, K_LOCAL);
	}
	fclose(f);
}

static void parse_map(char *s)
{
	char *tok = strtok(s, ",");
	while (tok) {
		char *eq = strchr(tok, '=');
		if (!eq || nif >= MAX_IFACE) { fprintf(stderr, "bad --map: %s\n", tok); exit(2); }
		*eq = 0;
		snprintf(IF[nif].name, sizeof IF[nif].name, "%s", tok);
		IF[nif].lid = (uint16_t)strtoul(eq + 1, NULL, 0);
		nif++;
		tok = strtok(NULL, ",");
	}
}

static void usage(void)
{
	fprintf(stderr,
	  "usage: fm6000_fibgen --map <if>=<lid>[,...] [options]\n"
	  "  --map et1=0x3ef,et3=0x3f0   interface -> internal VLAN (REQUIRED; not guessable)\n"
	  "  --routes <f>   default /proc/net/route\n"
	  "  --neigh  <f>   default /proc/net/arp\n"
	  "  --addrs  <f>   default: live getifaddrs\n"
	  "  --nh-out <f>   write the NEXTHOP entries to program ('<idx> <w0> <w1>')\n"
	  "  --nh-base <n>  first synthesised NEXTHOP index (default 10)\n"
	  "  --glean/--local/--loopback <n>  trap entry indices (default 5/7/6)\n"
	  "stdout is the boundary list for `fm6000_bst -p`.\n");
}

int main(int argc, char **argv)
{
	const char *rp = "/proc/net/route", *np = "/proc/net/arp", *ap = NULL, *nho = NULL;
	int i;

	for (i = 1; i < argc; i++) {
		if      (!strcmp(argv[i], "--map")      && i+1 < argc) parse_map(argv[++i]);
		else if (!strcmp(argv[i], "--routes")   && i+1 < argc) rp = argv[++i];
		else if (!strcmp(argv[i], "--neigh")    && i+1 < argc) np = argv[++i];
		else if (!strcmp(argv[i], "--addrs")    && i+1 < argc) ap = argv[++i];
		else if (!strcmp(argv[i], "--nh-out")   && i+1 < argc) nho = argv[++i];
		else if (!strcmp(argv[i], "--nh-base")  && i+1 < argc) nh_base = strtoul(argv[++i],NULL,0);
		else if (!strcmp(argv[i], "--glean")    && i+1 < argc) nh_glean = strtoul(argv[++i],NULL,0);
		else if (!strcmp(argv[i], "--local")    && i+1 < argc) nh_local = strtoul(argv[++i],NULL,0);
		else if (!strcmp(argv[i], "--loopback") && i+1 < argc) nh_loop  = strtoul(argv[++i],NULL,0);
		else { usage(); return 2; }
	}
	if (!nif) { usage(); return 2; }

	load_neigh(np);            /* neighbours first: routes resolve against them */
	load_routes(rp);
	resolve_routes(rp);
	if (ap) load_addrs_file(ap); else load_addrs_live();
	add_bnd(0x7f000001u, 32, nh_loop, 2, K_ROUTE);

	for (i = 0; i < nb; i++) {
		if (B[i].kind == K_ROUTE && !B[i].nh) continue;   /* unresolved gateway */
		printf("%u.%u.%u.%u/%u %u %u\n",
		       B[i].key >> 24, (B[i].key >> 16) & 0xff,
		       (B[i].key >> 8) & 0xff, B[i].key & 0xff,
		       B[i].len, B[i].nh, B[i].prec);
	}
	if (have_default)
		printf("0.0.0.0/0 %u 2\n", default_nh);

	if (nho) {
		FILE *f = fopen(nho, "w");
		if (!f) { perror(nho); return 1; }
		for (i = 0; i < nnh; i++)
			fprintf(f, "%u %08x %08x\n", NH[i].idx, NH[i].w0, NH[i].w1);
		fclose(f);
		fprintf(stderr, "fibgen: %d boundaries, %d nexthops -> %s\n", nb, nnh, nho);
	}
	return 0;
}
