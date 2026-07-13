/*
 * bcmd.c — EdgeNOS-4610 datapath on the full OpenBCM SDK BCM C API.
 *
 * This file is the datapath body of the daemon. It is APPENDED to the SDK's
 * systems/linux/user/common/socdiag.c at build time (see build-bcmd.sh), and
 * socdiag's `diag_shell();` call is rewritten to `bcmd_run();`. We reuse
 * socdiag's main() (SAL init, knet_kcom_config, chip_info_vect_config,
 * bde_create + all the BDE/PCI platform hooks the libs need) and replace only
 * the interactive REPL with this deterministic datapath bring-up.
 *
 * WHY a daemon (vs hand-driving bcm.user): the diag proved the hardware works —
 * copper RX reaches the kernel, egress reaches the 54282 PHY clean (MAC+PHY
 * loopback FCS-clean) — but the diag could not reliably configure the CPU-punt
 * DMA path. Frames forwarded to the CPU port chip-side were not delivered to
 * the knet netdev (filter-2 got 3 hits vs RxAPI's 135k), so ping replies never
 * returned. See memory project_4610_fullsdk_rx_breakthrough.
 *
 * THE FIX: bcm_rx_start() with the RX DMA channel servicing ALL CPU CoS queues
 * (cos_bmp = all). Plus a static our-MAC->CPU L2 entry (so unicast replies are
 * L2-forwarded to CPU instead of bounced back out the wire — the diag let
 * dynamic learning move our MAC onto ge25) and a KNET netif+filter via the C
 * API (deterministic, unlike knetctrl).
 *
 * Run: on the box, CWD holds config.bcm, BDE + KNET modules loaded,
 * /dev/linux-bcm-knet present. socdiag's main reads config.bcm and inits SAL.
 */

#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_addr.h>
#include <linux/neighbour.h>

/* The SDK builder's <linux/i2c-dev.h> is the newer split header that lacks the
 * SMBus structs/constants, so we talk to the CPLD with raw i2c byte read/write
 * (I2C_SLAVE_FORCE + write()/read()). Define the one ioctl we need locally. */
#ifndef I2C_SLAVE_FORCE
#define I2C_SLAVE_FORCE 0x0706
#endif
#ifndef SIOCGIFFLAGS
#define SIOCGIFFLAGS 0x8913
#endif
#ifndef SIOCSIFFLAGS
#define SIOCSIFFLAGS 0x8914
#endif

#include <bcm/error.h>
#include <bcm/types.h>
#include <bcm/port.h>
#include <bcm/link.h>
#include <bcm/rx.h>
#include <bcm/knet.h>
#include <bcm/l2.h>
#include <bcm/l3.h>
#include <bcm/field.h>
#include <bcm/vlan.h>
#include <bcm/stat.h>
#include <bcm/switch.h>

/* ------------------------------------------------------------------ config */

#define BCMD_UNIT       0
#define BCMD_VLAN       1           /* default VLAN (front ports moved off it) */
#define BCMD_CPU_PORT   0           /* CMIC/CPU logical port */

/* Per-port VLAN: each front port lives in its OWN VLAN (broadcast domain) so the
 * chip ROUTES between ports (via the L3 tables) instead of L2-bridging them. This
 * stops one segment's broadcast (e.g. the copper 10.14.1 LAN) from flooding out
 * another port (e.g. the SFP+ uplink). VLAN id = 100 + logical port (avoids the
 * default VLAN 1); ports < 4000 so no overflow. */
#define bcmd_vlan_for_port(p) ((bcm_vlan_t)(100 + (p)))

/* EVERY front-panel ethernet port is auto-discovered (bcm_port_config_get -> .e)
 * and brought up as its own Linux netdev, named by the SDK port name (ge0..ge47,
 * xe0..xe5). No hardcoded table — the chip tells us the ports. The user assigns
 * IPs in Linux (`ip addr add ... dev ge25`), the SONiC/Cumulus model.
 *
 * NOTE: all ports default to VLAN 1, so the chip L2-bridges any ports that share
 * an external segment — keep different networks on different ports (and never two
 * ports on the mgmt 10.1.1 LAN) to avoid an L2 loop. Per-port VLAN isolation /
 * L3 comes with the netlink->chip sync layer. */
#define BCMD_MAXPORT    128         /* sizing for per-port netif-id array */

/* MAC surfaced on the netdevs (shared; per-port RX is disambiguated by the
 * ingress-port KNET filter, and per-port TX by each netif's TX_LOCAL_PORT). */
static const bcm_mac_t BCMD_HOST_MAC = {0x02, 0x10, 0x18, 0x96, 0x59, 0xdd};

static volatile sig_atomic_t bcmd_g_run = 1;
static volatile sig_atomic_t bcmd_l2_resync_req = 0;  /* SIGHUP: re-sync L2 groups */
static void bcmd_on_sig(int s)
{
    if (s == SIGHUP) bcmd_l2_resync_req = 1;   /* live L2-group re-sync, in main loop */
    else             bcmd_g_run = 0;           /* SIGINT/SIGTERM: shut down */
}

/* Per-port current L2-group VID (0 = default per-port isolation). */
static bcm_vlan_t bcmd_port_group[BCMD_MAXPORT];

/* Front-panel ethernet LOGICAL ports: copper ge0..ge47 = logical 1..48; SFP+
 * xe0..xe3 = 50..53; stacking xe4..xe5 = 54..55; logical 49 (ge48) is internal.
 * (We iterate this explicit set instead of the SDK pbmp macros, which don't
 * expand cleanly in this -ansi build.)
 * NOTE: the silkscreen-jack -> geN map is NOT sequential; it's scrambled
 * (verified on HW: physical port 1 = ge25, port 2 = ge24, pair-swapped). The
 * netdev names here are the chip-logical SDK names. See ../PORTMAP.md. */
static int bcmd_is_front(int p)
{
    return (p >= 1 && p <= 48) || (p >= 50 && p <= 55);
}

#define BCMD_CHK(expr) do {                                                    \
        int _rv = (expr);                                                      \
        if (_rv != BCM_E_NONE) {                                               \
            printf("[bcmd] FAIL %s = %d (%s)\n", #expr, _rv, bcm_errmsg(_rv)); \
            return _rv;                                                        \
        }                                                                      \
    } while (0)

/* --------------------------------------------------------------- chip init */

static int bcmd_chip_init(void)
{
    /* Probe + attach the SOC device via the BDE (bde_create runs inside). */
    if (sysconf_init() < 0)  { printf("[bcmd] sysconf_init failed\n");  return -1; }
    if (sysconf_probe() < 0) { printf("[bcmd] sysconf_probe failed\n"); return -1; }
    if (soc_ndev < 1)        { printf("[bcmd] no SOC devices\n");       return -1; }
    if (!soc_attached(BCMD_UNIT) && sysconf_attach(BCMD_UNIT) < 0) {
        printf("[bcmd] sysconf_attach failed\n"); return -1;
    }

    /* soc_init + bcm_init via the proven diag commands. These read config.bcm
     * (already in CWD), bring up the 56340 + 84758 firmware + QSGMII GE ports,
     * exactly as the working bcm.user sequence did. The command processor is
     * self-contained (static command table). */
    if (sh_process_command(BCMD_UNIT, "init all") != 0) {
        printf("[bcmd] 'init all' failed\n"); return -1;
    }
    if (sh_process_command(BCMD_UNIT, "init bcm") != 0) {
        printf("[bcmd] 'init bcm' failed\n"); return -1;
    }

    /* Program the chip LED microcontroller. Without this the M0 sits idle and the
     * external LED latches power up driving every port LED solid-on. Load the
     * open-source Helix4 GE-family reference LED program (OpenMDK board/xgsled/
     * sdk53344_ref.c, 216 bytes, Broadcom Switch-APIs license), enable
     * linkscan-driven auto updates (down/no-SFP ports -> off, link -> on,
     * activity -> blink), and start the M0. Cosmetic only; no forwarding impact.
     * Best-effort: a failure here must not abort datapath bring-up. */
    if (sh_process_command(BCMD_UNIT,
            "led prog "
            "021d2860e167bc06e190d21974020219902860e167bc06e1802860e167bc06e190"
            "90d20974100202802860e167bc06e1902860e167bc06e18080d20a7428021d2860"
            "e1679206e190d21974400219902860e1679206e1802860e1679206e19090d20974"
            "4e0202802860e1679206e1902860e1679206e18080d20a746612e08505d2027186"
            "520012e28505d203719052003a3832003201b79775a112e2fee102035012e2fee1"
            "9575ab8577ad77c806e167b575d077d412a0f8151a005732049771d432039771c8"
            "77d016e0da0171d477d0320f8757320e8757") != 0) {
        printf("[bcmd] WARN: 'led prog' failed (LEDs cosmetic; continuing)\n");
    }
    sh_process_command(BCMD_UNIT, "led auto on");
    sh_process_command(BCMD_UNIT, "led start");

    printf("[bcmd] chip attached + init all/bcm done\n");
    return 0;
}

/* --------------------------------------------------------------- port up */

/* Move a front port into its own VLAN: create it, add {port (untagged), CPU},
 * pull the port out of default VLAN 1, and set the port PVID. Result — the port
 * is its own L2 broadcast domain; cross-port traffic must be L3-routed. CPU is a
 * member so broadcast/ARP on the port still floods to the CPU for punt. */
static int bcmd_vlan_isolate(bcm_port_t port)
{
    bcm_vlan_t vlan = bcmd_vlan_for_port(port);
    bcm_pbmp_t pbmp, ubmp, rbmp;
    int rv;
    rv = bcm_vlan_create(BCMD_UNIT, vlan);
    if (rv != BCM_E_NONE && rv != BCM_E_EXISTS) {
        printf("[bcmd] vlan %d create rv=%d\n", vlan, rv);
        return rv;
    }
    BCM_PBMP_CLEAR(pbmp); BCM_PBMP_CLEAR(ubmp); BCM_PBMP_CLEAR(rbmp);
    BCM_PBMP_PORT_ADD(pbmp, port); BCM_PBMP_PORT_ADD(pbmp, BCMD_CPU_PORT);
    BCM_PBMP_PORT_ADD(ubmp, port);
    (void)bcm_vlan_port_add(BCMD_UNIT, vlan, pbmp, ubmp);
    BCM_PBMP_PORT_ADD(rbmp, port);
    (void)bcm_vlan_port_remove(BCMD_UNIT, BCMD_VLAN, rbmp);   /* off default VLAN 1 */
    (void)bcm_port_untagged_vlan_set(BCMD_UNIT, port, vlan);  /* PVID */
    return 0;
}

/* Resolve an l2-groups.conf token to a front bcm_port: accepts the SOC port name
 * ("ge1", "xe0") or a raw bcm port number. Returns -1 if unknown/not-front. */
static int bcmd_port_from_token(const char *tok)
{
    char *end;
    long num = strtol(tok, &end, 10);
    if (*end == '\0') {                     /* pure number */
        int p = (int)num;
        return (p >= 0 && p < BCMD_MAXPORT && bcmd_is_front(p)) ? p : -1;
    }
    for (int p = 0; p < BCMD_MAXPORT; p++)
        if (bcmd_is_front(p) && strcmp(SOC_PORT_NAME(BCMD_UNIT, p), tok) == 0)
            return p;
    return -1;
}

/* L2 switch group — the inverse of bcmd_vlan_isolate(): bridge the listed front
 * ports into one shared VLAN (untagged, PVID set), pulling each off its per-port
 * isolation VLAN (100+port) and default VLAN 1. The chip then L2-bridges them
 * directly (MAC learning + flood among members); CPU stays a member for punt.
 * Mirrors the 5610 edged vlan_l2_group_apply(). */
static int bcmd_l2_group_apply(bcm_vlan_t vlan, const bcm_port_t *ports, int n)
{
    bcm_pbmp_t pbmp, ubmp, rbmp;
    int i, rv;

    if (vlan < 2 || vlan > 4094) {
        printf("[bcmd] L2 group: invalid VID %d (must be 2-4094)\n", vlan);
        return -1;
    }
    rv = bcm_vlan_create(BCMD_UNIT, vlan);
    if (rv != BCM_E_NONE && rv != BCM_E_EXISTS) {
        printf("[bcmd] L2 group vlan %d create rv=%d\n", vlan, rv);
        return rv;
    }
    BCM_PBMP_CLEAR(pbmp); BCM_PBMP_CLEAR(ubmp);
    BCM_PBMP_PORT_ADD(pbmp, BCMD_CPU_PORT);            /* CPU member for flood/punt */
    for (i = 0; i < n; i++) {
        BCM_PBMP_PORT_ADD(pbmp, ports[i]);
        BCM_PBMP_PORT_ADD(ubmp, ports[i]);            /* untagged member */
    }
    (void)bcm_vlan_port_add(BCMD_UNIT, vlan, pbmp, ubmp);
    for (i = 0; i < n; i++) {
        BCM_PBMP_CLEAR(rbmp); BCM_PBMP_PORT_ADD(rbmp, ports[i]);
        (void)bcm_vlan_port_remove(BCMD_UNIT, bcmd_vlan_for_port(ports[i]), rbmp);
        (void)bcm_vlan_port_remove(BCMD_UNIT, BCMD_VLAN, rbmp);
        (void)bcm_port_untagged_vlan_set(BCMD_UNIT, ports[i], vlan);   /* PVID */
        bcmd_port_group[ports[i]] = vlan;                              /* track */
    }
    printf("[bcmd] L2 group: VLAN %d bridging %d ports\n", vlan, n);
    return 0;
}

/* Parse l2-groups.conf and apply each group. One group per line:
 *   <vid> <portA> <portB> [portC ...]   (ports as "ge1"/"xe0" or a number)
 * '#' comments and blank lines ignored. Missing file = 0 (not an error). */
static int bcmd_load_l2_groups(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int groups = 0;

    if (!f) { printf("[bcmd] L2 groups: no %s (none configured)\n", path); return 0; }
    while (fgets(line, sizeof(line), f)) {
        bcm_port_t ports[BCMD_MAXPORT];
        int np = 0, vid;
        char *s = line, *tok, *saveptr;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\0') continue;
        tok = strtok_r(s, " \t\n", &saveptr);
        if (!tok) continue;
        vid = atoi(tok);
        while ((tok = strtok_r(NULL, " \t\n", &saveptr)) && np < BCMD_MAXPORT) {
            int p = bcmd_port_from_token(tok);
            if (p < 0) { printf("[bcmd] L2 group %d: unknown port '%s'\n", vid, tok); continue; }
            ports[np++] = p;
        }
        if (vid >= 2 && np >= 1 && bcmd_l2_group_apply(vid, ports, np) == 0)
            groups++;
    }
    fclose(f);
    printf("[bcmd] L2 groups: applied %d from %s\n", groups, path);
    return groups;
}

/* Restore every grouped port to its per-port isolation VLAN (reverse of L2 group),
 * so an SIGHUP re-sync can rebuild groups cleanly from the config. */
static void bcmd_l2_reset(void)
{
    bcm_pbmp_t rbmp;
    int port, n = 0;
    for (port = 0; port < BCMD_MAXPORT; port++) {
        if (!bcmd_port_group[port]) continue;
        BCM_PBMP_CLEAR(rbmp); BCM_PBMP_PORT_ADD(rbmp, port);
        (void)bcm_vlan_port_remove(BCMD_UNIT, bcmd_port_group[port], rbmp);
        bcmd_port_group[port] = 0;
        (void)bcmd_vlan_isolate(port);     /* back to own VLAN (100+port) + PVID */
        n++;
    }
    if (n) printf("[bcmd] L2 groups: reset %d ports to isolation\n", n);
}

static int bcmd_port_up(bcm_port_t port)
{
    /* SW linkscan so the MAC link tracks the 54282 PHY (without it the MAC
     * stays down though the PHY has link — the diag needed `linkscan 250000`
     * + `port geN linkscan=on`). */
    BCMD_CHK(bcm_linkscan_enable_set(BCMD_UNIT, 250000));            /* 250ms */
    BCMD_CHK(bcm_linkscan_mode_set(BCMD_UNIT, port, BCM_LINKSCAN_MODE_SW));
    BCMD_CHK(bcm_port_enable_set(BCMD_UNIT, port, 1));
    (void)bcmd_vlan_isolate(port);                                  /* own VLAN */

    /* Disable flow control — the diag saw a continuous TX pause-frame storm
     * that backpressured the upstream trunk. */
    BCMD_CHK(bcm_port_pause_set(BCMD_UNIT, port, /*tx*/0, /*rx*/0));

    /* Raise the chip max frame size to jumbo so the KNET netdev allows MTU > 1500
     * (the KNET driver caps netdev max_mtu at the chip frame-max; default 1500
     * blocked `ip link set xeN mtu 1600` for a 1600-MTU OSPF peer). */
    (void)bcm_port_frame_max_set(BCMD_UNIT, port, 9216);

    printf("[bcmd] port %d up (linkscan SW, pause off, frame_max 9216)\n", port);
    return 0;
}

/* ------------------------------------------------------------ RX + CPU punt */
/*
 * THE FIX. bcm_rx_start() arms the CPU RX DMA. The diag's `pw start` armed it
 * too, but serviced only a subset of CPU CoS queues — frames L2-forwarded to
 * the CPU (unicast replies to our MAC) landed in an unserviced queue and were
 * never DMA'd up. Map one RX DMA channel to ALL CoS queues.
 */
static int bcmd_rx_start(void)
{
    bcm_rx_cfg_t cfg;

    bcm_rx_cfg_t_init(&cfg);
    cfg.pkt_size       = 9216;       /* jumbo headroom (matches KNET rx_buf) */
    cfg.pkts_per_chain = 16;         /* continuous chained ring */
    cfg.global_pps     = 0;
    cfg.max_burst      = 0;

    /* Channel 1 services every CPU CoS queue (the fix for the punt gap). */
    cfg.chan_cfg[1].chains  = 8;
    cfg.chan_cfg[1].cos_bmp = 0xffffffff;

    BCMD_CHK(bcm_rx_start(BCMD_UNIT, &cfg));
    printf("[bcmd] RX DMA armed (all CoS -> channel 1)\n");
    return 0;
}

/* ---------------------------------------------------------- KNET netdev */
/*
 * Create one kernel netdev PER port (TX egresses that port), plus a per-port
 * ingress-port filter routing that port's CPU-bound frames to its netdev, plus
 * a low-precedence catch-all so nothing falls to the consumer-less DefaultRxAPI
 * (which would wedge the RX DMA ring). All via the C API (not knetctrl).
 *
 * Filter precedence (linux-bcm-knet sorts by priority ascending, equal priority
 * keeps creation order): all at priority 0 ("no channel check"), created
 * ingress-port filters FIRST then the catch-all LAST -> per-port filters match
 * first, catch-all is the fallback. STRIP_TAG removes the chip PVID tag so Linux
 * sees clean untagged frames (without it the netdev gets a bogus ethertype).
 */
static int bcmd_knet_setup(int netif_ids[])
{
    bcm_knet_netif_t   netif;
    bcm_knet_filter_t  filter;
    int i, port, first = -1, n = 0;

    BCMD_CHK(bcm_knet_init(BCMD_UNIT));

    /* Idempotent: the kernel module persists netifs/filters across processes.
     * Tear down any stale ones by id (filter 1 = auto DefaultRxAPI; leave it). */
    for (i = 2; i <= BCMD_MAXPORT + 2; i++) (void)bcm_knet_filter_destroy(BCMD_UNIT, i);
    for (i = 1; i <= BCMD_MAXPORT + 2; i++) (void)bcm_knet_netif_destroy(BCMD_UNIT, i);

    /* one TX_LOCAL_PORT netdev per port, named by SDK port name (ge*,xe*).
     * Per-port failures (e.g. KNET resource limit) are logged, not fatal. */
    for (port = 0; port < BCMD_MAXPORT; port++) {
        const char *nm;
        int rv;
        if (!bcmd_is_front(port)) continue;
        nm = SOC_PORT_NAME(BCMD_UNIT, port);
        netif_ids[port] = 0;
        bcm_knet_netif_t_init(&netif);
        netif.type = BCM_KNET_NETIF_T_TX_LOCAL_PORT;   /* TX egresses this port */
        netif.port = port;
        netif.vlan = bcmd_vlan_for_port(port);
        sal_memcpy(netif.mac_addr, BCMD_HOST_MAC, sizeof(bcm_mac_t));
        if (nm && nm[0]) sal_strncpy(netif.name, nm, sizeof(netif.name) - 1);
        else snprintf(netif.name, sizeof(netif.name) - 1, "p%d", port);
        rv = bcm_knet_netif_create(BCMD_UNIT, &netif);
        if (rv != BCM_E_NONE) {
            printf("[bcmd] WARN netif create port %d (%s) failed: %s\n",
                   port, netif.name, bcm_errmsg(rv));
            continue;
        }
        netif_ids[port] = netif.id;
        if (first < 0) first = netif.id;
        n++;
    }
    printf("[bcmd] created %d port netdevs\n", n);

    /* per-port ingress-port filter (created first -> matched first), striptag */
    for (port = 0; port < BCMD_MAXPORT; port++) {
        if (!bcmd_is_front(port)) continue;
        if (!netif_ids[port]) continue;
        bcm_knet_filter_t_init(&filter);
        filter.type        = BCM_KNET_FILTER_T_RX_PKT;
        filter.priority    = 0;
        filter.dest_type   = BCM_KNET_DEST_T_NETIF;
        filter.dest_id     = netif_ids[port];
        filter.match_flags = BCM_KNET_FILTER_M_INGPORT;
        filter.m_ingport   = port;
        filter.flags       = BCM_KNET_FILTER_F_STRIP_TAG;
        sal_strncpy(filter.desc, SOC_PORT_NAME(BCMD_UNIT, port), sizeof(filter.desc) - 1);
        (void)bcm_knet_filter_create(BCMD_UNIT, &filter);
    }

    /* catch-all (created last -> matched last) -> first netdev, so nothing
     * accumulates on the consumer-less DefaultRxAPI and wedges the RX ring. */
    if (first >= 0) {
        bcm_knet_filter_t_init(&filter);
        filter.type      = BCM_KNET_FILTER_T_RX_PKT;
        filter.priority  = 0;
        filter.dest_type = BCM_KNET_DEST_T_NETIF;
        filter.dest_id   = first;
        filter.flags     = BCM_KNET_FILTER_F_STRIP_TAG;
        sal_strncpy(filter.desc, "catch-all", sizeof(filter.desc) - 1);
        BCMD_CHK(bcm_knet_filter_create(BCMD_UNIT, &filter));
        printf("[bcmd] knet per-port ingress filters + catch-all -> netif %d (striptag)\n", first);
    }
    return 0;
}

/* ---------------------------------------------------------- L2 return path */
/*
 * Static our-MAC -> CPU so unicast replies (ping echo-replies) are L2-forwarded
 * to the CPU port instead of bounced back out the wire. The diag let dynamic
 * learning move this MAC onto ge25; BCM_L2_STATIC prevents that.
 */
static int bcmd_l2_punt(void)
{
    bcm_l2_addr_t l2;
    int port, n = 0;

    /* Per-port VLAN now, so add the static our-MAC->CPU entry in EACH front
     * port's VLAN (else an L2-forwarded unicast to us on that VLAN would DLF-
     * flood out the wire instead of reaching the CPU). MY_STATION still handles
     * the L3 path; this covers the L2 path per broadcast domain. */
    for (port = 0; port < BCMD_MAXPORT; port++) {
        if (!bcmd_is_front(port)) continue;
        bcm_l2_addr_t_init(&l2, BCMD_HOST_MAC, bcmd_vlan_for_port(port));
        l2.flags |= BCM_L2_STATIC;
        l2.port   = BCMD_CPU_PORT;
        if (bcm_l2_addr_add(BCMD_UNIT, &l2) == BCM_E_NONE) n++;
    }
    printf("[bcmd] L2: host MAC -> CPU (static) in %d per-port VLANs\n", n);
    return 0;
}

/* ------------------------------------------------------- SFP+ 84758 laser */
/*
 * Enable the SFP+ optic TX laser + optical config on the BCM84758 over the chip
 * CMIC MIIM (external bus 2 = phy_id 0x40-0x43 for xe0-3; PHY_ID_BUS_NUM encodes
 * bus 2 as bit6). The OpenBCM SDK reaches the 84758 here (its "84758 init" uses
 * the same soc_miimc45 path) but never enables the laser, because the 84758 is
 * not chained as xe0's outer port PHY (the port PHY is the internal Warpcore), so
 * phy84740's enable_set never runs. We replicate edged's sfp_tx_enable() — the
 * ICOS LOCKED values (c800=0x3f3f laser, c8e4=0xc8ef, c805 optical, 10G PMA, PCS
 * un-reset, clear TX_DISABLE, TxOnOff strap toggle). 84758 ucode is already loaded
 * by the SDK init. devad 1=PMA/PMD, 7=AN, 3=PCS.
 */
extern int soc_miimc45_write(int unit, uint16 phy_id, uint8 devad,
                             uint16 reg, uint16 data);
extern int soc_miimc45_read(int unit, uint16 phy_id, uint8 devad,
                            uint16 reg, uint16 *data);

#define X84_OPT_CFG     0xc8e4
#define X84_OPT_SIGLVL  0xc800
#define X84_TX_DISABLE  0x0009
#define X84_GENSIG_8071 0xcd16
#define X84_AER_ADDR    0xc702
#define X84_RESET_CTRL  0xcd17
#define X84_SIDE_SEL    0xffff
#define X84_CHIP_MODE   0xc805
#define X84_REPEATER_DET 0xc81d
#define X84_SQUELCH_CTL 0xcd18
#define X84_EDC_MODE        0xca1a   /* EDC mode (media-RX adaptation); SR/LR = 0x44 */
#define X84_EDC_SR_LR       0x44
#define X84_EDC_MASK        0xff
#define X84_RXLOS_OVERRIDE  0xc0c0   /* c8e4 RX_LOS override bits */
#define X84_MODABS_OVERRIDE 0x0808   /* c8e4 MOD_ABS override bits */
#define X84_RXLOS_LVL_B9    (1u << 9)/* c800 RXLOS_LVL (signal-present) bit */

static void bcmd_x84_rmw(int pid, uint8 dev, uint16 reg, uint16 clr, uint16 set)
{
    uint16 v = 0;
    if (soc_miimc45_read(BCMD_UNIT, (uint16)pid, dev, reg, &v) == 0)
        soc_miimc45_write(BCMD_UNIT, (uint16)pid, dev, reg, (uint16)((v & ~clr) | set));
}

/* READ-ONLY 84758 status. The SDK's phy84740 driver (attached during `init all`
 * + bcm_port_enable, once the CPLD reset is cleared) owns the full datapath —
 * laser, PMA type, line/system PCS, retimer/squelch. The old manual register
 * surgery below (kept #if 0 for reference) was an OpenMDK-era workaround for
 * having NO SDK PHY driver; firing it ON TOP of the SDK clobbered the RX path
 * (forced LRM PMA, reset the media PCS, flipped side-select) — frames egressed
 * but never ingressed. We now only observe; the SDK has already set c800=0x3f3f. */
static void bcmd_sfp_laser_one(int pid)
{
    uint16 c800 = 0, optcfg = 0, pmd = 0;
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, X84_OPT_SIGLVL, &c800);
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, X84_OPT_CFG,    &optcfg);
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, 0x0001,         &pmd);   /* PMD status1 */
    printf("[bcmd] SFP+ 84758 phy_id 0x%02x (SDK-owned): c800=0x%04x c8e4=0x%04x "
           "PMD(1.1)=0x%04x rx_link=%d\n", pid, c800, optcfg, pmd, (pmd>>2)&1);
#if 0   /* OpenMDK-era manual datapath surgery — disabled (clobbered SDK RX path) */
    soc_miimc45_write(BCMD_UNIT, pid, 1, X84_AER_ADDR, 0);
    bcmd_x84_rmw(pid, 1, X84_SIDE_SEL, 0x1, 0);
    bcmd_x84_rmw(pid, 1, X84_CHIP_MODE, (1<<3)|(1<<2), 0);
    bcmd_x84_rmw(pid, 7, 0x0000, (1<<12), 0);
    soc_miimc45_write(BCMD_UNIT, pid, 1, 0x0096, 0);
    soc_miimc45_write(BCMD_UNIT, pid, 1, 0x0000, 0x2040);
    bcmd_x84_rmw(pid, 1, 0x0007, 0xf, 0x8);
    soc_miimc45_write(BCMD_UNIT, pid, 1, X84_RESET_CTRL, 0);
    soc_miimc45_write(BCMD_UNIT, pid, 1, X84_OPT_CFG, 0xc8ef);
    soc_miimc45_write(BCMD_UNIT, pid, 1, X84_OPT_SIGLVL, 0x3f3f);
    soc_miimc45_write(BCMD_UNIT, pid, 1, 0xc82b, 0x8a40);
    bcmd_x84_rmw(pid, 1, X84_TX_DISABLE, 0x1, 0);
    bcmd_x84_rmw(pid, 1, X84_GENSIG_8071, 0x8, 0);
    bcmd_x84_rmw(pid, 1, X84_OPT_CFG, (1<<12), 0);
#endif
}

/* Scan the MIIM phy_id space for the 84758 (C45 dev1 reg2 ID0 = 0x600d) so we use
 * the SDK's real bus-encoded phy_id (port_phy_addr 0x40 != the live phy_id). */
/* ROOT CAUSE of "84758 unreachable": EdgeNOS/ONL boots with the external front-
 * panel PHYs (84758 10G SFP+ + 54282 1G copper) HELD IN RESET via CPLD GPIOs
 * (0x19=0x7f, 0x1b=0xef power-on default). The SDK's `init all` programs the CMIC
 * MIIM master correctly but CANNOT clear a board-level CPLD reset, so MIIM to the
 * external PHYs returns 0xffff. ICOS clears 0x19/0x1b=0x00. We must do the same
 * BEFORE `init all` so the SDK's PHY probe finds + attaches the 84758 (firmware,
 * serdes, link bring-up). CPLD is i2c-0 @ 0x30; use SMBus byte-data (same as the
 * bound accton_as4610_cpld driver) + read-back, FORCE past the bound driver. */
static int bcmd_cpld_read(int fd, int reg)
{
    unsigned char b = (unsigned char)reg, v = 0xff;
    if (write(fd, &b, 1) != 1) return -1;
    if (read(fd, &v, 1)  != 1) return -1;
    return v & 0xff;
}

static void bcmd_cpld_write(int fd, int reg, int val)
{
    unsigned char buf[2];
    int t;
    buf[0] = (unsigned char)reg; buf[1] = (unsigned char)val;
    for (t = 0; t < 8; t++) {
        if (write(fd, buf, 2) == 2) {
            int rb = bcmd_cpld_read(fd, reg);
            if (rb < 0 || rb == (val & 0xff)) return;   /* ok (or write-only reg) */
        }
        usleep(3000);   /* let the bound driver's poll finish, then retry */
    }
    printf("[bcmd] CPLD write 0x%02x=0x%02x not confirmed (%s)\n",
           reg, val, strerror(errno));
}

static void bcmd_deassert_ext_phy_reset(void)
{
    int fd = open("/dev/i2c-0", O_RDWR);
    int r19, r1b;
    if (fd < 0) { printf("[bcmd] open /dev/i2c-0 failed: %s\n", strerror(errno)); return; }
    if (ioctl(fd, I2C_SLAVE_FORCE, 0x30) < 0) {
        printf("[bcmd] I2C_SLAVE_FORCE 0x30 failed: %s\n", strerror(errno));
        close(fd); return;
    }
    bcmd_cpld_write(fd, 0x07, 0x02); bcmd_cpld_write(fd, 0x08, 0x02);
    bcmd_cpld_write(fd, 0x0d, 0x01);
    bcmd_cpld_write(fd, 0x19, 0x00); bcmd_cpld_write(fd, 0x1b, 0x00);
    r19 = bcmd_cpld_read(fd, 0x19); r1b = bcmd_cpld_read(fd, 0x1b);
    printf("[bcmd] ext-PHY CPLD reset deasserted: 0x19=0x%02x 0x1b=0x%02x %s\n",
           r19 & 0xff, r1b & 0xff,
           (r19 == 0 && r1b == 0) ? "(OK)" : "(WARN held)");
    close(fd);
}

/* Replay ICOS's CMIC MIIM bus-map / select registers @0x48011000 (RE'd from a
 * live ICOS dump — see edged.c write_icos_miim_map / dumps/icos_mdio_regs.txt).
 * These route the external MDIO buses to the front-panel PHY address space. */
static void bcmd_write_icos_miim_map(void)
{
    static const struct { uint32 off, val; } regs[] = {
        { 0x004, 0x00010012 }, { 0x008, 0x30001000 }, { 0x010, 0x08210001 },
        { 0x058, 0x00F00000 }, { 0x060, 0xFFFFFFFE }, { 0x064, 0x1101FFFF },
        { 0x074, 0x09249000 }, { 0x078, 0x09249249 }, { 0x07c, 0x03249249 },
        { 0x080, 0x00092480 }, { 0x084, 0x00000002 },
        { 0x094, 0x04030201 }, { 0x098, 0x08070605 }, { 0x09c, 0x0E0D0C0B },
        { 0x0a0, 0x1211100F }, { 0x0a4, 0x18171615 }, { 0x0a8, 0x1C1B1A19 },
        { 0x0ac, 0x04030201 }, { 0x0b0, 0x08070605 }, { 0x0b4, 0x0E0D0C0B },
        { 0x0b8, 0x1211100F }, { 0x0bc, 0x18171615 },
    };
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    volatile uint32 *m;
    unsigned k;
    if (fd < 0) { printf("[bcmd] open /dev/mem failed: %s\n", strerror(errno)); return; }
    m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x48011000);
    close(fd);
    if (m == MAP_FAILED) { printf("[bcmd] mmap 0x48011000 failed\n"); return; }
    for (k = 0; k < sizeof(regs)/sizeof(regs[0]); k++)
        m[regs[k].off / 4] = regs[k].val;
    munmap((void *)m, 4096);
    printf("[bcmd] wrote %u ICOS CMIC MIIM bus-map regs @0x48011000\n", k);
}

/* Hand the external MDIO to the CMIC (clear iProc MDIO-select 0x1803fc3c bit4)
 * so the chip's CMIC MIIM owns the bus and soc_miimc45 can reach the 84758. */
static void bcmd_mdio_to_cmic(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    volatile uint32 *p;
    if (fd < 0) return;
    p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x1803f000);
    close(fd);
    if (p == MAP_FAILED) return;
    printf("[bcmd] iProc MDIO-sel 0x1803fc3c before=0x%08x\n", p[0xc3c/4]);
    p[0xc3c/4] &= ~(1u << 4);                       /* CMIC = MDIO master */
    printf("[bcmd] iProc MDIO-sel 0x1803fc3c after =0x%08x (bit4 cleared)\n", p[0xc3c/4]);
    munmap((void*)p, 4096);
}

static void bcmd_sfp_scan(void)
{
    int id, n = 0;
    uint16 v;
    bcmd_mdio_to_cmic();
    printf("[bcmd] MIIM scan for 84758 (ID0=0x600d) over phy_id 0x00-0xff:\n");
    for (id = 0; id < 0x100; id++) {
        v = 0xffff;
        if (soc_miimc45_read(BCMD_UNIT, (uint16)id, 1, 0x0002, &v) == 0 &&
            v != 0xffff && v != 0x0000) {
            uint16 v1 = 0; soc_miimc45_read(BCMD_UNIT, (uint16)id, 1, 0x0003, &v1);
            printf("   phy_id 0x%02x: ID=%04x:%04x%s%s\n", id, v, v1,
                   (v == 0x600d && (v1&~0xf)==0x86f0) ? "  <== BCM84758" : "",
                   (v == 0x600d && (v1&~0xf)==0x845b) ? "  <== BCM54282" : "");
            n++;
        }
    }
    if (!n) printf("   (nothing responded on any phy_id)\n");
}

/* Bring up the 84758 REPEATER datapath the SDK leaves down (the 84758 isn't xe0's
 * chained port PHY, so phy84740's datapath setup never runs). Ported from edged's
 * proven sfp_tx_enable steps (0b)+(5):
 *   (0b) clear media PCS reset 1.0xcd17 -> 10GBASE-R block-locks on the media side.
 *   (5)  if in repeater/retimer mode (1.0xc81d b1|b2=0x6), un-squelch media->system
 *        via SYSTEM-side 3.0xcd18 (b4 RX + b6 enable) so the locked media RX is
 *        repeated to the Warpcore. Without (5): rx_link=1 yet 0 frames reach the MAC.
 * Idempotent (re-asserting the squelch-enable is harmless); side-select is restored
 * to media(0) so the SDK's media-side view is unchanged. */
static void bcmd_sfp_datapath_up(int pid)
{
    uint16 rep = 0, lsd = 0, lblk = 0, lpmd = 0, cmode = 0, pma7 = 0;
    uint16 ssd = 0, sblk = 0, sq = 0;
    soc_miimc45_write(BCMD_UNIT, (uint16)pid, 1, X84_AER_ADDR, 0);   /* lane 0 */
    /* --- LINE (media/optic) side --- */
    soc_miimc45_write(BCMD_UNIT, (uint16)pid, 1, X84_SIDE_SEL, 0);
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, 0x000a, &lsd);       /* signal-detect */
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 3, 0x0020, &lblk);      /* BASE-R block lock */
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, 0x0001, &lpmd);      /* PMD status1 */
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, X84_CHIP_MODE, &cmode);
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, 0x0007, &pma7);      /* PMA type ctrl2 */
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, X84_REPEATER_DET, &rep);
    soc_miimc45_write(BCMD_UNIT, (uint16)pid, 1, X84_RESET_CTRL, 0); /* (0b) enable media PCS */
    /* --- SYSTEM (Warpcore-facing/XFI) side --- */
    soc_miimc45_write(BCMD_UNIT, (uint16)pid, 1, X84_SIDE_SEL, 1);
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, 0x000a, &ssd);
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 3, 0x0020, &sblk);
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 3, X84_SQUELCH_CTL, &sq);
    soc_miimc45_write(BCMD_UNIT, (uint16)pid, 3, X84_SQUELCH_CTL,    /* un-squelch sys datapath */
                      (uint16)(sq | (1u << 4) | (1u << 6)));
    soc_miimc45_write(BCMD_UNIT, (uint16)pid, 1, X84_SIDE_SEL, 0);   /* rest at media */
    printf("[bcmd] SFP+ 0x%02x LINE: sd=%04x blk=%04x pmd=%04x cmode=%04x pma7=%04x rep=%04x | "
           "SYS: sd=%04x blk=%04x sq=%04x->%04x\n",
           pid, lsd, lblk, lpmd, cmode, pma7, rep, ssd, sblk, sq, (uint16)(sq|(1u<<4)|(1u<<6)));
}

/* Force the 84758 to RE-ACQUIRE its media RX (line->system datapath). Ported from
 * edged sfp_edc_reacquire: the 84758 uC starts media-RX acquisition only on a
 * LOS->present EDGE, but bcmd holds RXLOS_LVL "signal present" statically, so the uC
 * never re-runs acquisition with the right EDC (equalizer) mode -> the media side
 * block-locks but never forwards clean frames to the system side (our RX=0 with TX
 * fine + far side actively sending). Sequence: drop the RX_LOS/MOD_ABS overrides,
 * induce a fake LOS (flip c800 b9), set EDC mode = SR/LR (0x44), remove the LOS,
 * restore the overrides -> the uC re-acquires the media RX. Per SFP+ phy. */
static void bcmd_sfp_edc_reacquire(int pid)
{
    uint16 v = 0, c800 = 0;
    soc_miimc45_write(BCMD_UNIT, (uint16)pid, 1, X84_AER_ADDR, 0);   /* lane 0 */
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, X84_OPT_CFG, &v);    /* drop overrides */
    soc_miimc45_write(BCMD_UNIT, (uint16)pid, 1, X84_OPT_CFG,
                      (uint16)(v & ~(X84_RXLOS_OVERRIDE | X84_MODABS_OVERRIDE)));
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, X84_OPT_SIGLVL, &c800);   /* induce LOS */
    soc_miimc45_write(BCMD_UNIT, (uint16)pid, 1, X84_OPT_SIGLVL,
                      (uint16)((c800 & ~X84_RXLOS_LVL_B9) | ((~c800) & X84_RXLOS_LVL_B9)));
    usleep(5000);
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, X84_EDC_MODE, &v);   /* EDC = SR/LR */
    soc_miimc45_write(BCMD_UNIT, (uint16)pid, 1, X84_EDC_MODE,
                      (uint16)((v & ~X84_EDC_MASK) | X84_EDC_SR_LR));
    usleep(5000);
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, X84_OPT_SIGLVL, &v); /* remove LOS */
    soc_miimc45_write(BCMD_UNIT, (uint16)pid, 1, X84_OPT_SIGLVL,
                      (uint16)((v & ~X84_RXLOS_LVL_B9) | (c800 & X84_RXLOS_LVL_B9)));
    soc_miimc45_read(BCMD_UNIT, (uint16)pid, 1, X84_OPT_CFG, &v);    /* restore overrides */
    soc_miimc45_write(BCMD_UNIT, (uint16)pid, 1, X84_OPT_CFG,
                      (uint16)(v | X84_RXLOS_OVERRIDE | X84_MODABS_OVERRIDE));
    printf("[bcmd] SFP+ phy 0x%02x EDC media-RX re-acquire (EDC mode -> 0x44 SR/LR)\n", pid);
}

static void bcmd_sfp_laser_all(void)
{
    int p;
    bcmd_sfp_scan();
    for (p = 0; p < 4; p++) bcmd_sfp_laser_one(0x40 + p);   /* xe0-3 = phy_id 0x40-0x43 */
    for (p = 0; p < 4; p++) bcmd_sfp_datapath_up(0x40 + p); /* un-squelch repeater datapath */
}

/* Log MAC link state + negotiated speed + chip RX/TX packet counters + the
 * 84758 10GBASE-R PCS block-lock, so we can see WHERE frames stop:
 *  - TX counters climbing but no RX  -> we egress, far side not replying / not
 *    decoding (PCS), or reply not ingressing.
 *  - RX climbing but netdev RX 0      -> ingress OK, CPU-punt/KNET filter wrong.
 *  - block_lock 0                     -> 10G PCS not aligned (no usable RX). */
static void bcmd_link_status(bcm_port_t port)
{
    int link = -1, speed = -1;
    uint64 rxu, rxnu, txu, txnu, rxe;
    uint16 pcs = 0xffff;
    bcm_port_link_status_get(BCMD_UNIT, port, &link);
    bcm_port_speed_get(BCMD_UNIT, port, &speed);
    COMPILER_64_ZERO(rxu); COMPILER_64_ZERO(rxnu);
    COMPILER_64_ZERO(txu); COMPILER_64_ZERO(txnu); COMPILER_64_ZERO(rxe);
    bcm_stat_get(BCMD_UNIT, port, snmpIfInUcastPkts,     &rxu);
    bcm_stat_get(BCMD_UNIT, port, snmpIfInNUcastPkts,    &rxnu);
    bcm_stat_get(BCMD_UNIT, port, snmpIfOutUcastPkts,    &txu);
    bcm_stat_get(BCMD_UNIT, port, snmpIfOutNUcastPkts,   &txnu);
    bcm_stat_get(BCMD_UNIT, port, snmpIfInErrors,        &rxe);
    /* 84758 10GBASE-R PCS status (devad 3 reg 0x0020): bit0 = rx_link/block_lock */
    soc_miimc45_read(BCMD_UNIT, 0x40, 3, 0x0020, &pcs);
    printf("[bcmd] port %d link=%s %dMb | RX uc=%u nuc=%u err=%u  TX uc=%u nuc=%u | "
           "84758 PCS(3.0020)=0x%04x block_lock=%d\n", port,
           link == BCM_PORT_LINK_STATUS_UP ? "UP" :
           link == BCM_PORT_LINK_STATUS_DOWN ? "down" : "?", speed,
           COMPILER_64_LO(rxu), COMPILER_64_LO(rxnu), COMPILER_64_LO(rxe),
           COMPILER_64_LO(txu), COMPILER_64_LO(txnu), pcs, pcs & 0x1);
}

/* ----------------------------------------------- netlink -> chip control */
/*
 * Make Linux tools drive the hardware: a NETLINK_ROUTE listener mirrors netdev
 * admin state to the chip port, so `ip link set ge25 up/down` enables/disables
 * the physical port. This is the first slice of the switchdev-style sync (and
 * the netlink foundation the L3 route/neigh sync will reuse). Integrated into
 * the wait loop via poll() — no extra thread.
 */
static struct { int ifindex; int port; } bcmd_ifmap[BCMD_MAXPORT];
static int bcmd_nifmap = 0;

static int bcmd_port_for_ifindex(int idx)
{
    int i;
    for (i = 0; i < bcmd_nifmap; i++)
        if (bcmd_ifmap[i].ifindex == idx) return bcmd_ifmap[i].port;
    return -1;
}

/* Map each port's netdev (named by SOC_PORT_NAME) to its kernel ifindex. */
static void bcmd_build_ifmap(void)
{
    int port;
    bcmd_nifmap = 0;
    for (port = 0; port < BCMD_MAXPORT; port++) {
        const char *nm;
        unsigned idx;
        if (!bcmd_is_front(port)) continue;
        nm = SOC_PORT_NAME(BCMD_UNIT, port);
        if (!nm || !nm[0]) continue;
        idx = if_nametoindex(nm);
        if (idx > 0 && bcmd_nifmap < BCMD_MAXPORT) {
            bcmd_ifmap[bcmd_nifmap].ifindex = (int)idx;
            bcmd_ifmap[bcmd_nifmap].port = port;
            bcmd_nifmap++;
        }
    }
    printf("[bcmd] netlink: mapped %d netdevs -> chip ports\n", bcmd_nifmap);
}

/* --------------------------------------------- netlink L3 -> chip (FIB sync) */
/*
 * Mirror the Linux L3 state into the ASIC so the chip forwards routed traffic in
 * hardware: RTM_NEWADDR -> L3 interface + MY_STATION (route the switch's MAC);
 * RTM_NEWNEIGH -> L3 egress next-hop + /32 host; RTM_NEWROUTE -> L3 route (via
 * the gateway's egress). Routes that OSPF/BGP install in the kernel FIB thus get
 * programmed into the chip L3 tables.
 */
static bcm_if_t bcmd_l3intf[BCMD_MAXPORT];     /* per-port egress L3 intf id */
static char     bcmd_l3intf_set[BCMD_MAXPORT]; /* 1 = bcmd_l3intf[port] valid (id 0 is legal) */
static int      bcmd_mystation_done = 0;

/* next-hop egress cache so a route can resolve its gateway -> egress object. */
static struct { int port; bcm_ip_t ip; bcm_if_t egr; } bcmd_nh[512];
static int bcmd_nnh = 0;

/* Returns the cached egress id, or -1 if not found (egress id 0 is legal). */
static bcm_if_t bcmd_nh_find(int port, bcm_ip_t ip)
{
    int i;
    for (i = 0; i < bcmd_nnh; i++)
        if (bcmd_nh[i].port == port && bcmd_nh[i].ip == ip) return bcmd_nh[i].egr;
    return -1;
}

/* Remove a cache entry (port,ip) and return its egress id, or -1 if absent.
 * Invalidates in place (port=-99 never matches) so the slot can be reused. */
static bcm_if_t bcmd_nh_take(int port, bcm_ip_t ip)
{
    int i;
    for (i = 0; i < bcmd_nnh; i++)
        if (bcmd_nh[i].port == port && bcmd_nh[i].ip == ip) {
            bcm_if_t e = bcmd_nh[i].egr;
            bcmd_nh[i].port = -99; bcmd_nh[i].ip = 0;
            return e;
        }
    return -1;
}

/* A cache slot to write: reuse an invalidated one (bounded under churn), else
 * extend the array; -1 if full. */
static int bcmd_nh_slot(void)
{
    int i;
    for (i = 0; i < bcmd_nnh; i++)
        if (bcmd_nh[i].port == -99) return i;
    if (bcmd_nnh < (int)(sizeof(bcmd_nh)/sizeof(bcmd_nh[0]))) return bcmd_nnh++;
    return -1;
}

/* Ensure the router MAC (BCMD_HOST_MAC) is in the MY_STATION TCAM so the chip
 * L3-routes (not just L2-switches) frames addressed to us. Added once. */
static void bcmd_l3_mystation(void)
{
    bcm_l2_station_t st;
    int sid = 0, i, rv;
    if (bcmd_mystation_done) return;
    /* Enable object-based L3 egress mode: bcm_l3_egress_create returns
     * BCM_E_DISABLED (-12) unless this is set. Must precede the first egress
     * object create. Idempotent; harmless if already on. */
    rv = bcm_switch_control_set(BCMD_UNIT, bcmSwitchL3EgressMode, 1);
    if (rv != BCM_E_NONE)
        printf("[bcmd] L3: bcmSwitchL3EgressMode set rv=%d\n", rv);
    /* Trap L3 unicast TTL=1 packets to the CPU. Control-plane protocols addressed
     * to us (OSPF/RIP DBD, etc.) ride IP TTL=1; once MY_STATION forces them into the
     * L3 route path, the chip decrements TTL 1->0 and DROPS them as TTL-expired
     * BEFORE the local-IP L2TOCPU punt — so OSPF unicast DBD never reaches the CPU
     * (ICMP works because TTL=64). This makes the chip punt them instead of drop. */
    rv = bcm_switch_control_set(BCMD_UNIT, bcmSwitchL3UcastTtl1ToCpu, 1);
    if (rv != BCM_E_NONE)
        printf("[bcmd] L3: bcmSwitchL3UcastTtl1ToCpu set rv=%d\n", rv);
    bcm_l2_station_t_init(&st);
    for (i = 0; i < 6; i++) { st.dst_mac[i] = BCMD_HOST_MAC[i]; st.dst_mac_mask[i] = 0xff; }
    st.vlan = 0; st.vlan_mask = 0;            /* any VLAN */
    st.flags = BCM_L2_STATION_IPV4;
    if (bcm_l2_station_add(BCMD_UNIT, &sid, &st) == BCM_E_NONE) {
        bcmd_mystation_done = 1;
        printf("[bcmd] L3: MY_STATION added (router MAC -> route IPv4)\n");
    }
}

/* Ensure a per-port egress L3 interface (source MAC = router MAC, VLAN). */
static bcm_if_t bcmd_l3_intf_ensure(int port, bcm_vlan_t vlan)
{
    bcm_l3_intf_t intf;
    int i;
    if (port < 0 || port >= BCMD_MAXPORT) return -1;
    if (bcmd_l3intf_set[port]) return bcmd_l3intf[port];
    bcmd_l3_mystation();
    bcm_l3_intf_t_init(&intf);
    intf.l3a_vid = vlan;
    for (i = 0; i < 6; i++) intf.l3a_mac_addr[i] = BCMD_HOST_MAC[i];
    if (bcm_l3_intf_create(BCMD_UNIT, &intf) != BCM_E_NONE) return -1;
    bcmd_l3intf[port] = intf.l3a_intf_id;
    bcmd_l3intf_set[port] = 1;
    printf("[bcmd] L3: intf id=%d on port %d (vlan %d)\n", intf.l3a_intf_id, port, vlan);
    return intf.l3a_intf_id;
}

/* ===========================================================================
 * IPv6 L3 — parallel to the v4 path above. The SDK does the table work; v6
 * differs only by the BCM_L3_IP6 flag + 16-byte address fields. The L3 egress
 * objects and interfaces are family-agnostic (MAC/port/VLAN), so v4 and v6 host
 * routes can share them, but we keep a separate v6 next-hop cache keyed by the
 * 16-byte gateway address.
 * ========================================================================= */
static struct rtattr *bcmd_rta(struct rtattr *rta, int len, int type);  /* fwd (netlink section) */

static struct { int port; bcm_ip6_t ip; bcm_if_t egr; } bcmd_nh6[512];
static int bcmd_nnh6 = 0;

static bcm_if_t bcmd_nh6_find(int port, const bcm_ip6_t ip)
{
    int i;
    for (i = 0; i < bcmd_nnh6; i++)
        if (bcmd_nh6[i].port == port && sal_memcmp(bcmd_nh6[i].ip, ip, 16) == 0)
            return bcmd_nh6[i].egr;
    return -1;
}
static bcm_if_t bcmd_nh6_take(int port, const bcm_ip6_t ip)
{
    int i;
    for (i = 0; i < bcmd_nnh6; i++)
        if (bcmd_nh6[i].port == port && sal_memcmp(bcmd_nh6[i].ip, ip, 16) == 0) {
            bcm_if_t e = bcmd_nh6[i].egr;
            bcmd_nh6[i].port = -99; sal_memset(bcmd_nh6[i].ip, 0, 16);
            return e;
        }
    return -1;
}
static int bcmd_nh6_slot(void)
{
    int i;
    for (i = 0; i < bcmd_nnh6; i++) if (bcmd_nh6[i].port == -99) return i;
    if (bcmd_nnh6 < (int)(sizeof(bcmd_nh6) / sizeof(bcmd_nh6[0]))) return bcmd_nnh6++;
    return -1;
}
/* prefix length -> 16-byte v6 mask */
static void bcmd_v6_mask(int len, bcm_ip6_t out)
{
    int i;
    for (i = 0; i < 16; i++) {
        int b = len - i * 8;
        out[i] = (b >= 8) ? 0xff : (b <= 0 ? 0 : (uint8)(0xff << (8 - b)));
    }
}
/* compact (uncompressed) v6 string for logs */
static const char *bcmd_ip6s(const bcm_ip6_t a, char *b)
{
    snprintf(b, 48, "%x:%x:%x:%x:%x:%x:%x:%x",
            (a[0]<<8)|a[1], (a[2]<<8)|a[3], (a[4]<<8)|a[5], (a[6]<<8)|a[7],
            (a[8]<<8)|a[9], (a[10]<<8)|a[11], (a[12]<<8)|a[13], (a[14]<<8)|a[15]);
    return b;
}

/* RTM_NEWADDR (switch's own IPv6): host entry that punts to the CPU. */
static void bcmd_l3_local_ip6(const bcm_ip6_t ip)
{
    bcm_l3_host_t host;
    char b[48];
    if (bcmd_nh6_find(-1, ip) >= 0) return;
    bcm_l3_host_t_init(&host);
    host.l3a_flags = BCM_L3_L2TOCPU | BCM_L3_IP6;
    sal_memcpy(host.l3a_ip6_addr, ip, 16);
    host.l3a_intf = 0;
    if (bcm_l3_host_add(BCMD_UNIT, &host) != BCM_E_NONE) return;
    { int s = bcmd_nh6_slot();
      if (s >= 0) { bcmd_nh6[s].port = -1; sal_memcpy(bcmd_nh6[s].ip, ip, 16); bcmd_nh6[s].egr = 0; } }
    printf("[bcmd] L3v6: local %s -> CPU (HW punt)\n", bcmd_ip6s(ip, b));
}

/* RTM_NEWNEIGH (v6): directly-connected host -> L3 egress next-hop + host route. */
static void bcmd_l3_neigh_add6(int port, const bcm_ip6_t ip, const unsigned char *mac)
{
    bcm_l3_egress_t egr;
    bcm_l3_host_t host;
    bcm_if_t intf, eid = 0;
    int i, rv;
    char b[48];
    if (bcmd_nh6_find(port, ip) >= 0) return;
    intf = bcmd_l3_intf_ensure(port, bcmd_vlan_for_port(port));
    if (intf < 0) return;
    bcm_l3_egress_t_init(&egr);
    egr.intf = intf;
    egr.vlan = bcmd_vlan_for_port(port);
    egr.port = port;
    for (i = 0; i < 6; i++) egr.mac_addr[i] = mac[i];
    rv = bcm_l3_egress_create(BCMD_UNIT, 0, &egr, &eid);
    if (rv != BCM_E_NONE) {
        printf("[bcmd] L3v6: neigh %s port %d: egress_create rv=%d\n", bcmd_ip6s(ip, b), port, rv);
        return;
    }
    bcm_l3_host_t_init(&host);
    host.l3a_flags = BCM_L3_IP6;
    sal_memcpy(host.l3a_ip6_addr, ip, 16);
    host.l3a_intf = eid;
    rv = bcm_l3_host_add(BCMD_UNIT, &host);
    if (rv != BCM_E_NONE)
        printf("[bcmd] L3v6: host_add %s rv=%d (egr %d still cached)\n", bcmd_ip6s(ip, b), rv, eid);
    { int s = bcmd_nh6_slot();
      if (s >= 0) { bcmd_nh6[s].port = port; sal_memcpy(bcmd_nh6[s].ip, ip, 16); bcmd_nh6[s].egr = eid; } }
    printf("[bcmd] L3v6: host %s -> egr %d port %d (HW)\n", bcmd_ip6s(ip, b), eid, port);
}

/* RTM_NEWROUTE (v6): prefix via gateway -> L3 route at the gateway egress. */
static void bcmd_l3_route_add6(const bcm_ip6_t prefix, int len, const bcm_ip6_t gw, int oif_port)
{
    bcm_l3_route_t rt;
    bcm_if_t eid;
    char b[48];
    eid = bcmd_nh6_find(oif_port, gw);
    if (eid < 0) return;                 /* gateway not resolved yet (deferred to re-dump) */
    bcm_l3_route_t_init(&rt);
    rt.l3a_flags = BCM_L3_IP6;
    sal_memcpy(rt.l3a_ip6_net, prefix, 16);
    bcmd_v6_mask(len, rt.l3a_ip6_mask);
    rt.l3a_intf = eid;
    if (bcm_l3_route_add(BCMD_UNIT, &rt) == BCM_E_NONE)
        printf("[bcmd] L3v6: route %s/%d -> egr %d (HW)\n", bcmd_ip6s(prefix, b), len, eid);
}

/* RTM_NEWROUTE (v6) multipath -> ECMP group across the resolved v6 gateways. */
static void bcmd_l3_route_add_ecmp6(const bcm_ip6_t prefix, int len, struct rtattr *mp, int mplen)
{
    bcm_if_t intfs[16];
    int n = 0;
    bcm_l3_egress_ecmp_t ecmp;
    bcm_l3_route_t rt;
    char b[48];
    struct rtnexthop *rtnh = (struct rtnexthop *)RTA_DATA(mp);
    int rem = mplen;
    while (RTNH_OK(rtnh, rem) && n < 16) {
        struct rtattr *gwa = bcmd_rta(RTNH_DATA(rtnh),
                                      (int)(rtnh->rtnh_len - sizeof(*rtnh)), RTA_GATEWAY);
        int port = bcmd_port_for_ifindex(rtnh->rtnh_ifindex);
        if (gwa && port >= 0) {
            bcm_if_t e = bcmd_nh6_find(port, (uint8 *)RTA_DATA(gwa));
            if (e >= 0) intfs[n++] = e;
        }
        rtnh = RTNH_NEXT(rtnh);
    }
    if (n < 2) return;                       /* <2 resolved paths: defer to re-dump */
    bcm_l3_egress_ecmp_t_init(&ecmp);
    ecmp.max_paths = n;
    if (bcm_l3_egress_ecmp_create(BCMD_UNIT, &ecmp, n, intfs) != BCM_E_NONE) return;
    bcm_l3_route_t_init(&rt);
    rt.l3a_flags = BCM_L3_IP6 | BCM_L3_MULTIPATH;
    sal_memcpy(rt.l3a_ip6_net, prefix, 16);
    bcmd_v6_mask(len, rt.l3a_ip6_mask);
    rt.l3a_intf = ecmp.ecmp_intf;
    if (bcm_l3_route_add(BCMD_UNIT, &rt) == BCM_E_NONE)
        printf("[bcmd] L3v6: route %s/%d -> ECMP %d paths (HW)\n", bcmd_ip6s(prefix, b), len, n);
}

/* RTM_NEWADDR (switch's own IP): /32 L3 host entry that punts to the CPU.
 * Required once MY_STATION is installed: MY_STATION makes the chip L3-route any
 * frame addressed to the router MAC instead of CPU-punting it, so without a
 * local-IP host entry the chip would L3-miss (and drop) our own inbound unicast
 * (ICMP replies, OSPF/BGP to us). BCM_L3_L2TOCPU sends matches to the CPU. */
static void bcmd_l3_local_ip(bcm_ip_t ip)
{
    bcm_l3_host_t host;
    if (bcmd_nh_find(-1, ip) >= 0) return;     /* already added (port -1 = local) */
    bcm_l3_host_t_init(&host);
    host.l3a_ip_addr = ip;
    host.l3a_flags   = BCM_L3_L2TOCPU;
    host.l3a_intf    = 0;
    if (bcm_l3_host_add(BCMD_UNIT, &host) != BCM_E_NONE) return;
    { int s = bcmd_nh_slot();
      if (s >= 0) { bcmd_nh[s].port = -1; bcmd_nh[s].ip = ip; bcmd_nh[s].egr = 0; } }
    printf("[bcmd] L3: local %u.%u.%u.%u -> CPU (HW punt)\n",
           (ip>>24)&0xff,(ip>>16)&0xff,(ip>>8)&0xff,ip&0xff);
}

/* RTM_NEWNEIGH: directly-connected host -> L3 egress next-hop + /32 host route. */
static void bcmd_l3_neigh_add(int port, bcm_ip_t ip, const unsigned char *mac)
{
    bcm_l3_egress_t egr;
    bcm_l3_host_t host;
    bcm_if_t intf, eid = 0;
    int i, rv;
    if (bcmd_nh_find(port, ip) >= 0) return;   /* already programmed (re-dump) */
    intf = bcmd_l3_intf_ensure(port, bcmd_vlan_for_port(port));
    if (intf < 0) { printf("[bcmd] L3: neigh %u.%u.%u.%u port %d: intf_ensure failed\n",
                        (ip>>24)&0xff,(ip>>16)&0xff,(ip>>8)&0xff,ip&0xff, port); return; }
    bcm_l3_egress_t_init(&egr);
    egr.intf = intf;
    egr.vlan = bcmd_vlan_for_port(port);
    egr.port = port;
    for (i = 0; i < 6; i++) egr.mac_addr[i] = mac[i];
    rv = bcm_l3_egress_create(BCMD_UNIT, 0, &egr, &eid);
    if (rv != BCM_E_NONE) {
        printf("[bcmd] L3: neigh %u.%u.%u.%u port %d: egress_create rv=%d\n",
               (ip>>24)&0xff,(ip>>16)&0xff,(ip>>8)&0xff,ip&0xff, port, rv);
        return;
    }
    bcm_l3_host_t_init(&host);
    host.l3a_ip_addr = ip;
    host.l3a_intf = eid;
    rv = bcm_l3_host_add(BCMD_UNIT, &host);
    if (rv != BCM_E_NONE)
        printf("[bcmd] L3: host_add %u.%u.%u.%u rv=%d (egr %d still cached)\n",
               (ip>>24)&0xff,(ip>>16)&0xff,(ip>>8)&0xff,ip&0xff, rv, eid);
    { int s = bcmd_nh_slot();
      if (s >= 0) { bcmd_nh[s].port = port; bcmd_nh[s].ip = ip; bcmd_nh[s].egr = eid; } }
    printf("[bcmd] L3: host %u.%u.%u.%u -> egr %d port %d (HW)\n",
           (ip>>24)&0xff,(ip>>16)&0xff,(ip>>8)&0xff,ip&0xff, eid, port);
}

/* RTM_NEWROUTE: prefix via gateway -> L3 route pointing at the gateway egress. */
static void bcmd_l3_route_add(bcm_ip_t prefix, int len, bcm_ip_t gw, int oif_port)
{
    bcm_l3_route_t rt;
    bcm_if_t eid;
    if (gw == 0) return;                 /* connected route: handled by intf */
    eid = bcmd_nh_find(oif_port, gw);
    if (eid < 0) return;                 /* gateway not resolved yet (deferred) */
    bcm_l3_route_t_init(&rt);
    rt.l3a_subnet  = prefix;
    rt.l3a_ip_mask = len ? (bcm_ip_t)(~0u << (32 - len)) : 0;
    rt.l3a_intf    = eid;
    if (bcm_l3_route_add(BCMD_UNIT, &rt) == BCM_E_NONE)
        printf("[bcmd] L3: route %u.%u.%u.%u/%d -> egr %d (HW)\n",
               (prefix>>24)&0xff,(prefix>>16)&0xff,(prefix>>8)&0xff,prefix&0xff, len, eid);
}

/* A single shared "glean" egress that traps to the CPU. Connected-subnet routes
 * point here so the FIRST packet to an as-yet-unresolved local host is sent to the
 * CPU, Linux ARPs it (-> RTM_NEWNEIGH -> a /32 host entry that longest-prefix wins),
 * and subsequent packets forward in hardware. Without it the chip has no entry for
 * un-ARP'd hosts on our directly-connected subnets and drops transit to them. */
static struct rtattr *bcmd_rta(struct rtattr *rta, int len, int type);  /* fwd (defined in netlink section) */

static bcm_if_t bcmd_glean_eid = -1;
static bcm_if_t bcmd_l3_glean_ensure(void)
{
    bcm_l3_egress_t egr;
    bcm_if_t eid = 0;
    if (bcmd_glean_eid >= 0) return bcmd_glean_eid;
    bcm_l3_egress_t_init(&egr);
    egr.flags = BCM_L3_L2TOCPU;          /* unresolved dest -> CPU (ARP trigger) */
    if (bcm_l3_egress_create(BCMD_UNIT, 0, &egr, &eid) != BCM_E_NONE) return -1;
    bcmd_glean_eid = eid;
    printf("[bcmd] L3: glean egress %d (connected-subnet ARP trap)\n", eid);
    return eid;
}

/* RTM_NEWROUTE, directly-connected subnet (no gateway, scope link): program the
 * subnet -> glean so unresolved local hosts get ARP'd then HW-forwarded. */
static void bcmd_l3_connected_add(bcm_ip_t prefix, int len, int oif_port)
{
    bcm_l3_route_t rt;
    bcm_if_t glean;
    if (len <= 0 || len >= 32) return;       /* subnets only (not /32 host routes) */
    glean = bcmd_l3_glean_ensure();
    if (glean < 0) return;
    (void)bcmd_l3_intf_ensure(oif_port, bcmd_vlan_for_port(oif_port));  /* intf + MY_STATION */
    bcm_l3_route_t_init(&rt);
    rt.l3a_subnet  = prefix;
    rt.l3a_ip_mask = (bcm_ip_t)(~0u << (32 - len));
    rt.l3a_intf    = glean;
    if (bcm_l3_route_add(BCMD_UNIT, &rt) == BCM_E_NONE)
        printf("[bcmd] L3: connected %u.%u.%u.%u/%d -> glean (HW)\n",
               (prefix>>24)&0xff,(prefix>>16)&0xff,(prefix>>8)&0xff,prefix&0xff, len);
}

/* RTM_NEWROUTE with RTA_MULTIPATH: build an ECMP group across the resolved
 * next-hops and point the route at it (BCM_L3_MULTIPATH) so the chip hashes flows
 * across the paths. mp = the RTA_MULTIPATH attribute, mplen = its payload length. */
static void bcmd_l3_route_add_ecmp(bcm_ip_t prefix, int len, struct rtattr *mp, int mplen)
{
    bcm_if_t intfs[16];
    int n = 0;
    bcm_l3_egress_ecmp_t ecmp;
    bcm_l3_route_t rt;
    struct rtnexthop *rtnh = (struct rtnexthop *)RTA_DATA(mp);
    int rem = mplen;
    while (RTNH_OK(rtnh, rem) && n < 16) {
        struct rtattr *gwa = bcmd_rta(RTNH_DATA(rtnh),
                                      (int)(rtnh->rtnh_len - sizeof(*rtnh)), RTA_GATEWAY);
        int port = bcmd_port_for_ifindex(rtnh->rtnh_ifindex);
        if (gwa && port >= 0) {
            bcm_if_t e = bcmd_nh_find(port, ntohl(*(unsigned int *)RTA_DATA(gwa)));
            if (e >= 0) intfs[n++] = e;
        }
        rtnh = RTNH_NEXT(rtnh);
    }
    if (n < 2) return;                       /* <2 resolved paths: defer to re-dump */
    bcm_l3_egress_ecmp_t_init(&ecmp);
    ecmp.max_paths = n;
    if (bcm_l3_egress_ecmp_create(BCMD_UNIT, &ecmp, n, intfs) != BCM_E_NONE) return;
    bcm_l3_route_t_init(&rt);
    rt.l3a_subnet  = prefix;
    rt.l3a_ip_mask = len ? (bcm_ip_t)(~0u << (32 - len)) : 0;
    rt.l3a_intf    = ecmp.ecmp_intf;
    rt.l3a_flags   = BCM_L3_MULTIPATH;
    if (bcm_l3_route_add(BCMD_UNIT, &rt) == BCM_E_NONE)
        printf("[bcmd] L3: route %u.%u.%u.%u/%d -> ECMP %d paths (HW)\n",
               (prefix>>24)&0xff,(prefix>>16)&0xff,(prefix>>8)&0xff,prefix&0xff, len, n);
}

/* ------ delete path: keep the chip L3 tables a true mirror of the kernel FIB ---- */

/* RTM_DELADDR (switch's own IP removed): drop the /32 CPU-punt host. */
static void bcmd_l3_local_ip_del(bcm_ip_t ip)
{
    bcm_l3_host_t host;
    if (bcmd_nh_take(-1, ip) < 0) return;      /* not one of ours */
    bcm_l3_host_t_init(&host);
    host.l3a_ip_addr = ip;
    host.l3a_flags   = BCM_L3_L2TOCPU;
    (void)bcm_l3_host_delete(BCMD_UNIT, &host);
    printf("[bcmd] L3: del local %u.%u.%u.%u\n",
           (ip>>24)&0xff,(ip>>16)&0xff,(ip>>8)&0xff,ip&0xff);
}

/* RTM_DELNEIGH (neighbor removed): drop the /32 host + free the egress object. */
static void bcmd_l3_neigh_del(int port, bcm_ip_t ip)
{
    bcm_l3_host_t host;
    bcm_if_t eid = bcmd_nh_take(port, ip);
    if (eid < 0) return;                       /* not programmed / already gone */
    bcm_l3_host_t_init(&host);
    host.l3a_ip_addr = ip;
    (void)bcm_l3_host_delete(BCMD_UNIT, &host);
    (void)bcm_l3_egress_destroy(BCMD_UNIT, eid);   /* best-effort (a route may pin it) */
    printf("[bcmd] L3: del host %u.%u.%u.%u (egr %d) port %d\n",
           (ip>>24)&0xff,(ip>>16)&0xff,(ip>>8)&0xff,ip&0xff, eid, port);
}

/* RTM_DELROUTE (route removed): drop the L3 route entry. Deletes by prefix/mask,
 * so it withdraws gateway, connected-glean, and ECMP routes alike. (The shared
 * glean egress is left in place for other connected subnets; per-route ECMP
 * groups are a minor leak across churn — acceptable, like the neigh egress.) */
static void bcmd_l3_route_del(bcm_ip_t prefix, int len)
{
    bcm_l3_route_t rt;
    bcm_l3_route_t_init(&rt);
    rt.l3a_subnet  = prefix;
    rt.l3a_ip_mask = len ? (bcm_ip_t)(~0u << (32 - len)) : 0;
    if (bcm_l3_route_delete(BCMD_UNIT, &rt) == BCM_E_NONE)
        printf("[bcmd] L3: del route %u.%u.%u.%u/%d\n",
               (prefix>>24)&0xff,(prefix>>16)&0xff,(prefix>>8)&0xff,prefix&0xff, len);
}

/* ---- IPv6 delete path ---- */
static void bcmd_l3_local_ip6_del(const bcm_ip6_t ip)
{
    bcm_l3_host_t host;
    if (bcmd_nh6_take(-1, ip) < 0) return;
    bcm_l3_host_t_init(&host);
    host.l3a_flags = BCM_L3_L2TOCPU | BCM_L3_IP6;
    sal_memcpy(host.l3a_ip6_addr, ip, 16);
    (void)bcm_l3_host_delete(BCMD_UNIT, &host);
}
static void bcmd_l3_neigh_del6(int port, const bcm_ip6_t ip)
{
    bcm_l3_host_t host;
    bcm_if_t eid = bcmd_nh6_take(port, ip);
    char b[48];
    if (eid < 0) return;
    bcm_l3_host_t_init(&host);
    host.l3a_flags = BCM_L3_IP6;
    sal_memcpy(host.l3a_ip6_addr, ip, 16);
    (void)bcm_l3_host_delete(BCMD_UNIT, &host);
    (void)bcm_l3_egress_destroy(BCMD_UNIT, eid);
    printf("[bcmd] L3v6: del host %s (egr %d) port %d\n", bcmd_ip6s(ip, b), eid, port);
}
static void bcmd_l3_route_del6(const bcm_ip6_t prefix, int len)
{
    bcm_l3_route_t rt;
    bcm_l3_route_t_init(&rt);
    rt.l3a_flags = BCM_L3_IP6;
    sal_memcpy(rt.l3a_ip6_net, prefix, 16);
    bcmd_v6_mask(len, rt.l3a_ip6_mask);
    (void)bcm_l3_route_delete(BCMD_UNIT, &rt);
}

/* Set a netdev admin-UP via ioctl, so its admin state matches the chip-enabled
 * state at startup. Without this, the netdevs start admin-DOWN while the chip
 * ports are enabled; carrier-change RTM_NEWLINK events (IFF_UP clear) would then
 * make the listener wrongly disable the chip port. Done BEFORE the socket opens
 * so these events aren't fed back. */
static void bcmd_netdev_up(const char *name)
{
    int s;
    struct ifreq ifr;
    if (!name || !name[0]) return;
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    memset(&ifr, 0, sizeof(ifr));
    sal_strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0 && !(ifr.ifr_flags & IFF_UP)) {
        ifr.ifr_flags |= IFF_UP;
        (void)ioctl(s, SIOCSIFFLAGS, &ifr);
    }
    close(s);
}

static int bcmd_nl_open(void)
{
    int fd;
    struct sockaddr_nl sa;
    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) { printf("[bcmd] netlink socket: %s\n", strerror(errno)); return -1; }
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    /* link admin + IPv4 addr/neigh/route — the full L3 FIB mirror. */
    sa.nl_groups = RTMGRP_LINK | RTMGRP_NEIGH |
                   RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE |
                   RTMGRP_IPV6_IFADDR | RTMGRP_IPV6_ROUTE;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        printf("[bcmd] netlink bind: %s\n", strerror(errno)); close(fd); return -1;
    }
    return fd;
}

/* Ask the kernel to dump current addrs, neighbors, and routes so we program the
 * existing FIB (not just live changes). Responses arrive on the socket and are
 * handled by bcmd_nl_process. Order addr->neigh->route so gateways resolve. */
static void bcmd_nl_drain(int fd);   /* fwd: consume a dump fully before the next */

static void bcmd_nl_dump(int fd, int type, int family)
{
    struct { struct nlmsghdr nh; struct rtgenmsg g; } req;
    struct sockaddr_nl sa;
    memset(&req, 0, sizeof(req));
    req.nh.nlmsg_len = sizeof(req);
    req.nh.nlmsg_type = type;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.g.rtgen_family = family;
    memset(&sa, 0, sizeof(sa)); sa.nl_family = AF_NETLINK;
    (void)sendto(fd, &req, sizeof(req), 0, (struct sockaddr *)&sa, sizeof(sa));
    /* Netlink allows only one dump in flight per socket — drain this one fully
     * (until NLMSG_DONE) before the caller issues the next, else it gets EBUSY. */
    bcmd_nl_drain(fd);
}

static struct rtattr *bcmd_rta(struct rtattr *rta, int len, int type)
{
    for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len))
        if (rta->rta_type == type) return rta;
    return NULL;
}

/* Apply one netlink message to the chip (link admin + L3 FIB). */
static void bcmd_nl_handle(struct nlmsghdr *nh)
{
    if (nh->nlmsg_type == RTM_NEWLINK) {
        struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nh);
        int port = bcmd_port_for_ifindex(ifi->ifi_index), want, cur = -1;
        if (port < 0) return;
        want = (ifi->ifi_flags & IFF_UP) ? 1 : 0;
        bcm_port_enable_get(BCMD_UNIT, port, &cur);
        if (cur != want) {
            bcm_port_enable_set(BCMD_UNIT, port, want);
            printf("[bcmd] netlink: %s -> chip port %d admin %s\n",
                   SOC_PORT_NAME(BCMD_UNIT, port), port, want ? "UP" : "DOWN");
        }

    } else if (nh->nlmsg_type == RTM_NEWADDR || nh->nlmsg_type == RTM_DELADDR) {
        struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nh);
        int port = bcmd_port_for_ifindex(ifa->ifa_index), alen;
        struct rtattr *local;
        /* Loopback (always ifindex 1) carries the router's own IPs (OSPF loopback
         * etc.). Program those as CPU-punt local hosts too, so the chip doesn't drop
         * v4 destined to them (v6 survives via the L3-miss trap; v4 does not). The
         * L3-interface step is front-port only — loopback needs just the host punt. */
        int is_lo = (ifa->ifa_index == 1);
        if (port < 0 && !is_lo) return;
        if (ifa->ifa_family != AF_INET && ifa->ifa_family != AF_INET6) return;
        alen = (int)(nh->nlmsg_len - NLMSG_LENGTH(sizeof(*ifa)));
        local = bcmd_rta((struct rtattr *)IFA_RTA(ifa), alen, IFA_LOCAL);
        if (!local)
            local = bcmd_rta((struct rtattr *)IFA_RTA(ifa), alen, IFA_ADDRESS);
        if (!local) return;
        if (ifa->ifa_family == AF_INET6) {
            bcm_ip6_t ip6; sal_memcpy(ip6, RTA_DATA(local), 16);
            if (nh->nlmsg_type == RTM_NEWADDR) {
                if (port >= 0) (void)bcmd_l3_intf_ensure(port, bcmd_vlan_for_port(port));
                bcmd_l3_local_ip6(ip6);
            } else {
                bcmd_l3_local_ip6_del(ip6);
            }
        } else {
            bcm_ip_t ip = ntohl(*(unsigned int *)RTA_DATA(local));
            if (nh->nlmsg_type == RTM_NEWADDR) {
                if (port >= 0) (void)bcmd_l3_intf_ensure(port, bcmd_vlan_for_port(port));  /* switch IP -> L3 intf */
                bcmd_l3_local_ip(ip);
            } else {
                bcmd_l3_local_ip_del(ip);              /* leave the L3 intf in place */
            }
        }

    } else if (nh->nlmsg_type == RTM_NEWNEIGH || nh->nlmsg_type == RTM_DELNEIGH) {
        struct ndmsg *nd = (struct ndmsg *)NLMSG_DATA(nh);
        struct rtattr *dst, *lla;
        int port = bcmd_port_for_ifindex(nd->ndm_ifindex);
        if (port < 0) return;
        if (nd->ndm_family != AF_INET && nd->ndm_family != AF_INET6) return;
        dst = bcmd_rta((struct rtattr *)((char *)nd + NLMSG_ALIGN(sizeof(*nd))),
                       (int)(nh->nlmsg_len - NLMSG_LENGTH(sizeof(*nd))), NDA_DST);
        lla = bcmd_rta((struct rtattr *)((char *)nd + NLMSG_ALIGN(sizeof(*nd))),
                       (int)(nh->nlmsg_len - NLMSG_LENGTH(sizeof(*nd))), NDA_LLADDR);
        if (!dst) return;
        if (nd->ndm_family == AF_INET6) {
            bcm_ip6_t ip6; sal_memcpy(ip6, RTA_DATA(dst), 16);
            if (nh->nlmsg_type == RTM_NEWNEIGH) {
                if (!(nd->ndm_state & (NUD_REACHABLE|NUD_PERMANENT|NUD_STALE|NUD_DELAY|NUD_PROBE)))
                    return;
                if (lla) bcmd_l3_neigh_add6(port, ip6, (unsigned char *)RTA_DATA(lla));
            } else {
                bcmd_l3_neigh_del6(port, ip6);
            }
        } else if (nh->nlmsg_type == RTM_NEWNEIGH) {
            if (!(nd->ndm_state & (NUD_REACHABLE|NUD_PERMANENT|NUD_STALE|NUD_DELAY|NUD_PROBE)))
                return;                                /* skip FAILED/INCOMPLETE */
            if (lla)
                bcmd_l3_neigh_add(port, ntohl(*(unsigned int *)RTA_DATA(dst)),
                                  (unsigned char *)RTA_DATA(lla));
        } else {
            bcmd_l3_neigh_del(port, ntohl(*(unsigned int *)RTA_DATA(dst)));
        }

    } else if (nh->nlmsg_type == RTM_NEWROUTE || nh->nlmsg_type == RTM_DELROUTE) {
        struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(nh);
        struct rtattr *base, *dst, *gw, *oif, *mp;
        int alen, port, len = rtm->rtm_dst_len;
        int is6 = (rtm->rtm_family == AF_INET6);
        bcm_ip_t pfx = 0;
        if (rtm->rtm_family != AF_INET && !is6) return;
        if (rtm->rtm_table != RT_TABLE_MAIN && rtm->rtm_table != 0) return;
        if (rtm->rtm_type != RTN_UNICAST) return;      /* skip local/broadcast/etc */
        base = (struct rtattr *)((char *)rtm + NLMSG_ALIGN(sizeof(*rtm)));
        alen = (int)(nh->nlmsg_len - NLMSG_LENGTH(sizeof(*rtm)));
        dst = bcmd_rta(base, alen, RTA_DST);
        gw  = bcmd_rta(base, alen, RTA_GATEWAY);
        oif = bcmd_rta(base, alen, RTA_OIF);
        mp  = bcmd_rta(base, alen, RTA_MULTIPATH);
        if (is6) {
            bcm_ip6_t pfx6, gw6;
            sal_memset(pfx6, 0, 16);
            if (dst) sal_memcpy(pfx6, RTA_DATA(dst), 16);
            if (nh->nlmsg_type == RTM_DELROUTE) { bcmd_l3_route_del6(pfx6, len); return; }
            if (mp) {
                bcmd_l3_route_add_ecmp6(pfx6, len, mp, (int)RTA_PAYLOAD(mp));
            } else if (gw && oif) {
                port = bcmd_port_for_ifindex(*(int *)RTA_DATA(oif));
                sal_memcpy(gw6, RTA_DATA(gw), 16);
                if (port >= 0) bcmd_l3_route_add6(pfx6, len, gw6, port);
            }
            /* v6 directly-connected glean: not yet — kernel ND resolves on-link
             * hosts and the resulting RTM_NEWNEIGH programs a /128 host route. */
            return;
        }
        if (dst) pfx = ntohl(*(unsigned int *)RTA_DATA(dst));
        if (nh->nlmsg_type == RTM_DELROUTE) { bcmd_l3_route_del(pfx, len); return; }
        /* NEWROUTE: ECMP (multipath) > single-gateway > directly-connected glean */
        if (mp) {
            bcmd_l3_route_add_ecmp(pfx, len, mp, (int)RTA_PAYLOAD(mp));
        } else if (gw && oif) {
            port = bcmd_port_for_ifindex(*(int *)RTA_DATA(oif));
            if (port >= 0) bcmd_l3_route_add(pfx, len, ntohl(*(unsigned int *)RTA_DATA(gw)), port);
        } else if (!gw && oif && len < 32) {           /* connected subnet -> glean */
            port = bcmd_port_for_ifindex(*(int *)RTA_DATA(oif));
            if (port >= 0) bcmd_l3_connected_add(pfx, len, port);
        }
    }
}

/* One non-blocking read of pending events (live multicast updates). */
static void bcmd_nl_process(int fd)
{
    char buf[16384];
    int len;
    struct nlmsghdr *nh;
    len = (int)recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (len <= 0) return;
    for (nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, (unsigned)len);
         nh = NLMSG_NEXT(nh, len))
        bcmd_nl_handle(nh);
}

/* Blocking-drain a dump response: read until NLMSG_DONE/ERROR so the socket is
 * free for the next dump (one dump in flight per netlink socket). */
static void bcmd_nl_drain(int fd)
{
    char buf[16384];
    int len, done = 0, guard = 0;
    struct nlmsghdr *nh;
    while (!done && guard++ < 4096) {
        len = (int)recv(fd, buf, sizeof(buf), 0);   /* blocking until the dump arrives */
        if (len <= 0) return;
        for (nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, (unsigned)len);
             nh = NLMSG_NEXT(nh, len)) {
            if (nh->nlmsg_type == NLMSG_DONE || nh->nlmsg_type == NLMSG_ERROR) {
                done = 1; break;
            }
            bcmd_nl_handle(nh);
        }
    }
}

/* SFP+ diag: dump the xe0-3 MAC counters so we can localize where frames die on
 * the 10G path — In-NUcast (broadcast, e.g. inbound ARP) climbing means frames
 * DO reach the MAC (so it's punt/forwarding past the MAC); 0 means nothing
 * crosses the 84758 line->system datapath. InErrors/FCS climbing = corrupted
 * frames (serdes/BER). Out counters confirm our own TX leaves the MAC. */
static void bcmd_xe_counters(void)
{
    int port;
    uint64 inu, innu, inerr, infcs, outu, outnu;
    for (port = 50; port <= 53; port++) {
        int link = 0;
        if (bcm_port_link_status_get(BCMD_UNIT, port, &link) != BCM_E_NONE ||
            link != BCM_PORT_LINK_STATUS_UP) continue;
        COMPILER_64_ZERO(inu);  COMPILER_64_ZERO(innu);  COMPILER_64_ZERO(inerr);
        COMPILER_64_ZERO(infcs); COMPILER_64_ZERO(outu); COMPILER_64_ZERO(outnu);
        (void)bcm_stat_get(BCMD_UNIT, port, snmpIfInUcastPkts,      &inu);
        (void)bcm_stat_get(BCMD_UNIT, port, snmpIfInNUcastPkts,     &innu);
        (void)bcm_stat_get(BCMD_UNIT, port, snmpIfInErrors,         &inerr);
        (void)bcm_stat_get(BCMD_UNIT, port, snmpDot3StatsFCSErrors, &infcs);
        (void)bcm_stat_get(BCMD_UNIT, port, snmpIfOutUcastPkts,     &outu);
        (void)bcm_stat_get(BCMD_UNIT, port, snmpIfOutNUcastPkts,    &outnu);
        printf("[bcmd] %s cnt: inUcast=%u inBcast/Mcast=%u inErr=%u fcsErr=%u | outUcast=%u outBcast/Mcast=%u\n",
               SOC_PORT_NAME(BCMD_UNIT, port),
               COMPILER_64_LO(inu), COMPILER_64_LO(innu), COMPILER_64_LO(inerr),
               COMPILER_64_LO(infcs), COMPILER_64_LO(outu), COMPILER_64_LO(outnu));
    }
}

/* Reflect the chip link in the KNET netif's operstate so `ip link` shows UP/DOWN.
 * The linux-bcm-knet netdevs don't manage carrier (operstate stays UNKNOWN, every
 * netif shows LOWER_UP regardless of link, and IFLA_CARRIER set is unsupported), but
 * for such carrier-unmanaged devices the kernel lets userspace set IFLA_OPERSTATE.
 * So the link-scan loop pushes IF_OPER_UP/IF_OPER_DOWN here (admin state untouched). */
static void bcmd_netif_link(const char *name, int up)
{
    unsigned int idx;
    int s;
    struct sockaddr_nl sa;
    char buf[64];
    struct nlmsghdr *nlh;
    struct ifinfomsg *ifi;
    struct rtattr *rta;
    if (!name || !name[0]) return;
    idx = if_nametoindex(name);
    if (!idx) return;
    s = socket(AF_NETLINK, SOCK_RAW, 0);             /* NETLINK_ROUTE */
    if (s < 0) return;
    memset(&sa, 0, sizeof(sa)); sa.nl_family = AF_NETLINK;
    memset(buf, 0, sizeof(buf));
    nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_type  = RTM_NEWLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_len   = NLMSG_LENGTH(sizeof(*ifi));
    ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index  = (int)idx;
    rta = (struct rtattr *)(buf + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = 16;                              /* IFLA_OPERSTATE */
    rta->rta_len  = RTA_LENGTH(1);
    *(unsigned char *)RTA_DATA(rta) = up ? 6 : 2;    /* IF_OPER_UP : IF_OPER_DOWN */
    nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_LENGTH(1);
    (void)sendto(s, buf, nlh->nlmsg_len, 0, (struct sockaddr *)&sa, sizeof(sa));
    close(s);
}

/* ------------------------------------------------------------------- entry */
/* Called from socdiag's main() in place of diag_shell(). SAL/kcom/chip_info
 * config + config.bcm have already been done by main(). */
/* One-line summary of which ports have carrier (avoids 54 lines every tick). */
static void bcmd_link_summary(void)
{
    int port, link, up = 0, total = 0;
    char buf[256]; int n = 0;
    buf[0] = 0;
    for (port = 0; port < BCMD_MAXPORT; port++) {
        if (!bcmd_is_front(port)) continue;
        total++;
        link = 0;
        (void)bcm_port_link_status_get(BCMD_UNIT, port, &link);
        {
            int isup = (link == BCM_PORT_LINK_STATUS_UP);
            /* Re-assert every cycle: the copper link-up sequence can reset the
             * netif operstate after we set it, so a one-shot set wouldn't stick. */
            bcmd_netif_link(SOC_PORT_NAME(BCMD_UNIT, port), isup);
            if (isup) {
                int sp = 0; bcm_port_speed_get(BCMD_UNIT, port, &sp);
                up++;
                if (n < (int)sizeof(buf) - 24)
                    n += snprintf(buf + n, sizeof(buf) - n, " %s(%dG)",
                                      SOC_PORT_NAME(BCMD_UNIT, port), sp / 1000);
            }
        }
    }
    printf("[bcmd] link: %d/%d up:%s\n", up, total, up ? buf : " (none)");
}

/* ============================ ACLs (Field Processor) ======================= *
 * Operator ACLs programmed into the ingress Field Processor via the OpenBCM SDK.
 * Config /etc/bcmd/acls.conf, reloaded on SIGHUP alongside L2 groups:
 *   rule:  <name> <seq> permit|deny <proto> <src-cidr> <dst-cidr> [dport N] [sport N]
 *   bind:  <name> apply <port> [<port> ...]
 * A rule is programmed only for ACLs that have an apply line; unmatched traffic is
 * permitted (implicit permit). v4 and v6 rules use separate FP groups. Lower seq =
 * higher precedence. See docs/acl-design.md (Phase 1). */

static bcm_field_group_t bcmd_acl_g4 = -1, bcmd_acl_g6 = -1;

/* Installed entries, tracked so a reload can remove them (group_flush does not
 * uninstall from HW on this chip — a bare flush left stale drops in the TCAM). */
#define BCMD_ACL_MAX_ENTRIES 1024
static bcm_field_entry_t bcmd_acl_entries[BCMD_ACL_MAX_ENTRIES];
static int bcmd_acl_nent = 0;

#define BCMD_ACL_MAX_RULES 128
#define BCMD_ACL_MAX_BINDS 32
#define BCMD_ACL_PRIO_BASE 30000

struct bcmd_acl_rule { char name[32]; char src[48]; char dst[48];
                       int seq, deny, proto, dport, sport; };
struct bcmd_acl_bind { char name[32]; bcm_pbmp_t pbmp; };

/* Get (creating on first use) the v4 or v6 ingress FP group. */
static bcm_field_group_t bcmd_acl_group(int v6)
{
    bcm_field_group_t *g = v6 ? &bcmd_acl_g6 : &bcmd_acl_g4;
    bcm_field_qset_t qs;
    int rv;
    if (*g >= 0) return *g;
    BCM_FIELD_QSET_INIT(qs);
    BCM_FIELD_QSET_ADD(qs, bcmFieldQualifyInPorts);
    if (v6) { BCM_FIELD_QSET_ADD(qs, bcmFieldQualifySrcIp6);
              BCM_FIELD_QSET_ADD(qs, bcmFieldQualifyDstIp6); }
    else    { BCM_FIELD_QSET_ADD(qs, bcmFieldQualifySrcIp);
              BCM_FIELD_QSET_ADD(qs, bcmFieldQualifyDstIp); }
    BCM_FIELD_QSET_ADD(qs, bcmFieldQualifyIpProtocol);
    BCM_FIELD_QSET_ADD(qs, bcmFieldQualifyL4SrcPort);
    BCM_FIELD_QSET_ADD(qs, bcmFieldQualifyL4DstPort);
    rv = bcm_field_group_create(BCMD_UNIT, qs, BCM_FIELD_GROUP_PRIO_ANY, g);
    if (rv != BCM_E_NONE) {
        printf("[bcmd] ACL: field_group_create(v%d) rv=%d (%s)\n",
               v6 ? 6 : 4, rv, bcm_errmsg(rv));
        *g = -1; return -1;
    }
    return *g;
}

/* "10.0.0.0/24"/"any" -> ip+mask. Returns 1 = wildcard (skip qualifier), 0 = ok, -1 err. */
static int bcmd_acl_cidr4(const char *tok, bcm_ip_t *ip, bcm_ip_t *mask)
{
    char buf[48], *slash; int bits = 32; struct in_addr a;
    if (!strcmp(tok, "any") || !strcmp(tok, "0.0.0.0/0")) return 1;
    sal_strncpy(buf, tok, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    slash = strchr(buf, '/'); if (slash) { *slash = 0; bits = atoi(slash + 1); }
    if (inet_pton(AF_INET, buf, &a) != 1) return -1;
    *ip = ntohl(a.s_addr);
    *mask = bits <= 0 ? 0 : (bits >= 32 ? 0xFFFFFFFFu : (0xFFFFFFFFu << (32 - bits)));
    return 0;
}

/* Same for v6. ip/mask are bcm_ip6_t (uint8[16]). */
static int bcmd_acl_cidr6(const char *tok, bcm_ip6_t ip, bcm_ip6_t mask)
{
    char buf[48], *slash; int bits = 128, i; struct in6_addr a;
    if (!strcmp(tok, "any") || !strcmp(tok, "::/0")) return 1;
    sal_strncpy(buf, tok, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    slash = strchr(buf, '/'); if (slash) { *slash = 0; bits = atoi(slash + 1); }
    if (inet_pton(AF_INET6, buf, &a) != 1) return -1;
    memcpy(ip, a.s6_addr, 16);
    memset(mask, 0, 16);
    for (i = 0; i < bits && i < 128; i++) mask[i / 8] |= (unsigned char)(0x80 >> (i % 8));
    return 0;
}

static int bcmd_acl_proto(const char *p)
{
    if (!strcmp(p, "ip") || !strcmp(p, "any") || !strcmp(p, "all")) return 0;
    if (!strcmp(p, "tcp"))  return 6;
    if (!strcmp(p, "udp"))  return 17;
    if (!strcmp(p, "icmp")) return 1;
    if (!strcmp(p, "icmp6") || !strcmp(p, "ipv6-icmp")) return 58;
    return atoi(p);   /* numeric protocol number */
}

/* Program one rule as one FP entry matching exactly one ingress port. */
static int bcmd_acl_program_one(const struct bcmd_acl_rule *r, bcm_port_t port)
{
    int v6 = (strchr(r->src, ':') || strchr(r->dst, ':'));
    bcm_field_group_t g = bcmd_acl_group(v6);
    bcm_field_entry_t e;
    bcm_pbmp_t data, mask;
    int p, rv;
    if (g < 0) return -1;
    if (bcm_field_entry_create(BCMD_UNIT, g, &e) != BCM_E_NONE) return -1;
    BCM_PBMP_CLEAR(data); BCM_PBMP_PORT_ADD(data, port);
    BCM_PBMP_CLEAR(mask); for (p = 0; p < BCMD_MAXPORT; p++) BCM_PBMP_PORT_ADD(mask, p);
    bcm_field_qualify_InPorts(BCMD_UNIT, e, data, mask);
    if (v6) {
        bcm_ip6_t s, sm, d, dm;
        if (bcmd_acl_cidr6(r->src, s, sm) == 0) bcm_field_qualify_SrcIp6(BCMD_UNIT, e, s, sm);
        if (bcmd_acl_cidr6(r->dst, d, dm) == 0) bcm_field_qualify_DstIp6(BCMD_UNIT, e, d, dm);
    } else {
        bcm_ip_t s, sm, d, dm;
        if (bcmd_acl_cidr4(r->src, &s, &sm) == 0) bcm_field_qualify_SrcIp(BCMD_UNIT, e, s, sm);
        if (bcmd_acl_cidr4(r->dst, &d, &dm) == 0) bcm_field_qualify_DstIp(BCMD_UNIT, e, d, dm);
    }
    if (r->proto) bcm_field_qualify_IpProtocol(BCMD_UNIT, e, (uint8)r->proto, 0xff);
    if (r->dport) bcm_field_qualify_L4DstPort(BCMD_UNIT, e, r->dport, 0xffff);
    if (r->sport) bcm_field_qualify_L4SrcPort(BCMD_UNIT, e, r->sport, 0xffff);
    if (r->deny)  bcm_field_action_add(BCMD_UNIT, e, bcmFieldActionDrop, 0, 0);
    bcm_field_entry_prio_set(BCMD_UNIT, e, BCMD_ACL_PRIO_BASE - r->seq);
    rv = bcm_field_entry_install(BCMD_UNIT, e);
    if (rv != BCM_E_NONE) {
        printf("[bcmd] ACL %s seq %d: install rv=%d (%s)\n",
               r->name, r->seq, rv, bcm_errmsg(rv));
        (void)bcm_field_entry_destroy(BCMD_UNIT, e);
        return -1;
    }
    if (bcmd_acl_nent < BCMD_ACL_MAX_ENTRIES) bcmd_acl_entries[bcmd_acl_nent++] = e;
    return 0;
}

/* Remove all ACL entries (keep the groups) so a reload rebuilds cleanly. Each
 * entry is explicitly removed from HW then destroyed — group_flush alone left the
 * TCAM drops in place on this chip. */
static void bcmd_acl_reset(void)
{
    int i;
    for (i = 0; i < bcmd_acl_nent; i++) {
        (void)bcm_field_entry_remove(BCMD_UNIT, bcmd_acl_entries[i]);
        (void)bcm_field_entry_destroy(BCMD_UNIT, bcmd_acl_entries[i]);
    }
    bcmd_acl_nent = 0;
}

static int bcmd_load_acls(const char *path)
{
    static struct bcmd_acl_rule rules[BCMD_ACL_MAX_RULES];
    static struct bcmd_acl_bind binds[BCMD_ACL_MAX_BINDS];
    FILE *f = fopen(path, "r");
    char line[256];
    int nr = 0, nb = 0, i, j, done = 0;
    if (!f) { printf("[bcmd] ACL: no %s (none configured)\n", path); return 0; }
    while (fgets(line, sizeof(line), f)) {
        char *s = line, *name, *t2;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\0') continue;
        name = strtok(s, " \t\n"); if (!name) continue;
        t2 = strtok(NULL, " \t\n"); if (!t2) continue;
        if (!strcmp(t2, "apply")) {                       /* bind: <name> apply <port>... */
            struct bcmd_acl_bind *b = NULL; char *pt;
            for (i = 0; i < nb; i++) if (!strcmp(binds[i].name, name)) b = &binds[i];
            if (!b && nb < BCMD_ACL_MAX_BINDS) {
                b = &binds[nb++]; sal_strncpy(b->name, name, sizeof(b->name) - 1);
                b->name[sizeof(b->name) - 1] = 0; BCM_PBMP_CLEAR(b->pbmp);
            }
            while ((pt = strtok(NULL, " \t\n"))) {
                int pp = bcmd_port_from_token(pt);
                if (pp >= 0 && b) BCM_PBMP_PORT_ADD(b->pbmp, pp);
                else if (pp < 0)  printf("[bcmd] ACL %s: unknown port '%s'\n", name, pt);
            }
        } else {                                          /* rule: <name> <seq> ... */
            struct bcmd_acl_rule *r; char *act, *pr, *sr, *ds, *k;
            if (nr >= BCMD_ACL_MAX_RULES) continue;
            r = &rules[nr];
            memset(r, 0, sizeof(*r));
            sal_strncpy(r->name, name, sizeof(r->name) - 1);
            r->seq = atoi(t2);
            act = strtok(NULL, " \t\n"); pr = strtok(NULL, " \t\n");
            sr  = strtok(NULL, " \t\n"); ds = strtok(NULL, " \t\n");
            if (!act || !pr || !sr || !ds) { printf("[bcmd] ACL %s: short rule\n", name); continue; }
            r->deny  = !strcmp(act, "deny");
            r->proto = bcmd_acl_proto(pr);
            sal_strncpy(r->src, sr, sizeof(r->src) - 1);
            sal_strncpy(r->dst, ds, sizeof(r->dst) - 1);
            while ((k = strtok(NULL, " \t\n"))) {
                char *v = strtok(NULL, " \t\n"); if (!v) break;
                if      (!strcmp(k, "dport")) r->dport = atoi(v);
                else if (!strcmp(k, "sport")) r->sport = atoi(v);
            }
            nr++;
        }
    }
    fclose(f);
    for (i = 0; i < nr; i++) {                             /* program bound rules per port */
        struct bcmd_acl_bind *b = NULL; int p;
        for (j = 0; j < nb; j++) if (!strcmp(binds[j].name, rules[i].name)) b = &binds[j];
        if (!b) continue;                                 /* ACL with no apply line: inert */
        for (p = 0; p < BCMD_MAXPORT; p++)
            if (BCM_PBMP_MEMBER(b->pbmp, p) && bcmd_acl_program_one(&rules[i], p) == 0) done++;
    }
    printf("[bcmd] ACL: installed %d entr%s from %s (%d rules, %d bound ACLs)\n",
           done, done == 1 ? "y" : "ies", path, nr, nb);
    return done;
}

int bcmd_run(void)
{
    static int netif_ids[BCMD_MAXPORT + 1];
    int tick = 0, port, n = 0, nlfd = -1, edc_done = 0;

    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: see logs live in /tmp/bcmd.log */
    printf("[bcmd] EdgeNOS-4610 datapath daemon starting\n");

    /* Board-level external-PHY enable — MUST run before `init all` so the SDK's
     * PHY probe finds + attaches the now-awake 84758 (firmware + serdes + link). */
    bcmd_deassert_ext_phy_reset();
    bcmd_mdio_to_cmic();
    bcmd_write_icos_miim_map();

    if (bcmd_chip_init() < 0) return 1;

    /* bring up every front-panel port (tolerant — one failure doesn't abort) */
    for (port = 0; port < BCMD_MAXPORT; port++) {
        if (!bcmd_is_front(port)) continue;
        if (bcmd_port_up(port) == BCM_E_NONE) n++;
    }
    printf("[bcmd] enabled %d ports\n", n);

    /* L2 switch groups: bridge selected ports into one shared VLAN, overriding
     * their per-port isolation. Mirrors the 5610 edged /etc/edged/l2-groups.conf. */
    bcmd_load_l2_groups("/etc/bcmd/l2-groups.conf");
    bcmd_load_acls("/etc/bcmd/acls.conf");            /* operator ACLs (Field Processor) */

    bcmd_sfp_laser_all();   /* SFP+ optic TX laser on xe0-3 (harmless on copper) */
    if (bcmd_rx_start()          != BCM_E_NONE) return 1;
    if (bcmd_knet_setup(netif_ids) != BCM_E_NONE) return 1;
    if (bcmd_l2_punt()           != BCM_E_NONE) return 1;

    /* netlink -> chip control: `ip link set geN up/down` drives the chip port.
     * Bring every netdev admin-up FIRST (matches the chip-enabled state) so the
     * listener only acts on real admin changes, then open the socket. */
    bcmd_build_ifmap();
    for (port = 0; port < BCMD_MAXPORT; port++)
        if (bcmd_is_front(port)) bcmd_netdev_up(SOC_PORT_NAME(BCMD_UNIT, port));
    nlfd = bcmd_nl_open();
    if (nlfd >= 0) {                       /* program the existing FIB into the chip */
        bcmd_nl_dump(nlfd, RTM_GETADDR,  AF_INET);
        bcmd_nl_dump(nlfd, RTM_GETNEIGH, AF_INET);
        bcmd_nl_dump(nlfd, RTM_GETROUTE, AF_INET);
        bcmd_nl_dump(nlfd, RTM_GETADDR,  AF_INET6);
        bcmd_nl_dump(nlfd, RTM_GETNEIGH, AF_INET6);
        bcmd_nl_dump(nlfd, RTM_GETROUTE, AF_INET6);
    }

    printf("[bcmd] datapath UP — every port a Linux netdev; L3 FIB mirrored to chip.\n"
           "[bcmd] Linux-controlled: ip addr/link/route + OSPF/BGP -> ASIC L3 tables.\n");

    signal(SIGINT,  bcmd_on_sig);
    signal(SIGTERM, bcmd_on_sig);
    signal(SIGHUP,  bcmd_on_sig);   /* live L2-switch group re-sync from config */
    bcmd_link_summary();
    while (bcmd_g_run) {
        if (nlfd >= 0) {
            struct pollfd pfd;
            pfd.fd = nlfd; pfd.events = POLLIN; pfd.revents = 0;
            if (poll(&pfd, 1, 2000) > 0 && (pfd.revents & POLLIN))
                bcmd_nl_process(nlfd);     /* apply link/addr/neigh/route to chip */
        } else {
            sal_sleep(2);
        }
        if (bcmd_l2_resync_req) {       /* SIGHUP: re-apply L2 groups from config, live */
            bcmd_l2_resync_req = 0;
            printf("[bcmd] SIGHUP: re-syncing L2 switch groups + ACLs\n");
            bcmd_l2_reset();
            bcmd_load_l2_groups("/etc/bcmd/l2-groups.conf");
            bcmd_acl_reset();
            bcmd_load_acls("/etc/bcmd/acls.conf");
        }
        /* periodic FIB re-dump: converge routes whose gateway resolved later. */
        if (nlfd >= 0 && (tick % 15) == 14) {
            bcmd_nl_dump(nlfd, RTM_GETNEIGH, AF_INET);
            bcmd_nl_dump(nlfd, RTM_GETROUTE, AF_INET);
            bcmd_nl_dump(nlfd, RTM_GETNEIGH, AF_INET6);
            bcmd_nl_dump(nlfd, RTM_GETROUTE, AF_INET6);
        }
        if ((++tick % 5) == 0) { bcmd_link_summary(); bcmd_xe_counters(); }  /* ~10s */
        /* One-shot 84758 media-RX re-acquire once the link has settled (~16s) and
         * the optic signal is present, so the LOS-toggle actually triggers the uC. */
        if (!edc_done && tick == 8) {
            int p, any = 0;
            for (p = 50; p <= 53; p++) {
                int lk = 0;
                if (bcm_port_link_status_get(BCMD_UNIT, p, &lk) == BCM_E_NONE &&
                    lk == BCM_PORT_LINK_STATUS_UP) { bcmd_sfp_edc_reacquire(0x40 + (p - 50)); any = 1; }
            }
            if (any) edc_done = 1;
        }
        /* L3 FIB sync (add + del) is live: RTM_{NEW,DEL}{ADDR,NEIGH,ROUTE} ->
         * chip intf/host/route, so `ip route`/FRR program + withdraw HW forwarding. */
    }

    printf("[bcmd] shutting down\n");
    bcm_rx_stop(BCMD_UNIT, NULL);
    return 0;
}
