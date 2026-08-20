/*
 * TAP bridge: put hardware switch ports on the Linux network stack.
 *
 * This is the piece that lets an ordinary routing daemon (OSPF) run over ports
 * the SDK owns. Two directions, both plain:
 *
 *   wire -> Linux   a bcm_rx callback writes each received frame to /dev/net/tun
 *   Linux -> wire   a poll loop reads the tap devices and calls bcm_tx
 *
 * The kernel is built with CONFIG_TUN=y for exactly this (tools/kernel-7050.config);
 * the running #29 image had no TUN at all, which is why it was rebuilt.
 *
 * Enable with, in the environment of sdkpoc -- lists are comma separated and
 * are matched up by position, so the Nth name goes with the Nth port:
 *   SDKPOC_TAP=et1,et2          tap interface names
 *   SDKPOC_TAP_PORT=1,2         logical ports (xe0 = port 1, xe1 = port 2)
 *   SDKPOC_TAP_VLAN=1006,1007   dedicated VLAN per routed port (optional)
 *   SDKPOC_TAP_MAC=...,...      one MAC per tap (optional; defaults increment)
 *
 * WHY MORE THAN ONE PORT. With a single routed port the chip cannot be shown to
 * forward anything: the only test path is to send a packet back out the
 * interface it arrived on, and Trident2+ drops that by design
 * (bcmSwitchL3UcSameInterfaceDrop, which is not settable on this silicon --
 * bcm_switch_control_set returns BCM_E_UNAVAIL). Two ports remove the ambiguity
 * entirely: traffic in one and out the other, with the CPU counter flat.
 * See docs/HARDWARE-L3-WORKING-20260819.md.
 *
 * EACH PORT GETS ITS OWN MAC. EOS uses one router MAC across all routed ports,
 * which the chip is happy with, but it puts two Linux interfaces with the same
 * hardware address on different subnets and that makes the kernel's ARP
 * behaviour depend on arp_ignore/arp_announce. Distinct MACs cost one extra
 * MY_STATION entry each and avoid the question.
 *
 * NOTE ON RUNTS. bcm_tx rejects frames shorter than 60 bytes with
 * "Discarding tagged runt packet without higig header" -- observed on
 * 2026-08-18 when a 46-byte ARP was refused. The Linux stack will hand us short
 * frames, so they are zero-padded to 60 here rather than dropped.
 *
 * NOTE ON LINKSCAN. _tx_pkt_desc_add ANDs the transmit bitmap with
 * sop->lc_pbm_link, which only linkscan populates, and returns BCM_E_NONE
 * having built no descriptor when it is empty. Without linkscan running every
 * transmit here silently vanishes. bsh.sh prepends it; anything driving this
 * bridge must do the same.
 */
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <linux/if_tun.h>

#include <sal/types.h>
#include <bcm/types.h>
#include <bcm/error.h>
#include <bcm/rx.h>
#include <bcm/tx.h>
#include <bcm/pkt.h>
#include <bcm/link.h>
#include <bcm/l2.h>
#include <bcm/vlan.h>
#include <bcm/stg.h>
#include <bcm/port.h>
#include <soc/drv.h>

/* l3sync.c -- mirror the Linux FIB into the chip's DEFIP table */
extern int  l3sync_add_intf(int unit, const char *ifname, int port, int vlan,
                            const bcm_mac_t mac);
extern int  l3sync_poll(void);
extern void l3sync_stats(void);

/* ledsync.c -- front-panel port LEDs follow link state */
extern int  ledsync_start(int unit);
extern void ledsync_poll(void);

#define TAP_BUF   9216
#define TAP_MINPK 60
#define MAX_TAP   8

struct tapif {
    int        fd;
    int        port;
    bcm_vlan_t vlan;
    char       name[IFNAMSIZ];
    bcm_mac_t  mac;
};

static struct tapif taps[MAX_TAP];
static int  ntap;
static int  tap_unit = 0;

static unsigned long st_rx, st_rx_drop, st_rx_noport, st_tx, st_tx_err;

/* The bridge owns each tap's MAC. It must, because the same address is
 * installed in the chip's L2 table pointing at the CPU port -- if the kernel
 * picked a random one (which is the default for a tap device) the two would
 * disagree and every unicast reply would be dropped by the pipeline. */
static const bcm_mac_t tap_mac_base = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };

/* ONE transmit packet, allocated at start-up and reused for every frame.
 *
 * bcm_pkt_alloc per frame leaks: the BDE shim's salloc is a bump allocator with
 * no free, so bcm_pkt_free returns nothing to it. Observed in the field --
 * after ~2400 transmits the log filled with
 *   "bde: salloc(60, pkt alloc data) exhausted the 64 MB pool (used 64 MB)"
 * transmit stopped, and the OSPF adjacency fell from ExStart back to Init
 * because the far end stopped hearing our Hellos while we still heard its.
 * bcm_tx is synchronous here (no async flag, NULL cookie), and the transmit
 * loop is single threaded, so one buffer stays safe to reuse across ports. */
static bcm_pkt_t *tx_pkt = NULL;

static int tap_mac_parse(const char *s, bcm_mac_t out)
{
    unsigned int v[6];
    int i;

    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return -1;
    }
    for (i = 0; i < 6; i++) {
        out[i] = (uint8)v[i];
    }
    return 0;
}

static int tap_mac_apply(const char *ifname, const bcm_mac_t mac)
{
    struct ifreq ifr;
    int s, rv;

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
    memcpy(ifr.ifr_hwaddr.sa_data, mac, 6);
    rv = ioctl(s, SIOCSIFHWADDR, &ifr);
    close(s);
    return rv;
}

static int tap_open(const char *name)
{
    struct ifreq ifr;
    int fd = open("/dev/net/tun", O_RDWR);

    if (fd < 0) {
        printf("tap: open /dev/net/tun: %s (kernel needs CONFIG_TUN)\n",
               strerror(errno));
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        printf("tap: TUNSETIFF %s: %s\n", name, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* Which tap does a frame arriving on this hardware port belong to? */
static struct tapif *tap_by_port(int port)
{
    int i;

    for (i = 0; i < ntap; i++) {
        if (taps[i].port == port) return &taps[i];
    }
    return NULL;
}

/* wire -> Linux. Runs on the SDK's RX thread.
 *
 * pkt->rx_port is what makes more than one port possible: it names the ingress
 * port, so the frame can be written to the tap that owns it. Sending everything
 * to a single tap would work by accident for one port and silently cross the
 * wires for two. */
static bcm_rx_t tap_rx_cb(int unit, bcm_pkt_t *pkt, void *cookie)
{
    struct tapif *t;
    int len;

    if (pkt == NULL || pkt->blk_count < 1 || pkt->pkt_data[0].data == NULL) {
        return BCM_RX_NOT_HANDLED;
    }
    t = tap_by_port(pkt->rx_port);
    if (t == NULL || t->fd < 0) {
        st_rx_noport++;
        return BCM_RX_NOT_HANDLED;
    }
    len = pkt->tot_len ? pkt->tot_len : pkt->pkt_data[0].len;
    if (len > pkt->pkt_data[0].len) {
        len = pkt->pkt_data[0].len;         /* never read past the block */
    }
    if (len <= 0) {
        return BCM_RX_NOT_HANDLED;
    }
    {
        uint8 *d = pkt->pkt_data[0].data;
        uint8  flat[TAP_BUF];

        /* Punted frames arrive tagged; Linux wants what was on the wire. */
        if (len > 16 && d[12] == 0x81 && d[13] == 0x00 && len - 4 <= TAP_BUF) {
            memcpy(flat, d, 12);
            memcpy(flat + 12, d + 16, len - 16);
            len -= 4;
            d = flat;
        }
        if (write(t->fd, d, len) != len) {
            st_rx_drop++;
        } else {
            st_rx++;
        }
    }
    return BCM_RX_HANDLED;
}

/* Linux -> wire.
 *
 * THE PACKET MUST CARRY A VLAN TAG. The SDK's transmit path assumes a tag at
 * offset 12 and, because tx_upbmp marks the egress port untagged, strips four
 * bytes there on the way out. Handing it the untagged frame Linux produced
 * therefore deletes the ethertype and two payload bytes instead of a tag: the
 * far end saw our correct MAC addresses followed by "802.3, length 0" and
 * "ethertype IPv4, IP0 (invalid)" -- everything past the MAC header shifted by
 * four. The diag `tx` command warns about the same thing ("Untagged packet read
 * from file for tx"). So insert a tag here and let the chip take it back off.
 */
static int tap_tx_frame(struct tapif *t, uint8 *buf, int len)
{
    uint8 tagged[TAP_BUF + 4];
    bcm_pkt_t *pkt = tx_pkt;

    if (len >= 12 && !(buf[12] == 0x81 && buf[13] == 0x00)) {
        memcpy(tagged, buf, 12);                 /* DA + SA           */
        tagged[12] = 0x81; tagged[13] = 0x00;    /* TPID              */
        tagged[14] = (uint8)((t->vlan >> 8) & 0x0f);
        tagged[15] = (uint8)(t->vlan & 0xff);    /* prio 0, VID       */
        memcpy(tagged + 16, buf + 12, len - 12); /* original ethertype on */
        len += 4;
        buf = tagged;
    }
    if (len < TAP_MINPK) {                  /* see NOTE ON RUNTS */
        memset(buf + len, 0, TAP_MINPK - len);
        len = TAP_MINPK;
    }
    if (pkt == NULL || len > TAP_BUF) {
        return BCM_E_MEMORY;
    }
    memcpy(pkt->pkt_data[0].data, buf, len);
    pkt->pkt_data[0].len = len;
    pkt->pkt_len = len;
    pkt->tot_len = len;
    pkt->flags  |= BCM_TX_CRC_APPEND;
    BCM_PBMP_CLEAR(pkt->tx_pbmp);
    BCM_PBMP_PORT_ADD(pkt->tx_pbmp, t->port);
    BCM_PBMP_CLEAR(pkt->tx_upbmp);
    BCM_PBMP_PORT_ADD(pkt->tx_upbmp, t->port);   /* leave the wire untagged */

    return bcm_tx(tap_unit, pkt, NULL);
}

/* Everything one port needs in the chip: a dedicated VLAN, spanning tree
 * forwarding on it, and a static L2 entry sending our MAC to the CPU. */
static int tap_setup_port(int unit, struct tapif *t, const char *vlan_arg)
{
    int rv;

    rv = bcm_linkscan_mode_set(unit, t->port, BCM_LINKSCAN_MODE_HW);
    if (rv != BCM_E_NONE) {
        printf("tap: bcm_linkscan_mode_set port %d rv=%d\n", t->port, rv);
        return -1;
    }

    /* A DEDICATED VLAN FOR THE ROUTED PORT, matching EOS.
     *
     * EOS gives each routed port its own internal VLAN rather than leaving it
     * in VLAN 1 with every other port -- dumped from the running platform:
     *
     *   Unit Intf VRF Group VLAN    Source Mac     MTU
     *   1    1006  0    0    1006 44:4c:a8:eb:93:f7 1600    <- Et1
     *   1    1007  0    0    1007 44:4c:a8:eb:93:f7 1500
     *
     * This must happen BEFORE the port's untagged VLAN is read below, because
     * that value becomes both the L2 punt entry's VLAN and the tag the transmit
     * path inserts. */
    if (vlan_arg) {
        bcm_vlan_t vid = (bcm_vlan_t)atoi(vlan_arg);
        bcm_pbmp_t pbm, upbm;
        int rc, ra, rs;

        BCM_PBMP_CLEAR(pbm);
        BCM_PBMP_CLEAR(upbm);
        BCM_PBMP_PORT_ADD(pbm,  t->port);
        BCM_PBMP_PORT_ADD(pbm,  CMIC_PORT(unit));   /* CPU must be a member */
        BCM_PBMP_PORT_ADD(upbm, t->port);           /* untagged on the wire */

        rc = bcm_vlan_create(unit, vid);            /* BCM_E_EXISTS is fine */
        ra = bcm_vlan_port_add(unit, vid, pbm, upbm);
        rs = bcm_port_untagged_vlan_set(unit, t->port, vid);
        printf("tap: %s vlan %d create=%d port_add=%d pvid=%d (port %d + cpu %d)\n",
               t->name, vid, rc, ra, rs, t->port, CMIC_PORT(unit));

        /* Ports joining a freshly created VLAN take the default spanning tree
         * state, which is not forwarding, and everything is dropped silently.
         * VLAN 1 hides this because its ports already forward -- which is why
         * moving to a dedicated VLAN cut the bridge's receive count from 74 to
         * 2 with no other change. */
        {
            bcm_stg_t stg = 0;
            int rg = bcm_vlan_stg_get(unit, vid, &stg);
            int rp = bcm_stg_stp_set(unit, stg, t->port, BCM_STG_STP_FORWARD);
            int rq = bcm_stg_stp_set(unit, stg, CMIC_PORT(unit),
                                     BCM_STG_STP_FORWARD);
            printf("tap: %s vlan %d stg=%d stp_get=%d port=%d cpu=%d (FORWARD)\n",
                   t->name, vid, stg, rg, rp, rq);
        }
    }

    /* Unicast destined to us must be PUNTED, not just received. bcm_rx only
     * sees what the chip forwards to the CPU: multicast is punted, so OSPF and
     * IGMP arrived, but ARP and ping replies addressed to our MAC reached the
     * port (RPKT counted them) and the pipeline then dropped them, because
     * nothing said that MAC lives on the CPU port. A static L2 entry says it. */
    {
        bcm_l2_addr_t l2;
        bcm_vlan_t vid = 1;

        if (bcm_port_untagged_vlan_get(unit, t->port, &vid) != BCM_E_NONE) {
            vid = 1;
        }
        t->vlan = vid;
        bcm_l2_addr_t_init(&l2, t->mac, vid);
        l2.flags = BCM_L2_STATIC;
        l2.port  = CMIC_PORT(unit);
        rv = bcm_l2_addr_add(unit, &l2);
        printf("tap: %s L2 %02x:%02x:%02x:%02x:%02x:%02x vlan %d -> CPU port %d, rv=%d\n",
               t->name, t->mac[0], t->mac[1], t->mac[2], t->mac[3], t->mac[4],
               t->mac[5], vid, CMIC_PORT(unit), rv);
        if (rv != BCM_E_NONE) {
            return -1;
        }
    }
    return 0;
}

/* Split a comma separated list; returns the count and fills argv with pointers
 * into a private copy of the string. */
static int tap_split(const char *src, char *buf, size_t buflen, char **argv,
                     int maxargs)
{
    int n = 0;
    char *p;

    if (src == NULL) return 0;
    strncpy(buf, src, buflen - 1);
    buf[buflen - 1] = '\0';
    for (p = buf; *p && n < maxargs; ) {
        argv[n++] = p;
        p = strchr(p, ',');
        if (p == NULL) break;
        *p++ = '\0';
    }
    return n;
}

int tapbridge_start(int unit, const char *ifnames, int first_port)
{
    char  nbuf[256], pbuf[128], vbuf[128], mbuf[256];
    char *nv[MAX_TAP], *pv[MAX_TAP], *vv[MAX_TAP], *mv[MAX_TAP];
    int   nn, np, nvl, nm, i, rv;

    tap_unit = unit;

    nn  = tap_split(ifnames,                 nbuf, sizeof(nbuf), nv, MAX_TAP);
    np  = tap_split(getenv("SDKPOC_TAP_PORT"), pbuf, sizeof(pbuf), pv, MAX_TAP);
    nvl = tap_split(getenv("SDKPOC_TAP_VLAN"), vbuf, sizeof(vbuf), vv, MAX_TAP);
    nm  = tap_split(getenv("SDKPOC_TAP_MAC"),  mbuf, sizeof(mbuf), mv, MAX_TAP);

    if (nn == 0) {
        printf("tap: no interface names given\n");
        return -1;
    }

    /* LINKSCAN IS NOT OPTIONAL -- see the note at the top. The shell path gets
     * it from bsh.sh, but this path never reaches the shell, so enable it here
     * or every bcm_tx below silently builds no descriptor. */
    rv = bcm_linkscan_enable_set(unit, 250000);
    if (rv != BCM_E_NONE) {
        printf("tap: bcm_linkscan_enable_set rv=%d\n", rv);
        return -1;
    }

    for (i = 0; i < nn; i++) {
        struct tapif *t = &taps[ntap];

        memset(t, 0, sizeof(*t));
        strncpy(t->name, nv[i], IFNAMSIZ - 1);
        t->port = (i < np) ? atoi(pv[i]) : (first_port + i);

        memcpy(t->mac, tap_mac_base, 6);
        t->mac[5] = (uint8)(tap_mac_base[5] + i);   /* ...:55, ...:56, ... */
        if (i < nm && tap_mac_parse(mv[i], t->mac) != 0) {
            printf("tap: bad MAC '%s'\n", mv[i]);
            return -1;
        }

        t->fd = tap_open(t->name);
        if (t->fd < 0) return -1;
        if (tap_mac_apply(t->name, t->mac) != 0) {
            printf("tap: SIOCSIFHWADDR %s: %s\n", t->name, strerror(errno));
            return -1;
        }
        if (tap_setup_port(unit, t, (i < nvl) ? vv[i] : NULL) != 0) {
            return -1;
        }
        ntap++;
    }

    rv = bcm_pkt_alloc(unit, TAP_BUF + 8, BCM_TX_CRC_APPEND, &tx_pkt);
    if (rv != BCM_E_NONE || tx_pkt == NULL) {
        printf("tap: bcm_pkt_alloc rv=%d\n", rv);
        return -1;
    }

    rv = bcm_rx_register(unit, "tapbridge", tap_rx_cb, 100, NULL,
                         BCM_RCO_F_ALL_COS);
    if (rv != BCM_E_NONE) {
        printf("tap: bcm_rx_register rv=%d\n", rv);
        return -1;
    }
    if (!bcm_rx_active(unit)) {
        rv = bcm_rx_start(unit, NULL);
        if (rv != BCM_E_NONE) {
            printf("tap: bcm_rx_start rv=%d\n", rv);
            return -1;
        }
    }

    /* PORT LEDs. Nothing else on the system drives them, so without this they
     * hold whatever was last written -- which is how a pulled transceiver left
     * its LED lit. Not fatal if the SCD cannot be mapped; the bridge still
     * carries traffic, the panel is just wrong. */
    if (!getenv("SDKPOC_NO_LEDS")) {
        ledsync_start(unit);
    }

    /* Hardware L3 is opt-in: without it the bridge still carries a control
     * plane, but every routed packet crosses the CPU. Each port becomes its own
     * router interface -- that is what makes forwarding between them a chip
     * operation rather than a CPU one. */
    if (getenv("SDKPOC_TAP_L3")) {
        for (i = 0; i < ntap; i++) {
            l3sync_add_intf(unit, taps[i].name, taps[i].port, taps[i].vlan,
                            taps[i].mac);
        }
    }

    for (i = 0; i < ntap; i++) {
        printf("tap: %s <-> logical port %d, vlan %d, mac "
               "%02x:%02x:%02x:%02x:%02x:%02x\n",
               taps[i].name, taps[i].port, taps[i].vlan,
               taps[i].mac[0], taps[i].mac[1], taps[i].mac[2],
               taps[i].mac[3], taps[i].mac[4], taps[i].mac[5]);
    }
    printf("tap: %d port(s), rx active=%d\n", ntap, bcm_rx_active(unit));
    fflush(stdout);
    return 0;
}

/* Pump Linux -> wire until killed. Never returns. */
void tapbridge_run(void)
{
    uint8 buf[TAP_BUF];
    struct pollfd p[MAX_TAP];
    int i;

    /* THE FIB MIRROR MUST NOT DEPEND ON THE LINK BEING IDLE.
     *
     * This loop originally called l3sync_poll() only when poll() timed out, so
     * on a busy port -- exactly the case hardware L3 exists for -- new routes
     * would never be picked up at all. Drive it off the clock instead, so it
     * runs at a steady cadence no matter how much traffic is flowing. */
    time_t last_sync = 0, last_stat = 0;

    for (i = 0; i < ntap; i++) {
        p[i].fd     = taps[i].fd;
        p[i].events = POLLIN;
    }

    for (;;) {
        int n = poll(p, ntap, 5000);
        time_t now;

        now = time(NULL);
        if (now - last_sync >= 5) {
            last_sync = now;
            l3sync_poll();
            ledsync_poll();      /* cheap: reads link, writes only on change */
        }
        if (now - last_stat >= 30) {
            last_stat = now;
            printf("tap: rx %lu (drop %lu, noport %lu)  tx %lu (err %lu)\n",
                   st_rx, st_rx_drop, st_rx_noport, st_tx, st_tx_err);
            fflush(stdout);
            l3sync_stats();
        }

        if (n < 0) {
            if (errno == EINTR) continue;
            printf("tap: poll: %s\n", strerror(errno));
            return;
        }
        if (n == 0) continue;                /* idle tick: nothing else to do */

        for (i = 0; i < ntap; i++) {
            int len;

            if (!(p[i].revents & POLLIN)) continue;
            len = read(taps[i].fd, buf, sizeof(buf) - TAP_MINPK);
            if (len <= 0) continue;
            if (tap_tx_frame(&taps[i], buf, len) != BCM_E_NONE) {
                st_tx_err++;
            } else {
                st_tx++;
            }
        }
    }
}
