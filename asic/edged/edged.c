/*
 * edged.c - EdgeNOS Switch Daemon
 *
 * Main daemon that initializes the BCM56846 ASIC via OpenMDK,
 * creates TUN interfaces for each port, handles packet I/O
 * between the kernel and ASIC, and programs L2/L3 forwarding
 * via netlink events.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>
#include <syslog.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "edged.h"
#include "portmap.h"
#include "packet_io.h"
#include "netlink.h"
#include "l2.h"
#include "l3.h"
#include "vlan.h"

/* BMD/PHY headers for CLAUSE45 fix */
#include <bmd/bmd.h>
#include <bmd/bmd_phy_ctrl.h>
#include <cdk/cdk_device.h>
#include <cdk/arch/xgs_chip.h>
#include <phy/phy.h>

/* Global state */
struct edged_state edged;
static volatile int running = 1;

static void signal_handler(int sig)
{
    if (sig == SIGTERM || sig == SIGINT) {
        syslog(LOG_INFO, "Received signal %d, shutting down", sig);
        running = 0;
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -c, --config FILE   ASIC config file (default: /etc/edged/config.bcm)\n"
        "  -d, --debug         Enable debug logging\n"
        "  -f, --foreground    Run in foreground (don't daemonize)\n"
        "  -h, --help          Show this help\n",
        prog);
}

static int parse_config(const char *path)
{
    FILE *fp;
    char line[256];
    int count = 0;

    fp = fopen(path, "r");
    if (!fp) {
        syslog(LOG_ERR, "Cannot open config %s: %s", path, strerror(errno));
        return -1;
    }

    syslog(LOG_INFO, "Loading config from %s", path);

    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char *key, *val;

        /* Skip whitespace, comments, empty lines */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;

        /* Remove trailing newline */
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';

        /* Parse key=value */
        key = p;
        val = strchr(p, '=');
        if (!val) continue;
        *val++ = '\0';

        /* Store in config (portmap entries handled by portmap module) */
        if (strncmp(key, "portmap_", 8) == 0) {
            portmap_parse_config(key, val);
        }

        count++;
    }

    fclose(fp);
    syslog(LOG_INFO, "Loaded %d config entries", count);
    return 0;
}

static int asic_init(void)
{
    int rv;

    syslog(LOG_INFO, "Initializing ASIC...");

    /* Open BDE device */
    rv = bde_open();
    if (rv < 0) {
        syslog(LOG_ERR, "Failed to open BDE device");
        return rv;
    }

    /* Probe and attach chip via CDK */
    rv = cdk_init();
    if (rv < 0) {
        syslog(LOG_ERR, "CDK init failed");
        return rv;
    }

    /* Run BMD initialization (reset, port init, enable) */
    rv = bmd_init_all();
    if (rv < 0) {
        syslog(LOG_ERR, "BMD init failed");
        return rv;
    }

    /*
     * CRITICAL: Clear PHY_F_CLAUSE45 on all internal Warpcore PHYs
     * BEFORE bmd_switching_init (which calls bmd_port_mode_set).
     *
     * bmd_init sets CLAUSE45 based on Warpcore MULTIMMDS_EN register.
     * On BCM56846 with iProc, CL45 MIIM doesn't work for internal
     * SerDes — register writes via CL45 path silently fail, causing
     * speed encoding to never change from default CX4.
     *
     * Must be cleared after bmd_init (which sets the flag) but before
     * any bmd_port_mode_set call (which writes speed registers).
     */
    {
        int p;
        for (p = 0; p < BMD_CONFIG_MAX_PORTS; p++) {
            phy_ctrl_t *pc = BMD_PORT_PHY_CTRL(edged.unit, p);
            if (pc && (PHY_CTRL_FLAGS(pc) & PHY_F_CLAUSE45)) {
                PHY_CTRL_FLAGS(pc) &= ~PHY_F_CLAUSE45;
            }
        }
        syslog(LOG_INFO, "Cleared PHY_F_CLAUSE45 on all internal Warpcores");
    }

    /* Initialize switching (L2 tables, VLANs) */
    rv = bmd_switching_init_all();
    if (rv < 0) {
        syslog(LOG_ERR, "BMD switching init failed");
        return rv;
    }

    /* Configure port modes */
    rv = portmap_configure_ports();
    if (rv < 0) {
        syslog(LOG_ERR, "Port configuration failed");
        return rv;
    }

    /* Set up default VLAN (all ports in VLAN 1) */
    rv = vlan_init_default();
    if (rv < 0) {
        syslog(LOG_ERR, "VLAN init failed");
        return rv;
    }

    /* Per-port service VLANs (Cumulus's 3301-3352 scheme).  Required so
     * CPU TX can direct frames via 802.1Q tag instead of HiGig SOB —
     * the SOB path works for ARP but Nexus drops our IPv4 SOB-directed
     * frames, while Cumulus did it this way and ICMP worked. */
    rv = vlan_init_resv_per_port();
    if (rv < 0) {
        syslog(LOG_WARNING,
               "Per-port service VLAN init failed (continuing anyway)");
    }

    /* Diagnostic: dump VID 3301/3302 membership after setup so we can
     * confirm CPU (port 0) is in the bitmap. */
    {
        int v;
        for (v = 3301; v <= 3302; v++) {
            int plist[BMD_CONFIG_MAX_PORTS + 1];
            int utlist[BMD_CONFIG_MAX_PORTS + 1];
            int i;
            char pbuf[128] = "";
            char ubuf[128] = "";
            int rc = bmd_vlan_port_get(0, v, plist, utlist);
            if (rc != 0) {
                syslog(LOG_INFO, "VID %d dump: bmd_vlan_port_get rc=%d", v, rc);
                continue;
            }
            for (i = 0; plist[i] != -1 && i < 32; i++) {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), "%d ", plist[i]);
                strncat(pbuf, tmp, sizeof(pbuf) - strlen(pbuf) - 1);
            }
            for (i = 0; utlist[i] != -1 && i < 32; i++) {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), "%d ", utlist[i]);
                strncat(ubuf, tmp, sizeof(ubuf) - strlen(ubuf) - 1);
            }
            syslog(LOG_INFO, "VID %d members: [%s] untagged: [%s]",
                   v, pbuf, ubuf);
        }
    }

    /* Configure datapath: CPU punt, hash, buffer thresholds */
    rv = datapath_init();
    if (rv < 0) {
        syslog(LOG_ERR, "Datapath init failed");
        return rv;
    }

    syslog(LOG_INFO, "ASIC initialization complete");

    /*
     * If /tmp/regdump.in exists, read every (address, name) pair from
     * it and write our chip's actual values to /tmp/regdump.out so we
     * can diff vs Cumulus's dump_soc_diff.txt.
     *
     * Input  format (matches Cumulus dump):
     *     0x0f180d34 NAME.scope = 0x00000001
     * Output format:
     *     0x0f180d34 NAME.scope cum=0x00000001 ours=0x????????
     */
    {
        FILE *fi = fopen("/tmp/regdump.in", "r");
        if (fi) {
            FILE *fo = fopen("/tmp/regdump.out", "w");
            if (fo) {
                char line[512];
                int matched = 0, read_err = 0;
                while (fgets(line, sizeof(line), fi)) {
                    unsigned long addr;
                    char namebuf[200] = {0};
                    unsigned long cumval;
                    int n = sscanf(line, "%lx %199s = %lx",
                                   &addr, namebuf, &cumval);
                    if (n != 3) continue;
                    uint32_t our_val = 0;
                    int rv = cdk_xgs_reg32_read(edged.unit,
                                                (uint32_t)addr,
                                                &our_val);
                    if (rv != 0) { read_err++; }
                    fprintf(fo, "0x%08lx %s cum=0x%08lx ours=0x%08x%s\n",
                            addr, namebuf, cumval, our_val,
                            (rv == 0 && our_val == cumval) ? "" : " DIFF");
                    matched++;
                }
                fclose(fo);
                syslog(LOG_INFO,
                       "regdump: %d registers read, %d read-errors, "
                       "output in /tmp/regdump.out",
                       matched, read_err);
            }
            fclose(fi);
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    const char *config_file = "/etc/edged/config.bcm";
    int foreground = 0;
    int debug = 0;
    int opt;

    static struct option long_opts[] = {
        {"config",     required_argument, 0, 'c'},
        {"debug",      no_argument,       0, 'd'},
        {"foreground", no_argument,       0, 'f'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "c:dfh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'c': config_file = optarg; break;
        case 'd': debug = 1; break;
        case 'f': foreground = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    /* Open syslog */
    openlog("edged", LOG_PID | (foreground ? LOG_PERROR : 0), LOG_DAEMON);
    syslog(LOG_INFO, "EdgeNOS edged starting");

    if (debug)
        setlogmask(LOG_UPTO(LOG_DEBUG));

    /* Daemonize unless foreground mode */
    if (!foreground) {
        if (daemon(0, 0) < 0) {
            syslog(LOG_ERR, "daemon() failed: %s", strerror(errno));
            return 1;
        }
    }

    /* Signal handling */
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Initialize state */
    memset(&edged, 0, sizeof(edged));

    /* Parse config */
    if (parse_config(config_file) < 0) {
        syslog(LOG_ERR, "Config parse failed");
        return 1;
    }

    /* Initialize ASIC */
    if (asic_init() < 0) {
        syslog(LOG_ERR, "ASIC init failed, exiting");
        return 1;
    }

    /*
     * Reset DMA pool after ASIC init.
     * BMD init uses DMA for S-Channel table writes (temporary).
     * These are freed by BMD but our bump allocator ignores frees.
     * Reset the pool so packet I/O can use the full 4MB.
     */
    bde_dma_pool_reset();

    /*
     * Set DMA endianness right before packet I/O starts.
     * CPS reset during bmd_reset clears CMIC_ENDIANESS_SEL, and
     * the CDK's re-init doesn't stick reliably. Force it here.
     * 0x06000006 = DMA_PACKET + DMA_OTHER (no PIO endian swap
     * since iowrite32 already provides LE on PPC).
     */
    bde_set_dma_endianness();

    /* Create TUN interfaces for each port */
    if (packet_io_init() < 0) {
        syslog(LOG_ERR, "Packet I/O init failed");
        return 1;
    }

    /* Start netlink listener */
    if (netlink_init() < 0) {
        syslog(LOG_ERR, "Netlink init failed");
        return 1;
    }

    /* Initialize L2 forwarding */
    l2_init();

    /* Initialize L3 routing */
    l3_init();

    syslog(LOG_INFO, "edged ready, entering main loop");

    /*
     * Main loop: poll for RX packets, netlink events, and link state.
     *
     * Cumulus architecture (from RE captures):
     *   Thread 10792: RX path (ASIC -> TUN write), runs on BDE interrupt
     *   Thread 10793: TX path (TUN read -> ASIC), runs on select()
     *   Thread 5294:  Link polling (MIIM reads at 30ms intervals)
     *
     * Our simplified single-threaded approach:
     *   - packet_io_rx_poll(): check for RX packets from ASIC
     *   - netlink_poll(): process kernel route/neigh/link events
     *   - portmap_link_poll(): poll PHY link status every ~30ms
     *     (uses bmd_port_mode_update which reads WC MII_STATUS
     *      on page 0x1800 and enables/disables MAC on change)
     */
    int poll_count = 0;
    int stat_poll_count = 0;
    while (running) {
        packet_io_rx_poll();
        netlink_poll();

        /* Link poll every ~30ms (300 iterations at 100us) */
        if (++poll_count >= 300) {
            portmap_link_poll();
            poll_count = 0;
        }

        /* Chip-level RX/TX counter sample every ~3s. */
        if (++stat_poll_count >= 30000) {
            stat_poll_count = 0;
            int chip_port;
            int probe_ports[] = {0, 65, 66, -1};   /* CPU + swp1 + swp2 */
            int pi;
            for (pi = 0; (chip_port = probe_ports[pi]) >= 0; pi++) {
                bmd_counter_t rx_pkts, rx_drops, rx_err, tx_pkts, tx_err;
                int r1, r2, r3, r4, r5;
                memset(&rx_pkts,  0, sizeof(rx_pkts));
                memset(&rx_drops, 0, sizeof(rx_drops));
                memset(&rx_err,   0, sizeof(rx_err));
                memset(&tx_pkts,  0, sizeof(tx_pkts));
                memset(&tx_err,   0, sizeof(tx_err));
                r1 = bmd_stat_get(edged.unit, chip_port,
                                  bmdStatRxPackets, &rx_pkts);
                r2 = bmd_stat_get(edged.unit, chip_port,
                                  bmdStatRxDrops,   &rx_drops);
                r3 = bmd_stat_get(edged.unit, chip_port,
                                  bmdStatRxErrors,  &rx_err);
                r4 = bmd_stat_get(edged.unit, chip_port,
                                  bmdStatTxPackets, &tx_pkts);
                r5 = bmd_stat_get(edged.unit, chip_port,
                                  bmdStatTxErrors,  &tx_err);
                const char *label =
                    (chip_port == 0) ? "CPU" :
                    (chip_port == 65) ? "swp1" :
                    (chip_port == 66) ? "swp2" : "???";
                syslog(LOG_INFO,
                       "chip stats %s (port %d): "
                       "rx_pkts=%u rx_drops=%u rx_err=%u tx_pkts=%u tx_err=%u "
                       "(rv=%d/%d/%d/%d/%d)",
                       label, chip_port,
                       rx_pkts.v[0], rx_drops.v[0], rx_err.v[0],
                       tx_pkts.v[0], tx_err.v[0],
                       r1, r2, r3, r4, r5);
            }
        }

        usleep(100);  /* 100us poll interval */
    }

    /* Cleanup */
    syslog(LOG_INFO, "edged shutting down");
    netlink_cleanup();
    packet_io_cleanup();
    bde_close();
    closelog();

    return 0;
}
