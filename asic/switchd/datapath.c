/*
 * datapath.c - ASIC datapath configuration for AS5610-52X
 *
 * Applies CPU punt rules, buffer thresholds, scheduling, and hash
 * configuration. Equivalent to Cumulus rc.datapath_0.
 *
 * Values captured from Cumulus 2.5.1 on live AS5610-52X (March 27, 2026).
 * See traces/cumulus_rc.datapath_0 for the full Cumulus configuration.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <syslog.h>

#include "switchd.h"

#include <string.h>

#include <cdk/chip/bcm56840_a0_defs.h>
#include <cdk/arch/xgs_chip.h>

/* From bcm56840_a0_internal.h */
extern int bcm56840_a0_xlport_pbmp_get(int unit, cdk_pbmp_t *pbmp);

/* S-Channel memory access (from cdk/pkgsrc/arch/xgs/xgs_mem.c) */
extern int cdk_xgs_mem_read(int unit, uint32_t addr, uint32_t idx, void *vptr, int size);
extern int cdk_xgs_mem_write(int unit, uint32_t addr, uint32_t idx, void *vptr, int size);

/*
 * CPU punt configuration.
 *
 * Without these, protocol packets (ARP, DHCP, LLDP) and routing
 * misses stay in the ASIC pipeline and never reach the CPU.
 * Cumulus sets these via "modreg cpu_control_1" in rc.datapath_0.
 */
static int datapath_cpu_punt_init(int unit)
{
    int ioerr = 0;
    CPU_CONTROL_1r_t cpu_ctrl1;

    ioerr += READ_CPU_CONTROL_1r(unit, &cpu_ctrl1);

    /* Send L3 MTU failures to CPU */
    CPU_CONTROL_1r_L3_MTU_FAIL_TOCPUf_SET(cpu_ctrl1, 1);

    /* Send IP packets with options to CPU (slow path) */
    CPU_CONTROL_1r_L3_SLOWPATH_TOCPUf_SET(cpu_ctrl1, 1);

    /* Send L3 destination misses to CPU (for routing) */
    CPU_CONTROL_1r_V4L3DSTMISS_TOCPUf_SET(cpu_ctrl1, 1);
    CPU_CONTROL_1r_V6L3DSTMISS_TOCPUf_SET(cpu_ctrl1, 1);

    ioerr += WRITE_CPU_CONTROL_1r(unit, cpu_ctrl1);

    /* Enable ARP and DHCP punt to CPU on all valid ports.
     * PROTOCOL_PKT_CONTROLr is per-port (indexed by logical port).
     * BCM56840 has ports 1-72 (logical). Skip 0 (CPU) and stop at 72.
     * S-channel errors on non-existent ports are harmless but slow.
     */
    {
        int p;
        PROTOCOL_PKT_CONTROLr_t ppc;
        for (p = 1; p <= 72; p++) {
            if (READ_PROTOCOL_PKT_CONTROLr(unit, p, &ppc) != 0)
                continue;  /* Skip non-existent ports */
            PROTOCOL_PKT_CONTROLr_ARP_REQUEST_TO_CPUf_SET(ppc, 1);
            PROTOCOL_PKT_CONTROLr_ARP_REPLY_TO_CPUf_SET(ppc, 1);
            PROTOCOL_PKT_CONTROLr_DHCP_PKT_TO_CPUf_SET(ppc, 1);
            ioerr += WRITE_PROTOCOL_PKT_CONTROLr(unit, p, ppc);
        }
    }

    /*
     * Enable L3 processing on all front-panel ports.
     * LPORT_TABm V4L3_ENABLE=1 and V6L3_ENABLE=1 are required for
     * MY_STATION_TCAM to trigger L3 lookup on matching frames.
     * Without this, even matched frames stay in L2 forwarding.
     */
    {
        static uint32_t lptab[10];  /* LPORT_TABm is 10 words */
        int p, rv;
        for (p = 1; p <= 72; p++) {
            memset(lptab, 0, sizeof(lptab));
            rv = cdk_xgs_mem_read(unit, LPORT_TABm, p, lptab, 10);
            if (rv != 0) continue;
            lptab[0] |= (1 << 19) | (1 << 18);  /* V4L3_ENABLE | V6L3_ENABLE */
            cdk_xgs_mem_write(unit, LPORT_TABm, p, lptab, 10);
        }
        syslog(LOG_INFO, "L3 enabled on all front-panel ports (V4+V6)");
    }

    /* Disable hardware MAC learning on CPU port (port 0).
     * When CPU sends a frame, the ASIC learns the source MAC on port 0.
     * Later, when a reply arrives on port 66 with that MAC as destination,
     * the L2_ENTRY hit sends it to port 0 (CPU) — but the L2_ENTRY
     * match takes priority over L2_USER_ENTRY COPY_TO_CPU.
     * By disabling learning on CPU, no L2_ENTRY is created, and
     * L2_USER_ENTRY COPY_TO_CPU triggers instead.
     *
     * Actually: we need the OPPOSITE. L2_ENTRY at port 0 is GOOD.
     * The problem is learning on FRONT-PANEL ports: when we TX from
     * CPU out port 66, port 66 might learn our MAC too (overriding
     * the CPU port learning). Disable learning on front-panel only.
     */
    {
        static uint32_t ptab[10];
        int p, rv;
        /* ALL ports including CPU: CML=0. No L2_ENTRY will be created.
         * L2_USER_ENTRY COPY_TO_CPU triggers on L2 miss (direct punt). */
        for (p = 0; p <= 72; p++) {
            memset(ptab, 0, sizeof(ptab));
            rv = cdk_xgs_mem_read(unit, PORT_TABm, p, ptab, 10);
            if (rv != 0) continue;
            ptab[4] &= ~(0x1FE);  /* CML=0 */
            cdk_xgs_mem_write(unit, PORT_TABm, p, ptab, 10);
        }
        syslog(LOG_INFO, "ALL ports CML=0 (learning disabled)");

        /* Configure MMU output queues for CPU port using OP_QUEUE_CONFIG registers.
         * From Cumulus rc.datapath_0: q_shared_limit_cell=2073, q_min_cell=307 */
        {
            OP_QUEUE_CONFIG_CELLr_t oqc;
            OP_QUEUE_CONFIG1_CELLr_t oqc1;
            int q;
            for (q = 0; q < 8; q++) {
                OP_QUEUE_CONFIG_CELLr_CLR(oqc);
                OP_QUEUE_CONFIG_CELLr_Q_SHARED_LIMIT_CELLf_SET(oqc, 2073);
                OP_QUEUE_CONFIG_CELLr_Q_MIN_CELLf_SET(oqc, 307);
                WRITE_OP_QUEUE_CONFIG_CELLr(unit, CMIC_PORT, q, oqc);

                OP_QUEUE_CONFIG1_CELLr_CLR(oqc1);
                OP_QUEUE_CONFIG1_CELLr_Q_LIMIT_ENABLE_CELLf_SET(oqc1, 1);
                WRITE_OP_QUEUE_CONFIG1_CELLr(unit, CMIC_PORT, q, oqc1);
            }
            syslog(LOG_INFO, "MMU: CPU OP_QUEUE_CONFIG q0-7 (min=307, shared=2073, enable=1)");
        }

        /* Disable egress VLAN filter on CPU port.
         * bmd_init sets EN_EFILTER=1 on ALL ports including CPU.
         * This causes the egress pipeline to drop frames to CPU
         * if the frame's VLAN doesn't have CPU in EGR_VLAN bitmap. */
        {
            EGR_PORTm_t egr_port;
            EGR_PORTm_CLR(egr_port);
            EGR_PORTm_EN_EFILTERf_SET(egr_port, 0);  /* DISABLE egress filter */
            WRITE_EGR_PORTm(unit, 0, egr_port);  /* lport 0 = CPU */
            syslog(LOG_INFO, "CPU port: egress VLAN filter DISABLED");

        /* TEST: Add broadcast MAC to L2_USER_ENTRY to verify TCAM is searched.
         * If we get EXTRA ARP copies (beyond protocol punt), TCAM works. */
        {
            uint32_t test_words[5];
            /* Broadcast MAC: ff:ff:ff:ff:ff:ff */
            uint64_t bcast_mac = 0xFFFFFFFFFFFFull;
            uint64_t mask = 0x1000FFFFFFFFFFFFull; /* KEY_TYPE mask + MAC mask */
            test_words[0] = 1u | ((uint32_t)(bcast_mac & 0x7FFFFFFFu) << 1);
            test_words[1] = (uint32_t)((bcast_mac >> 31) & 0x1FFFFu) |
                           (0 << 17) | (0 << 29) |
                           ((uint32_t)(mask & 3u) << 30);
            test_words[2] = (uint32_t)((mask >> 2) & 0xFFFFFFFFu);
            test_words[3] = (uint32_t)((mask >> 34) & 0x7FFFFFFFu);
            test_words[4] = 0;  /* No CPU flag in TCAM key */
            int rv3 = cdk_xgs_mem_write(unit, 0x06168000, 511, test_words, 5);
            /* Write COPY_TO_CPU to the DATA table */
            uint32_t bcast_data[2] = {0, 0};
            bcast_data[0] = (1 << 6);  /* CPUf at bit 6 */
            cdk_xgs_mem_write(unit, 0x0616c000, 511, bcast_data, 2);
            syslog(LOG_INFO, "L2_USER[511]: BROADCAST COPY_TO_CPU test (rv=%d)", rv3);
        }
        }
    }

    syslog(LOG_INFO, "CPU punt: L3 MTU/slowpath/dstmiss + ARP/DHCP enabled");
    return ioerr;
}

/*
 * RTAG7 hash configuration for ECMP and trunk load balancing.
 *
 * Uses CRC16-CCITT hash over src/dst IP, L4 ports, protocol.
 * From Cumulus rc.datapath_0 RTAG7 section.
 */
static int datapath_hash_init(int unit)
{
    int ioerr = 0;
    RTAG7_IPV4_TCP_UDP_HASH_FIELD_BMAP_2r_t ipv4_tcp_bmap2;
    RTAG7_IPV6_TCP_UDP_HASH_FIELD_BMAP_2r_t ipv6_tcp_bmap2;
    RTAG7_IPV4_TCP_UDP_HASH_FIELD_BMAP_1r_t ipv4_tcp_bmap1;
    RTAG7_IPV6_TCP_UDP_HASH_FIELD_BMAP_1r_t ipv6_tcp_bmap1;
    RTAG7_HASH_FIELD_BMAP_1r_t hash_bmap1;
    RTAG7_HASH_FIELD_BMAP_2r_t hash_bmap2;
    RTAG7_HASH_CONTROL_3r_t hash_ctrl3;
    RTAG7_HASH_SEED_Ar_t hash_seed;
    HASH_CONTROLr_t hash_control;

    /* TCP/UDP: hash src/dst IP + L4 ports + protocol + src port/mod */
    /* bitmap = 0b0111101111100 = 0x1EFC */
    ioerr += READ_RTAG7_IPV4_TCP_UDP_HASH_FIELD_BMAP_2r(unit, &ipv4_tcp_bmap2);
    RTAG7_IPV4_TCP_UDP_HASH_FIELD_BMAP_2r_IPV4_TCP_UDP_FIELD_BITMAP_Af_SET(
        ipv4_tcp_bmap2, 0x1EFC);
    ioerr += WRITE_RTAG7_IPV4_TCP_UDP_HASH_FIELD_BMAP_2r(unit, ipv4_tcp_bmap2);

    ioerr += READ_RTAG7_IPV6_TCP_UDP_HASH_FIELD_BMAP_2r(unit, &ipv6_tcp_bmap2);
    RTAG7_IPV6_TCP_UDP_HASH_FIELD_BMAP_2r_IPV6_TCP_UDP_FIELD_BITMAP_Af_SET(
        ipv6_tcp_bmap2, 0x1EFC);
    ioerr += WRITE_RTAG7_IPV6_TCP_UDP_HASH_FIELD_BMAP_2r(unit, ipv6_tcp_bmap2);

    /* src==dst L4 port special case: drop one L4 port from hash */
    /* bitmap = 0b0111100111100 = 0x1E7C */
    ioerr += READ_RTAG7_IPV4_TCP_UDP_HASH_FIELD_BMAP_1r(unit, &ipv4_tcp_bmap1);
    RTAG7_IPV4_TCP_UDP_HASH_FIELD_BMAP_1r_IPV4_TCP_UDP_SRC_EQ_DST_FIELD_BITMAP_Af_SET(
        ipv4_tcp_bmap1, 0x1E7C);
    ioerr += WRITE_RTAG7_IPV4_TCP_UDP_HASH_FIELD_BMAP_1r(unit, ipv4_tcp_bmap1);

    ioerr += READ_RTAG7_IPV6_TCP_UDP_HASH_FIELD_BMAP_1r(unit, &ipv6_tcp_bmap1);
    RTAG7_IPV6_TCP_UDP_HASH_FIELD_BMAP_1r_IPV6_TCP_UDP_SRC_EQ_DST_FIELD_BITMAP_Af_SET(
        ipv6_tcp_bmap1, 0x1E7C);
    ioerr += WRITE_RTAG7_IPV6_TCP_UDP_HASH_FIELD_BMAP_1r(unit, ipv6_tcp_bmap1);

    /* Non-TCP/UDP: hash src/dst IP + protocol + src port/mod (no L4 ports) */
    /* bitmap = 0b0111100011100 = 0x1E1C */
    ioerr += READ_RTAG7_HASH_FIELD_BMAP_1r(unit, &hash_bmap1);
    RTAG7_HASH_FIELD_BMAP_1r_IPV4_FIELD_BITMAP_Af_SET(hash_bmap1, 0x1E1C);
    ioerr += WRITE_RTAG7_HASH_FIELD_BMAP_1r(unit, hash_bmap1);

    ioerr += READ_RTAG7_HASH_FIELD_BMAP_2r(unit, &hash_bmap2);
    RTAG7_HASH_FIELD_BMAP_2r_IPV6_FIELD_BITMAP_Af_SET(hash_bmap2, 0x1E1C);
    ioerr += WRITE_RTAG7_HASH_FIELD_BMAP_2r(unit, hash_bmap2);

    /* Hash function: CRC16-CCITT (function select = 9) */
    ioerr += READ_RTAG7_HASH_CONTROL_3r(unit, &hash_ctrl3);
    RTAG7_HASH_CONTROL_3r_HASH_A0_FUNCTION_SELECTf_SET(hash_ctrl3, 9);
    ioerr += WRITE_RTAG7_HASH_CONTROL_3r(unit, hash_ctrl3);

    /* Hash seed */
    RTAG7_HASH_SEED_Ar_CLR(hash_seed);
    RTAG7_HASH_SEED_Ar_SET(hash_seed, 42);
    ioerr += WRITE_RTAG7_HASH_SEED_Ar(unit, hash_seed);

    /* Enable RTAG7 for ECMP, use TCP/UDP ports for trunk, L3 hash = CRC16 */
    ioerr += READ_HASH_CONTROLr(unit, &hash_control);
    HASH_CONTROLr_ECMP_HASH_USE_RTAG7f_SET(hash_control, 1);
    HASH_CONTROLr_USE_TCP_UDP_PORTSf_SET(hash_control, 1);
    HASH_CONTROLr_L3_HASH_SELECTf_SET(hash_control, 4);
    HASH_CONTROLr_NON_UC_TRUNK_HASH_USE_RTAG7f_SET(hash_control, 1);
    ioerr += WRITE_HASH_CONTROLr(unit, hash_control);

    syslog(LOG_INFO, "RTAG7 hash: CRC16-CCITT, ECMP+trunk enabled");
    return ioerr;
}

/*
 * MAC and CMIC register configuration from Cumulus rc.soc.
 *
 * These registers are set by Cumulus after "init all" and before
 * datapath configuration. Without them, links may not come up.
 *
 * From traces/cumulus_rc.soc:
 *   setreg xmac_tx_ctrl 0xc802
 *   s MAC_RSV_MASK MASK=0x18
 *   m cmic_misc_control LINK40G_ENABLE=1
 *   setreg IFP_METER_PARITY_CONTROL 0
 */
static int datapath_mac_init(int unit)
{
    int ioerr = 0;
    int port;
    cdk_pbmp_t pbmp;

    /*
     * xmac_tx_ctrl = 0xc802 on all XE ports.
     * Controls MAC TX behavior including CRC mode and pad enable.
     * 0xc802 = CRC_MODE=2 (replace), TX_ANY_START=1, PAD_EN=1
     */
    XMAC_TX_CTRLr_t xmac_tx_ctrl;
    bcm56840_a0_xlport_pbmp_get(unit, &pbmp);
    CDK_PBMP_ITER(pbmp, port) {
        XMAC_TX_CTRLr_CLR(xmac_tx_ctrl);
        XMAC_TX_CTRLr_SET(xmac_tx_ctrl, 0, 0xc802);
        ioerr += WRITE_XMAC_TX_CTRLr(unit, port, xmac_tx_ctrl);
    }
    syslog(LOG_INFO, "MAC: xmac_tx_ctrl=0xc802 on all XE ports");

    /*
     * MAC_RSV_MASK = 0x18 on all ports.
     * Controls which reserved frame types are dropped vs forwarded.
     * 0x18 = pass BPDU frames, drop other reserved MACs.
     */
    MAC_RSV_MASKr_t mac_rsv;
    CDK_PBMP_ITER(pbmp, port) {
        MAC_RSV_MASKr_SET(mac_rsv, 0x18);
        ioerr += WRITE_MAC_RSV_MASKr(unit, port, mac_rsv);
    }
    syslog(LOG_INFO, "MAC: MAC_RSV_MASK=0x18 on all ports");

    /*
     * cmic_misc_control LINK40G_ENABLE=1
     * Enables 40G link status detection in CMIC.
     * Without this, 40G QSFP ports may not report link-up.
     */
    CMIC_MISC_CONTROLr_t cmic_misc;
    ioerr += READ_CMIC_MISC_CONTROLr(unit, &cmic_misc);
    CMIC_MISC_CONTROLr_LINK40G_ENABLEf_SET(cmic_misc, 1);
    ioerr += WRITE_CMIC_MISC_CONTROLr(unit, cmic_misc);
    syslog(LOG_INFO, "MAC: cmic_misc_control LINK40G_ENABLE=1");

    /*
     * IFP_METER_PARITY_CONTROL = 0
     * Errata workaround for Trident — avoids false FP meter parity errors.
     */
    IFP_METER_PARITY_CONTROLr_t ifp_meter;
    IFP_METER_PARITY_CONTROLr_SET(ifp_meter, 0);
    ioerr += WRITE_IFP_METER_PARITY_CONTROLr(unit, ifp_meter);
    syslog(LOG_INFO, "MAC: IFP_METER_PARITY_CONTROL=0 (errata)");

    return ioerr;
}

/*
 * Initialize datapath configuration.
 *
 * Called after bmd_switching_init() and portmap_configure_ports().
 * Applies MAC config, CPU punt rules, hash config, and basic buffer setup.
 *
 * Note: Full buffer threshold configuration (THDO tables from
 * rc.datapath_0) is not yet implemented. The ASIC defaults should
 * allow basic packet forwarding; full QoS tuning can be added later.
 */
int datapath_init(void)
{
    int ioerr = 0;

    ioerr += datapath_mac_init(switchd.unit);
    ioerr += datapath_cpu_punt_init(switchd.unit);
    ioerr += datapath_hash_init(switchd.unit);

    if (ioerr) {
        syslog(LOG_ERR, "Datapath init had %d I/O errors", ioerr);
        return -1;
    }

    syslog(LOG_INFO, "Datapath configuration complete");
    return 0;
}
