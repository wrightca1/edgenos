/*
 * Hardware L3: mirror the Linux FIB into the ASIC, so routed packets are
 * forwarded by the chip instead of taking a round trip through the CPU.
 *
 * What the chip needs, and why:
 *
 *   bcm_l3_intf_create   a router interface holding OUR MAC for the VLAN. This
 *                        is the source MAC the chip puts on routed frames.
 *   bcm_l2_station_add   MY_STATION. Without it the chip treats a frame sent to
 *                        our MAC as an L2 frame to be switched (or punted via
 *                        the static L2 entry the bridge installs) and never
 *                        considers routing it. This is the entry that says
 *                        "frames to this MAC are mine -- route them".
 *   bcm_l3_egress_create a next hop: destination MAC, port and VLAN to send to.
 *   bcm_l3_route_add     the LPM entry itself, in DEFIP, pointing at that egress.
 *   bcm_field_*          the punt rules that keep the control plane alive once
 *                        MY_STATION exists. See sec.FP below -- this is the part
 *                        that was missing for a week.
 *
 * ONE ROUTER INTERFACE PER PORT. Each bridged port gets its own VLAN, its own
 * L3 interface and its own MY_STATION entry, which is what EOS does and what
 * makes forwarding BETWEEN ports a chip operation. With a single port the chip
 * could never be shown to forward at all: the only path was back out the
 * ingress interface, which Trident2+ drops by design.
 *
 * The FIB source is /proc/net/route (v4) and /proc/net/ipv6_route (v6) rather
 * than a netlink route socket. Same information for our purposes, no state
 * machine, and re-reading a few times a minute costs nothing at this scale.
 * Neighbours come from /proc/net/arp for v4; v6 has no /proc equivalent, so the
 * NDP cache is read over netlink (nd_lookup below). A real implementation would
 * follow netlink for routes too and react to churn immediately; this is
 * deliberately the simple version, and the boundary is documented not hidden.
 *
 * Addresses in /proc/net/route are printed as the in-kernel big-endian u32 read
 * back as a host u32, so on this little-endian box they come out byte-reversed:
 * 10.101.101.0 prints as 0065650A. be_hex_to_host() undoes that.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <sal/types.h>
#include <bcm/types.h>
#include <bcm/error.h>
#include <bcm/l2.h>
#include <bcm/l3.h>
#include <bcm/field.h>
#include <soc/drv.h>
#include <bcm/switch.h>

#define MAX_IF  8
#define MAX_NH  32
#define MAX_RT  512

struct l3if {
    char       ifname[IFNAMSIZ];
    int        port;
    bcm_vlan_t vlan;
    bcm_mac_t  mac;
    bcm_if_t   intf;
    int        station;
    bcm_if_t   cpu_eg;          /* egress object pointing at the CPU port */
    int        self_done;       /* v4 self-punt host entry installed       */
    int        fp_self;         /* per-interface "traffic aimed at us" rule */
    int        fp_stat_self;
};

static struct l3if ifs[MAX_IF];
static int nif;
static int l3_unit, l3_on;

static struct { int ifx; uint32_t gw;   bcm_if_t eg; } nh4[MAX_NH];
static int nnh4;
static struct { int ifx; uint8_t gw[16]; bcm_if_t eg; } nh6[MAX_NH];
static int nnh6;

static struct { uint32_t dst, mask; bcm_if_t eg; int seen; } rt4[MAX_RT];
static int nrt4;
static struct { uint8_t dst[16]; int plen; bcm_if_t eg; int seen; } rt6[MAX_RT];
static int nrt6;

static struct { int ifx; uint32_t ip; }  hs4[MAX_NH * 4];
static int nhs4;
static struct { int ifx; uint8_t ip[16]; } hs6[MAX_NH * 4];
static int nhs6;
static unsigned long st_host4, st_host6;
static struct { int ifx; uint8_t ip[16]; } self6[MAX_IF * 4];
static int nself6;

static unsigned long st4_added, st4_failed, st4_unresolved, st4_moved, st4_gone;
static unsigned long st6_added, st6_failed, st6_unresolved, st6_moved, st6_gone;

/* Field processor: one group for v4, one for v6. */
static bcm_field_group_t fp_grp4 = -1, fp_grp6 = -1;
static bcm_field_entry_t fp_ospf4 = -1, fp_ospf6 = -1;
static int fp_stat_ospf4 = -1, fp_stat_ospf6 = -1;

static uint32_t be_hex_to_host(const char *h)
{
    unsigned long v = strtoul(h, NULL, 16);
    return ((v & 0xff) << 24) | (((v >> 8) & 0xff) << 16) |
           (((v >> 16) & 0xff) << 8) | ((v >> 24) & 0xff);
}

static struct l3if *if_by_name(const char *n)
{
    int i;

    for (i = 0; i < nif; i++) {
        if (strcmp(ifs[i].ifname, n) == 0) return &ifs[i];
    }
    return NULL;
}

/* Our own address on an interface, for the "this is me" host entry. */
static int iface_ipv4(const char *ifname, uint32_t *ip)
{
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0), rv;

    if (fd < 0) return -1;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_addr.sa_family = AF_INET;
    rv = ioctl(fd, SIOCGIFADDR, &ifr);
    if (rv == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        *ip = ntohl(sin->sin_addr.s_addr);
    }
    close(fd);
    return rv;
}

/* v4 neighbour: the kernel exports the ARP cache as text. */
static int arp_lookup(const char *dev, uint32_t gw, uint8_t *mac)
{
    char line[256], ip[64], hw[64], d[64];
    unsigned int m[6];
    FILE *f = fopen("/proc/net/arp", "r");
    int found = 0;

    if (!f) return 0;
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }   /* header */
    while (fgets(line, sizeof(line), f)) {
        unsigned a, b, c, e;
        if (sscanf(line, "%63s %*s %*s %63s %*s %63s", ip, hw, d) != 3)
            continue;
        if (strcmp(d, dev) != 0)                          continue;
        if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &e) != 4) continue;
        if (((a << 24) | (b << 16) | (c << 8) | e) != gw) continue;
        if (sscanf(hw, "%x:%x:%x:%x:%x:%x",
                   &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) continue;
        for (int i = 0; i < 6; i++) mac[i] = (uint8_t)m[i];
        found = 1;
        break;
    }
    fclose(f);
    return found;
}

/* v6 neighbour: THERE IS NO /proc FILE FOR THE NDP CACHE.
 *
 * /proc/net/arp is v4 only and has no v6 counterpart, so the neighbour has to
 * come from the kernel over netlink. This dumps RTM_GETNEIGH and looks for an
 * NDA_DST matching the next hop, taking its NDA_LLADDR. Reachability state is
 * deliberately not filtered on: a STALE entry still has the right MAC, and
 * refusing it would leave a route unprogrammed for no benefit. */
static int nd_lookup(const uint8_t *ip6, uint8_t *mac)
{
    struct { struct nlmsghdr n; struct ndmsg r; } req;
    uint8_t buf[16384];
    int s, len, found = 0;

    s = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (s < 0) return 0;

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len   = NLMSG_LENGTH(sizeof(struct ndmsg));
    req.n.nlmsg_type  = RTM_GETNEIGH;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.n.nlmsg_seq   = 1;
    req.r.ndm_family  = AF_INET6;

    if (send(s, &req, req.n.nlmsg_len, 0) < 0) { close(s); return 0; }

    while (!found && (len = recv(s, buf, sizeof(buf), 0)) > 0) {
        struct nlmsghdr *nh = (struct nlmsghdr *)buf;

        for (; NLMSG_OK(nh, (unsigned)len); nh = NLMSG_NEXT(nh, len)) {
            struct ndmsg *nd;
            struct rtattr *rta;
            int rlen;
            const uint8_t *dst = NULL, *ll = NULL;

            if (nh->nlmsg_type == NLMSG_DONE) { len = 0; break; }
            if (nh->nlmsg_type != RTM_NEWNEIGH) continue;

            nd  = (struct ndmsg *)NLMSG_DATA(nh);
            if (nd->ndm_family != AF_INET6) continue;
            rta = (struct rtattr *)((char *)nd + NLMSG_ALIGN(sizeof(*nd)));
            rlen = nh->nlmsg_len - NLMSG_LENGTH(sizeof(*nd));

            for (; RTA_OK(rta, rlen); rta = RTA_NEXT(rta, rlen)) {
                if (rta->rta_type == NDA_DST &&
                    RTA_PAYLOAD(rta) == 16) dst = RTA_DATA(rta);
                if (rta->rta_type == NDA_LLADDR &&
                    RTA_PAYLOAD(rta) == 6)  ll  = RTA_DATA(rta);
            }
            if (dst && ll && memcmp(dst, ip6, 16) == 0) {
                memcpy(mac, ll, 6);
                found = 1;
                break;
            }
        }
        if (len == 0) break;
    }
    close(s);
    return found;
}

/* IPv6 MULTICAST IS NOT A NEXT HOP.
 *
 * The kernel's NDP cache holds multicast entries alongside real neighbours --
 * solicited-node (ff02::1:ffxx:xxxx), all-nodes, all-routers. Mirroring those
 * the way we mirror unicast neighbours built nine egress objects pointing at
 * 33:33:.. multicast MACs, none of which is a destination anything can be
 * forwarded to. Skip them at every entry point rather than only where they were
 * first noticed. */
static int is_mcast6(const uint8_t *a)
{
    return a[0] == 0xff;
}

/* One egress object per (interface, gateway), created on first use. */
static int nexthop4(struct l3if *ifp, int ifx, uint32_t gw, bcm_if_t *eg)
{
    bcm_l3_egress_t egr;
    uint8_t mac[6];
    int rv, i;

    for (i = 0; i < nnh4; i++) {
        if (nh4[i].ifx == ifx && nh4[i].gw == gw) { *eg = nh4[i].eg; return BCM_E_NONE; }
    }
    if (nnh4 >= MAX_NH)                        return BCM_E_RESOURCE;
    if (!arp_lookup(ifp->ifname, gw, mac))     return BCM_E_NOT_FOUND;

    bcm_l3_egress_t_init(&egr);
    memcpy(egr.mac_addr, mac, 6);
    egr.intf   = ifp->intf;
    egr.vlan   = ifp->vlan;
    egr.port   = ifp->port;
    egr.module = 0;

    rv = bcm_l3_egress_create(l3_unit, 0, &egr, eg);
    if (rv != BCM_E_NONE) return rv;

    nh4[nnh4].ifx = ifx; nh4[nnh4].gw = gw; nh4[nnh4].eg = *eg; nnh4++;
    printf("l3: v4 next hop %u.%u.%u.%u dev %s via "
           "%02x:%02x:%02x:%02x:%02x:%02x -> egress %d\n",
           gw >> 24, (gw >> 16) & 0xff, (gw >> 8) & 0xff, gw & 0xff, ifp->ifname,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], *eg);
    fflush(stdout);
    return BCM_E_NONE;
}

static int nexthop6(struct l3if *ifp, int ifx, const uint8_t *gw, bcm_if_t *eg)
{
    bcm_l3_egress_t egr;
    uint8_t mac[6];
    int rv, i;

    for (i = 0; i < nnh6; i++) {
        if (nh6[i].ifx == ifx && memcmp(nh6[i].gw, gw, 16) == 0) {
            *eg = nh6[i].eg; return BCM_E_NONE;
        }
    }
    if (is_mcast6(gw))           return BCM_E_PARAM;
    if (nnh6 >= MAX_NH)          return BCM_E_RESOURCE;
    if (!nd_lookup(gw, mac))     return BCM_E_NOT_FOUND;

    bcm_l3_egress_t_init(&egr);
    memcpy(egr.mac_addr, mac, 6);
    egr.intf   = ifp->intf;
    egr.vlan   = ifp->vlan;
    egr.port   = ifp->port;
    egr.module = 0;

    rv = bcm_l3_egress_create(l3_unit, 0, &egr, eg);
    if (rv != BCM_E_NONE) return rv;

    nh6[nnh6].ifx = ifx; memcpy(nh6[nnh6].gw, gw, 16); nh6[nnh6].eg = *eg; nnh6++;
    printf("l3: v6 next hop dev %s via %02x:%02x:%02x:%02x:%02x:%02x -> egress %d\n",
           ifp->ifname, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], *eg);
    fflush(stdout);
    return BCM_E_NONE;
}

/* ---------------------------------------------------------------------------
 * FIELD-PROCESSOR PUNT RULES -- the piece that was missing.
 *
 * MY_STATION makes the chip route frames addressed to our MAC. That is the
 * whole point of it, but it also means packets aimed AT US are handed to the
 * routing engine, which has no business terminating them. A self-address host
 * entry covers exactly one address and, on its own, did not restore the control
 * plane: the adjacency stalled at ExStart, because OSPF's unicast Database
 * Description packets were being routed rather than delivered.
 *
 * EOS does not rely on host entries for this at all. Its ingress field
 * processor carries explicit rules, read off the running reference platform:
 *
 *   IPv4 proto 89 to 224.0.0.4/30   -> CopyToCpu      (OSPF, all of it)
 *   DstClassL3 == <local address>   -> CopyToCpu      (anything aimed at us)
 *
 * We reproduce the shape, not the exact encoding. A rule on IP protocol 89,
 * which catches hellos and unicast DD/LSU/LSAck alike and is immune to whatever
 * the routing engine would otherwise decide to do with them; and a rule per
 * interface on our own address. Matching DstIp directly rather than DstClassL3
 * drops the need to maintain a lookup class on every local host entry.
 *
 * v4 and v6 need SEPARATE GROUPS. bcmFieldQualifyIp6NextHeader is the same enum
 * value as bcmFieldQualifyIpProtocol, but the destination qualifier is not --
 * DstIp is 32 bits and DstIp6 is 128 -- so one group cannot carry both keys.
 *
 * Every rule carries a counter, because "the rule installed" and "the rule is
 * being hit" are different claims and only the second is evidence. This project
 * has already closed one lead on the strength of a return code that meant
 * nothing.
 */
static int fp_add(bcm_field_group_t grp, const char *what,
                  bcm_field_entry_t *ent, int *stat_id,
                  int (*qual)(bcm_field_entry_t), int quiet)
{
    bcm_field_stat_t stats[1] = { bcmFieldStatPackets };
    int rv;

    if (grp < 0) return -1;
    rv = bcm_field_entry_create(l3_unit, grp, ent);
    if (rv != BCM_E_NONE) {
        printf("fp: %s entry_create rv=%d\n", what, rv);
        return -1;
    }
    rv = qual(*ent);
    if (rv != BCM_E_NONE) {
        printf("fp: %s qualify rv=%d\n", what, rv);
        return -1;
    }
    rv = bcm_field_action_add(l3_unit, *ent, bcmFieldActionCopyToCpu, 0, 0);
    if (rv != BCM_E_NONE) {
        printf("fp: %s action_add rv=%d\n", what, rv);
        return -1;
    }
    /* Counter is best effort -- a chip that will not give us one is not a
     * reason to refuse to install the rule that carries the traffic. */
    if (bcm_field_stat_create(l3_unit, grp, 1, stats, stat_id) != BCM_E_NONE)
        *stat_id = -1;
    else if (bcm_field_entry_stat_attach(l3_unit, *ent, *stat_id) != BCM_E_NONE)
        *stat_id = -1;

    rv = bcm_field_entry_install(l3_unit, *ent);
    if (!quiet || rv != BCM_E_NONE) {
        printf("fp: %s -> CopyToCpu entry=%d stat=%d install rv=%d\n",
               what, *ent, *stat_id, rv);
        fflush(stdout);
    }
    return rv == BCM_E_NONE ? 0 : -1;
}

static int q_ospf4(bcm_field_entry_t e)
{
    return bcm_field_qualify_IpProtocol(l3_unit, e, 89, 0xff);
}
static int q_ospf6(bcm_field_entry_t e)
{
    return bcm_field_qualify_Ip6NextHeader(l3_unit, e, 89, 0xff);
}

static uint32_t q_self_ip;
static int q_self4(bcm_field_entry_t e)
{
    return bcm_field_qualify_DstIp(l3_unit, e, (bcm_ip_t)q_self_ip, 0xffffffff);
}

static void fp_setup(void)
{
    bcm_field_qset_t q4, q6;
    int rv;

    BCM_FIELD_QSET_INIT(q4);
    BCM_FIELD_QSET_ADD(q4, bcmFieldQualifyStageIngress);
    BCM_FIELD_QSET_ADD(q4, bcmFieldQualifyIpProtocol);
    BCM_FIELD_QSET_ADD(q4, bcmFieldQualifyDstIp);
    rv = bcm_field_group_create(l3_unit, q4, BCM_FIELD_GROUP_PRIO_ANY, &fp_grp4);
    printf("fp: v4 group=%d rv=%d\n", fp_grp4, rv);
    if (rv != BCM_E_NONE) fp_grp4 = -1;
    else fp_add(fp_grp4, "v4 ospf(proto 89)", &fp_ospf4, &fp_stat_ospf4,
                q_ospf4, 0);

    BCM_FIELD_QSET_INIT(q6);
    BCM_FIELD_QSET_ADD(q6, bcmFieldQualifyStageIngress);
    BCM_FIELD_QSET_ADD(q6, bcmFieldQualifyIp6NextHeader);
    BCM_FIELD_QSET_ADD(q6, bcmFieldQualifyDstIp6);
    rv = bcm_field_group_create(l3_unit, q6, BCM_FIELD_GROUP_PRIO_ANY, &fp_grp6);
    printf("fp: v6 group=%d rv=%d\n", fp_grp6, rv);
    if (rv != BCM_E_NONE) fp_grp6 = -1;
    else fp_add(fp_grp6, "v6 ospfv3(next hdr 89)", &fp_ospf6, &fp_stat_ospf6,
                q_ospf6, 0);
}

static void fp_show(const char *tag, int stat_id)
{
    uint64 v;

    if (stat_id < 0) return;
    if (bcm_field_stat_get(l3_unit, stat_id, bcmFieldStatPackets, &v)
        != BCM_E_NONE)
        return;
    printf("fp: %-24s punted %llu packets\n", tag,
           (unsigned long long)COMPILER_64_LO(v) |
           ((unsigned long long)COMPILER_64_HI(v) << 32));
}

/* OUR OWN ADDRESS MUST PUNT.
 *
 * MY_STATION diverts packets aimed at US into the routing engine, which has no
 * entry for our own address and drops them. BCM_L3_L2TOCPU on a host entry for
 * our own IP means "deliver to the CPU, unrouted", restoring that punt.
 *
 * Installed lazily from the poll loop, not at start-up: the bridge creates the
 * tap before userspace assigns an address to it, so at start-up there is no
 * address to read -- which is exactly what went wrong the first time.
 *
 * The host entry's l3a_intf is an EGRESS OBJECT id, not an L3 interface id. Our
 * first attempt passed the interface id (1) and the entry, while accepted
 * (rv=0, used_host=1), pointed nowhere useful. EOS's own host table shows what
 * it should look like -- all-zero MAC, CLASS 1, and an INTF in the 100000+
 * range that bcm_l3_egress_create returns. */
static void self_punt_try(struct l3if *ifp)
{
    bcm_l3_host_t   h;
    bcm_l3_egress_t egr;
    uint32_t ip = 0;
    int rv;

    if (iface_ipv4(ifp->ifname, &ip) != 0 || ip == 0) return;

    if (ifp->fp_self < 0 && fp_grp4 >= 0) {
        char b[48];
        snprintf(b, sizeof(b), "v4 self %s(%u.%u.%u.%u)", ifp->ifname,
                 ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
        q_self_ip = ip;
        fp_add(fp_grp4, b, &ifp->fp_self, &ifp->fp_stat_self, q_self4, 0);
    }
    if (ifp->self_done) return;

    if (ifp->cpu_eg < 0) {
        bcm_l3_egress_t_init(&egr);
        egr.intf  = ifp->intf;
        egr.vlan  = ifp->vlan;
        egr.port  = CMIC_PORT(l3_unit);
        egr.flags = BCM_L3_L2TOCPU;
        memset(egr.mac_addr, 0, 6);
        rv = bcm_l3_egress_create(l3_unit, 0, &egr, &ifp->cpu_eg);
        printf("l3: %s CPU egress (port %d) id=%d rv=%d\n",
               ifp->ifname, CMIC_PORT(l3_unit), ifp->cpu_eg, rv);
        if (rv != BCM_E_NONE) { ifp->cpu_eg = -1; return; }
    }

    bcm_l3_host_t_init(&h);
    h.l3a_ip_addr = ip;
    h.l3a_flags   = BCM_L3_L2TOCPU;
    h.l3a_intf    = ifp->cpu_eg;
    rv = bcm_l3_host_add(l3_unit, &h);
    printf("l3: %s self %u.%u.%u.%u -> CPU (L2TOCPU) rv=%d\n", ifp->ifname,
           ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff, rv);
    fflush(stdout);
    if (rv == BCM_E_NONE) ifp->self_done = 1;
}

/* THE CHIP MUST FOLLOW THE CONTROL PLANE, INCLUDING WHEN A ROUTE MOVES.
 *
 * The first version of this cache keyed on (prefix, mask, egress) and treated
 * anything not matching all three as new. When OSPF moved every prefix from one
 * next hop to the other -- which is exactly what happened the moment the second
 * link came up and its lower cost won -- each re-add hit an existing DEFIP entry
 * and returned an error, 442 of them, while the chip quietly went on forwarding
 * to the OLD next hop. A hardware table that lags the routing table is worse
 * than no hardware table: traffic is blackholed with everything reporting
 * healthy.
 *
 * So: look the prefix up on (prefix, mask) alone. If the egress differs, re-add
 * with BCM_L3_REPLACE. And mark every prefix seen on each pass so the ones that
 * have gone away can be deleted, rather than lingering in the chip forever. */
/* OUR OWN v6 ADDRESSES MUST PUNT TOO -- and this was missed the first time.
 *
 * The v4 side has had a self-punt host entry since the day MY_STATION went in,
 * because without it unicast aimed at us is handed to the routing engine and
 * dropped. Exactly the same is true of v6, and adding BCM_L2_STATION_IPV6 to
 * MY_STATION turned that on without the matching punt.
 *
 * It hid, because the thing that normally exposes it -- OSPF -- kept working:
 * OSPFv3 is IP protocol 89, so the v6 field-processor rule punted it and the
 * adjacency went Full. What did NOT work was neighbour discovery. A Neighbour
 * Advertisement for our own address is ICMPv6, not 89, so it was routed and
 * dropped, and the peer's global address sat at FAILED in the NDP cache
 * for ever while its link-local resolved fine:
 *
 *   fe80::6eb2:aeff:fecd:1333  lladdr 6c:b2:ae:cd:13:33  router  STALE
 *   2001:470:882d:1056::2      FAILED
 *
 * A control plane that comes up while the data plane cannot resolve its own
 * neighbours is the worst kind of half-working, so punt every address the
 * interface owns -- link-local included, since OSPFv3 and NDP both use it.
 *
 * /proc/net/if_inet6 columns: address(32 hex) ifindex plen scope flags ifname
 */
static void hex2bin(const char *h, uint8_t *out, int n);   /* defined below */

static uint8_t q_self6_ip[16];
static int q_self6(bcm_field_entry_t e)
{
    bcm_ip6_t data, mask;

    memcpy(data, q_self6_ip, 16);
    memset(mask, 0xff, 16);
    return bcm_field_qualify_DstIp6(l3_unit, e, data, mask);
}

static int self6_seen(int ifx, const uint8_t *ip)
{
    for (int i = 0; i < nself6; i++)
        if (self6[i].ifx == ifx && memcmp(self6[i].ip, ip, 16) == 0) return 1;
    return 0;
}

static void self_punt6_try(struct l3if *ifp)
{
    char line[256], addr[64], name[64];
    unsigned ifidx, plen, scope, flags;
    FILE *f;
    int ifx = (int)(ifp - ifs);

    if (ifp->cpu_eg < 0) return;        /* built by the v4 path first */
    f = fopen("/proc/net/if_inet6", "r");
    if (!f) return;

    while (fgets(line, sizeof(line), f)) {
        bcm_l3_host_t h;
        bcm_field_entry_t ent = -1;
        uint8_t ip[16];
        int stat = -1, rv;
        char b[80];

        if (sscanf(line, "%63s %x %x %x %x %63s",
                   addr, &ifidx, &plen, &scope, &flags, name) != 6) continue;
        if (strcmp(name, ifp->ifname) != 0) continue;
        hex2bin(addr, ip, 16);
        if (is_mcast6(ip))       continue;
        if (self6_seen(ifx, ip)) continue;
        if (nself6 >= (int)(sizeof(self6) / sizeof(self6[0]))) break;

        bcm_l3_host_t_init(&h);
        h.l3a_flags = BCM_L3_IP6 | BCM_L3_L2TOCPU;
        memcpy(h.l3a_ip6_addr, ip, 16);
        h.l3a_intf = ifp->cpu_eg;
        rv = bcm_l3_host_add(l3_unit, &h);

        snprintf(b, sizeof(b), "v6 self %s(%02x%02x:..:%02x%02x)", ifp->ifname,
                 ip[0], ip[1], ip[14], ip[15]);
        if (fp_grp6 >= 0) {
            memcpy(q_self6_ip, ip, 16);
            fp_add(fp_grp6, b, &ent, &stat, q_self6, 0);
        }

        memcpy(self6[nself6].ip, ip, 16); self6[nself6].ifx = ifx; nself6++;
        printf("l3: %s self v6 -> CPU (L2TOCPU) rv=%d\n", ifp->ifname, rv);
        fflush(stdout);
    }
    fclose(f);
}

static int rt4_find(uint32_t dst, uint32_t mask)
{
    for (int i = 0; i < nrt4; i++)
        if (rt4[i].dst == dst && rt4[i].mask == mask) return i;
    return -1;
}

static void rt4_sweep(void)
{
    int i, gone = 0;

    for (i = 0; i < nrt4; ) {
        if (rt4[i].seen) { rt4[i].seen = 0; i++; continue; }

        bcm_l3_route_t r;
        bcm_l3_route_t_init(&r);
        r.l3a_subnet  = rt4[i].dst;
        r.l3a_ip_mask = rt4[i].mask;
        r.l3a_intf    = rt4[i].eg;
        bcm_l3_route_delete(l3_unit, &r);
        st4_gone++; gone++;

        rt4[i] = rt4[nrt4 - 1];         /* compact */
        nrt4--;
    }
    if (gone) {
        printf("l3: -%d v4 routes withdrawn from DEFIP\n", gone);
        fflush(stdout);
    }
}
static int rt6_find(const uint8_t *dst, int plen)
{
    for (int i = 0; i < nrt6; i++)
        if (rt6[i].plen == plen && memcmp(rt6[i].dst, dst, 16) == 0) return i;
    return -1;
}

static void rt6_sweep(void)
{
    int i, gone = 0;

    for (i = 0; i < nrt6; ) {
        if (rt6[i].seen) { rt6[i].seen = 0; i++; continue; }

        bcm_l3_route_t r;
        int j;
        bcm_l3_route_t_init(&r);
        r.l3a_flags = BCM_L3_IP6;
        memcpy(r.l3a_ip6_net, rt6[i].dst, 16);
        for (j = 0; j < 16; j++) {
            int bits = rt6[i].plen - j * 8;
            r.l3a_ip6_mask[j] = bits >= 8 ? 0xff :
                                bits <= 0 ? 0x00 : (uint8_t)(0xff << (8 - bits));
        }
        r.l3a_intf = rt6[i].eg;
        bcm_l3_route_delete(l3_unit, &r);
        st6_gone++; gone++;

        rt6[i] = rt6[nrt6 - 1];
        nrt6--;
    }
    if (gone) {
        printf("l3: -%d v6 routes withdrawn from DEFIP\n", gone);
        fflush(stdout);
    }
}

static void poll_v4(void)
{
    char line[512], iface[64], dst[32], gw[32], mask[32];
    FILE *f = fopen("/proc/net/route", "r");
    int flags, added = 0, idx, moved = 0;

    if (!f) return;
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }   /* header */

    while (fgets(line, sizeof(line), f)) {
        bcm_l3_route_t r;
        struct l3if *ifp;
        bcm_if_t eg;
        uint32_t d, m, g;
        int rv;

        if (sscanf(line, "%63s %31s %31s %x %*d %*d %*d %31s",
                   iface, dst, gw, &flags, mask) != 5) continue;
        ifp = if_by_name(iface);
        if (!ifp)             continue;
        if (!(flags & 0x1))   continue;   /* RTF_UP           */
        if (!(flags & 0x2))   continue;   /* RTF_GATEWAY only */

        d = be_hex_to_host(dst);
        m = be_hex_to_host(mask);
        g = be_hex_to_host(gw);

        rv = nexthop4(ifp, (int)(ifp - ifs), g, &eg);
        if (rv != BCM_E_NONE) { st4_unresolved++; continue; }

        idx = rt4_find(d, m);
        if (idx >= 0) {
            rt4[idx].seen = 1;
            if (rt4[idx].eg == eg) continue;        /* unchanged */
            moved = 1;                              /* same prefix, new next hop */
        }

        bcm_l3_route_t_init(&r);
        r.l3a_subnet  = d;
        r.l3a_ip_mask = m;
        r.l3a_intf    = eg;
        if (moved) r.l3a_flags |= BCM_L3_REPLACE;

        rv = bcm_l3_route_add(l3_unit, &r);
        if (rv != BCM_E_NONE) { st4_failed++; moved = 0; continue; }

        if (moved) {
            rt4[idx].eg = eg;
            st4_moved++; moved = 0;
        } else if (nrt4 < MAX_RT) {
            rt4[nrt4].dst = d; rt4[nrt4].mask = m; rt4[nrt4].eg = eg;
            rt4[nrt4].seen = 1; nrt4++;
            st4_added++; added++;
        }
    }
    fclose(f);
    rt4_sweep();
    if (added) {
        printf("l3: +%d v4 routes into DEFIP (total %lu, moved %lu, failed %lu, unresolved %lu)\n",
               added, st4_added, st4_moved, st4_failed, st4_unresolved);
        fflush(stdout);
    }
}

static void hex2bin(const char *h, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        char b[3] = { h[i * 2], h[i * 2 + 1], 0 };
        out[i] = (uint8_t)strtoul(b, NULL, 16);
    }
}

/* /proc/net/ipv6_route columns:
 *   dst(32hex) dstplen(hex) src(32hex) srcplen nexthop(32hex)
 *   metric refcnt use flags ifname  */
static void poll_v6(void)
{
    char line[512], dst[64], nh[64], iface[64];
    unsigned plen, flags;
    FILE *f = fopen("/proc/net/ipv6_route", "r");
    int added = 0, idx, moved = 0;

    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        bcm_l3_route_t r;
        struct l3if *ifp;
        bcm_if_t eg;
        uint8_t d[16], g[16];
        int rv, i;

        if (sscanf(line, "%63s %x %*s %*x %63s %*x %*x %*x %x %63s",
                   dst, &plen, nh, &flags, iface) != 5) continue;
        ifp = if_by_name(iface);
        if (!ifp)             continue;
        if (!(flags & 0x1))   continue;   /* RTF_UP           */
        if (!(flags & 0x2))   continue;   /* RTF_GATEWAY only */
        if (plen == 0 || plen > 128)      continue;

        hex2bin(dst, d, 16);
        hex2bin(nh,  g, 16);
        if (is_mcast6(d) || is_mcast6(g)) continue;

        rv = nexthop6(ifp, (int)(ifp - ifs), g, &eg);
        if (rv != BCM_E_NONE) { st6_unresolved++; continue; }

        idx = rt6_find(d, (int)plen);
        if (idx >= 0) {
            rt6[idx].seen = 1;
            if (rt6[idx].eg == eg) continue;
            moved = 1;
        }

        bcm_l3_route_t_init(&r);
        r.l3a_flags = BCM_L3_IP6;
        memcpy(r.l3a_ip6_net, d, 16);
        for (i = 0; i < 16; i++) {
            int bits = (int)plen - i * 8;
            r.l3a_ip6_mask[i] = bits >= 8 ? 0xff :
                                bits <= 0 ? 0x00 : (uint8_t)(0xff << (8 - bits));
        }
        r.l3a_intf = eg;
        if (moved) r.l3a_flags |= BCM_L3_REPLACE;

        rv = bcm_l3_route_add(l3_unit, &r);
        if (rv != BCM_E_NONE) { st6_failed++; moved = 0; continue; }

        if (moved) {
            rt6[idx].eg = eg;
            st6_moved++; moved = 0;
        } else if (nrt6 < MAX_RT) {
            memcpy(rt6[nrt6].dst, d, 16);
            rt6[nrt6].plen = (int)plen; rt6[nrt6].eg = eg;
            rt6[nrt6].seen = 1; nrt6++;
            st6_added++; added++;
        }
    }
    fclose(f);
    rt6_sweep();
    if (added) {
        printf("l3: +%d v6 routes into DEFIP (total %lu, moved %lu, failed %lu, unresolved %lu)\n",
               added, st6_added, st6_moved, st6_failed, st6_unresolved);
        fflush(stdout);
    }
}

/* DIRECTLY ATTACHED HOSTS NEED HOST ENTRIES.
 *
 * Only routes with a gateway are mirrored into DEFIP above, which leaves a hole
 * a router cannot have: a destination on one of our own subnets has no gateway,
 * so nothing is programmed and the chip cannot forward to it. That is not an
 * edge case -- the peer at the far end of each link is exactly this, and it is
 * the first thing anyone pings.
 *
 * The L3 host table is what fills it. For every neighbour the kernel has
 * resolved on one of our interfaces, install a host entry pointing at an egress
 * object carrying that neighbour's MAC. Resolution is the kernel's job; we just
 * mirror what it already knows, the same way the route mirror does.
 *
 * Our own address is skipped -- it already has an L2TOCPU entry from
 * self_punt_try(), and overwriting that with a forwarding entry would send
 * packets meant for us back out of the port they came in on.
 */
static int hs4_seen(int ifx, uint32_t ip)
{
    for (int i = 0; i < nhs4; i++)
        if (hs4[i].ifx == ifx && hs4[i].ip == ip) return 1;
    return 0;
}
static int hs6_seen(int ifx, const uint8_t *ip)
{
    for (int i = 0; i < nhs6; i++)
        if (hs6[i].ifx == ifx && memcmp(hs6[i].ip, ip, 16) == 0) return 1;
    return 0;
}

static void poll_hosts4(void)
{
    char line[256], ip[64], hw[64], dev[64];
    FILE *f = fopen("/proc/net/arp", "r");
    int added = 0;

    if (!f) return;
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
    while (fgets(line, sizeof(line), f)) {
        bcm_l3_host_t h;
        struct l3if *ifp;
        bcm_if_t eg;
        unsigned a, b, c, d;
        uint32_t v, self = 0;
        int ifx, rv;

        if (sscanf(line, "%63s %*s %*s %63s %*s %63s", ip, hw, dev) != 3)
            continue;
        if (strcmp(hw, "00:00:00:00:00:00") == 0) continue;   /* incomplete */
        ifp = if_by_name(dev);
        if (!ifp) continue;
        if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) continue;
        v   = (a << 24) | (b << 16) | (c << 8) | d;
        ifx = (int)(ifp - ifs);

        if (iface_ipv4(ifp->ifname, &self) == 0 && v == self) continue;
        if (hs4_seen(ifx, v)) continue;
        if (nhs4 >= (int)(sizeof(hs4) / sizeof(hs4[0]))) break;

        if (nexthop4(ifp, ifx, v, &eg) != BCM_E_NONE) continue;

        bcm_l3_host_t_init(&h);
        h.l3a_ip_addr = v;
        h.l3a_intf    = eg;
        rv = bcm_l3_host_add(l3_unit, &h);
        if (rv != BCM_E_NONE) continue;

        hs4[nhs4].ifx = ifx; hs4[nhs4].ip = v; nhs4++;
        st_host4++; added++;
        printf("l3: v4 host %u.%u.%u.%u dev %s -> egress %d\n",
               a, b, c, d, ifp->ifname, eg);
    }
    fclose(f);
    if (added) fflush(stdout);
}

/* Same for v6, straight off the NDP cache over netlink. */
static void poll_hosts6(void)
{
    struct { struct nlmsghdr n; struct ndmsg r; } req;
    uint8_t buf[16384];
    int s, len, added = 0;

    s = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (s < 0) return;
    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len   = NLMSG_LENGTH(sizeof(struct ndmsg));
    req.n.nlmsg_type  = RTM_GETNEIGH;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.n.nlmsg_seq   = 2;
    req.r.ndm_family  = AF_INET6;
    if (send(s, &req, req.n.nlmsg_len, 0) < 0) { close(s); return; }

    while ((len = recv(s, buf, sizeof(buf), 0)) > 0) {
        struct nlmsghdr *nh = (struct nlmsghdr *)buf;
        int done = 0;

        for (; NLMSG_OK(nh, (unsigned)len); nh = NLMSG_NEXT(nh, len)) {
            struct ndmsg  *nd;
            struct rtattr *rta;
            bcm_l3_host_t  h;
            struct l3if   *ifp = NULL;
            bcm_if_t       eg;
            const uint8_t *dst = NULL, *ll = NULL;
            char           name[IFNAMSIZ];
            int            rlen, ifx, i, rv;

            if (nh->nlmsg_type == NLMSG_DONE) { done = 1; break; }
            if (nh->nlmsg_type != RTM_NEWNEIGH) continue;
            nd = (struct ndmsg *)NLMSG_DATA(nh);
            if (nd->ndm_family != AF_INET6) continue;

            rta  = (struct rtattr *)((char *)nd + NLMSG_ALIGN(sizeof(*nd)));
            rlen = nh->nlmsg_len - NLMSG_LENGTH(sizeof(*nd));
            for (; RTA_OK(rta, rlen); rta = RTA_NEXT(rta, rlen)) {
                if (rta->rta_type == NDA_DST    && RTA_PAYLOAD(rta) == 16)
                    dst = RTA_DATA(rta);
                if (rta->rta_type == NDA_LLADDR && RTA_PAYLOAD(rta) == 6)
                    ll  = RTA_DATA(rta);
            }
            if (!dst || !ll) continue;
            if (is_mcast6(dst)) continue;
            if (!if_indextoname((unsigned)nd->ndm_ifindex, name)) continue;
            ifp = if_by_name(name);
            if (!ifp) continue;
            ifx = (int)(ifp - ifs);
            if (hs6_seen(ifx, dst)) continue;
            if (nhs6 >= (int)(sizeof(hs6) / sizeof(hs6[0]))) continue;
            if (nexthop6(ifp, ifx, dst, &eg) != BCM_E_NONE) continue;

            bcm_l3_host_t_init(&h);
            h.l3a_flags = BCM_L3_IP6;
            memcpy(h.l3a_ip6_addr, dst, 16);
            h.l3a_intf = eg;
            rv = bcm_l3_host_add(l3_unit, &h);
            if (rv != BCM_E_NONE) continue;

            memcpy(hs6[nhs6].ip, dst, 16); hs6[nhs6].ifx = ifx; nhs6++;
            st_host6++; added++;
            printf("l3: v6 host dev %s -> egress %d (", ifp->ifname, eg);
            for (i = 0; i < 16; i += 2)
                printf("%02x%02x%s", dst[i], dst[i + 1], i < 14 ? ":" : ")\n");
        }
        if (done) break;
    }
    close(s);
    if (added) fflush(stdout);
}

int l3sync_poll(void)
{
    int i;

    if (!l3_on) return 0;
    for (i = 0; i < nif; i++) { self_punt_try(&ifs[i]);
                                self_punt6_try(&ifs[i]); }
    poll_v4();
    poll_v6();
    poll_hosts4();
    poll_hosts6();
    return 0;
}

int l3sync_add_intf(int unit, const char *ifname, int port, int vlan,
                    const bcm_mac_t mac)
{
    bcm_l3_intf_t    intf;
    bcm_l2_station_t st;
    struct l3if     *ifp;
    int rv;

    if (nif >= MAX_IF) return -1;
    l3_unit = unit;

    /* Egress objects need "advanced egress management" turned on first.
     * Without it bcm_l3_egress_create returns BCM_E_DISABLED (-12), which is
     * exactly what we hit: the CPU egress object failed to create, so the
     * self-punt host entry could not be built and unicast to us was still
     * dropped by the routing engine. Idempotent, so set it once per call. */
    if (nif == 0) {
        rv = bcm_switch_control_set(unit, bcmSwitchL3EgressMode, 1);
        printf("l3: L3EgressMode=1 rv=%d\n", rv);
    }

    ifp = &ifs[nif];
    memset(ifp, 0, sizeof(*ifp));
    strncpy(ifp->ifname, ifname, IFNAMSIZ - 1);
    ifp->port   = port;
    ifp->vlan   = (bcm_vlan_t)vlan;
    ifp->intf   = -1;
    ifp->cpu_eg = -1;
    ifp->fp_self = -1;
    ifp->fp_stat_self = -1;
    memcpy(ifp->mac, mac, 6);

    bcm_l3_intf_t_init(&intf);
    memcpy(intf.l3a_mac_addr, mac, 6);
    intf.l3a_vid = ifp->vlan;
    intf.l3a_mtu = 1600;
    rv = bcm_l3_intf_create(unit, &intf);
    if (rv != BCM_E_NONE) {
        printf("l3: %s bcm_l3_intf_create rv=%d\n", ifname, rv);
        return -1;
    }
    ifp->intf = intf.l3a_intf_id;

    /* MY_STATION: without this the chip never routes frames sent to our MAC.
     * IPV6 is included so v6 terminates and routes as well as v4 -- without
     * that flag the chip treats an IPv6 frame to our MAC as pure L2. */
    bcm_l2_station_t_init(&st);
    memcpy(st.dst_mac, mac, 6);
    memset(st.dst_mac_mask, 0xff, 6);
    st.flags = BCM_L2_STATION_IPV4 | BCM_L2_STATION_IPV6 |
               BCM_L2_STATION_ARP_RARP;
    rv = bcm_l2_station_add(unit, &ifp->station, &st);
    if (rv != BCM_E_NONE) {
        printf("l3: %s bcm_l2_station_add rv=%d\n", ifname, rv);
        return -1;
    }

    /* MULTICAST MUST STILL TERMINATE, and only needs saying once.
     *
     * MY_STATION diverts frames addressed to our MAC into the routing engine,
     * which also means the bridge's static "our MAC -> CPU port" L2 entry no
     * longer gets a look in. EOS carries a SECOND station entry for exactly
     * this reason -- dumped from the running reference platform:
     *
     *   MY_STATION_TCAM[ 9]  MAC=01:00:5e:00:00:00 mask=ff:ff:ff:00:00:00
     *   MY_STATION_TCAM[10]  MAC=<router mac>      mask=ff:ff:ff:ff:ff:ff
     *
     * Without [9], OSPF's hellos to 224.0.0.5 (MAC 01:00:5e:00:00:05) stop
     * reaching the CPU: the bridge's receive count fell from ~52 frames to 2
     * and the adjacency stalled at ExStart. 33:33:.. is the v6 equivalent, for
     * OSPFv3's ff02::5/ff02::6 and for neighbour discovery. */
    if (nif == 0) {
        static const bcm_mac_t mc4  = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x00 };
        static const bcm_mac_t mc4m = { 0xff, 0xff, 0xff, 0x00, 0x00, 0x00 };
        static const bcm_mac_t mc6  = { 0x33, 0x33, 0x00, 0x00, 0x00, 0x00 };
        static const bcm_mac_t mc6m = { 0xff, 0xff, 0x00, 0x00, 0x00, 0x00 };
        bcm_l2_station_t mc;
        int id;

        bcm_l2_station_t_init(&mc);
        memcpy(mc.dst_mac, mc4, 6); memcpy(mc.dst_mac_mask, mc4m, 6);
        mc.flags = BCM_L2_STATION_IPV4 | BCM_L2_STATION_ARP_RARP;
        rv = bcm_l2_station_add(unit, &id, &mc);
        printf("l3: station 01:00:5e:00:00:00/24 (v4 mcast) id=%d rv=%d\n", id, rv);

        bcm_l2_station_t_init(&mc);
        memcpy(mc.dst_mac, mc6, 6); memcpy(mc.dst_mac_mask, mc6m, 6);
        mc.flags = BCM_L2_STATION_IPV6;
        rv = bcm_l2_station_add(unit, &id, &mc);
        printf("l3: station 33:33:00:00:00:00/16 (v6 mcast) id=%d rv=%d\n", id, rv);

        fp_setup();
    }

    nif++;
    l3_on = 1;
    printf("l3: %s intf %d (port %d, vlan %d, mtu 1600), my_station %d\n",
           ifname, ifp->intf, port, ifp->vlan, ifp->station);
    fflush(stdout);
    return 0;
}

void l3sync_stats(void)
{
    bcm_l3_info_t info;
    int i;

    if (!l3_on) return;
    printf("l3: v4 %lu routes / %d nh (moved %lu, gone %lu, failed %lu, unresolved %lu)   "
           "v6 %lu routes / %d nh (moved %lu, gone %lu, failed %lu, unresolved %lu)\n",
           st4_added, nnh4, st4_moved, st4_gone, st4_failed, st4_unresolved,
           st6_added, nnh6, st6_moved, st6_gone, st6_failed, st6_unresolved);
    printf("l3: host entries v4=%lu v6=%lu\n", st_host4, st_host6);

    fp_show("v4 ospf(proto 89)",     fp_stat_ospf4);
    fp_show("v6 ospfv3(next hdr 89)", fp_stat_ospf6);
    for (i = 0; i < nif; i++) {
        char b[48];
        snprintf(b, sizeof(b), "v4 self %s", ifs[i].ifname);
        fp_show(b, ifs[i].fp_stat_self);
    }

    /* Read the chip's own accounting rather than trusting our counters -- this
     * is what answers "are the routes actually IN the ASIC". */
    if (bcm_l3_info(l3_unit, &info) == BCM_E_NONE) {
        printf("l3: CHIP used_route=%d/%d  used_intf=%d/%d  used_host=%d/%d\n",
               info.l3info_used_route, info.l3info_max_route,
               info.l3info_used_intf,  info.l3info_max_intf,
               info.l3info_used_host,  info.l3info_max_host);
    }
    fflush(stdout);
}
