/*
 * switchd.c - EdgeNOS Switch Daemon
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

#include "switchd.h"
#include "portmap.h"
#include "packet_io.h"
#include "netlink.h"
#include "l2.h"
#include "l3.h"
#include "vlan.h"

/* Global state */
struct switchd_state switchd;
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
        "  -c, --config FILE   ASIC config file (default: /etc/switchd/config.bcm)\n"
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

    /* Configure datapath: CPU punt, hash, buffer thresholds */
    rv = datapath_init();
    if (rv < 0) {
        syslog(LOG_ERR, "Datapath init failed");
        return rv;
    }

    syslog(LOG_INFO, "ASIC initialization complete");
    return 0;
}

int main(int argc, char **argv)
{
    const char *config_file = "/etc/switchd/config.bcm";
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
    openlog("switchd", LOG_PID | (foreground ? LOG_PERROR : 0), LOG_DAEMON);
    syslog(LOG_INFO, "EdgeNOS switchd starting");

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
    memset(&switchd, 0, sizeof(switchd));

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

    syslog(LOG_INFO, "switchd ready, entering main loop");

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
    while (running) {
        packet_io_rx_poll();
        netlink_poll();

        /* Link poll every ~30ms (300 iterations at 100us) */
        if (++poll_count >= 300) {
            portmap_link_poll();
            poll_count = 0;
        }

        usleep(100);  /* 100us poll interval */
    }

    /* Cleanup */
    syslog(LOG_INFO, "switchd shutting down");
    netlink_cleanup();
    packet_io_cleanup();
    bde_close();
    closelog();

    return 0;
}
