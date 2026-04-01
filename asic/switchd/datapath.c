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

    /* Disable MAC learning on ALL ports and flush stale L2 entries.
     * bmd_init sets CML=8 (HW learn) during bmd_switching_init.
     * By the time we get here, the ASIC may have already learned
     * our TX source MACs on the wrong ports. Clear CML and flush. */
    {
        static uint32_t ptab[10];
        int p, rv;
        for (p = 0; p <= 72; p++) {
            memset(ptab, 0, sizeof(ptab));
            rv = cdk_xgs_mem_read(unit, PORT_TABm, p, ptab, 10);
            if (rv != 0) continue;
            ptab[4] &= ~(0x1FE);  /* CML_FLAGS_NEW=0, CML_FLAGS_MOVE=0 */
            cdk_xgs_mem_write(unit, PORT_TABm, p, ptab, 10);
        }

        /* Flush all dynamic L2 entries via L2_BULK_CONTROL */
        {
            L2_BULK_CONTROLr_t l2bc;
            L2_BULK_CONTROLr_CLR(l2bc);
            /* Delete all non-static entries: NUM_ENTRIES=0 (all), ACTION bits */
            L2_BULK_CONTROLr_SET(l2bc, (1 << 1));  /* L2_MOD_FIFO_ENABLE + action */
            WRITE_L2_BULK_CONTROLr(unit, l2bc);
        }

        syslog(LOG_INFO, "All ports CML=0, dynamic L2 entries flushed");

        /* Write VLAN 1 directly to the REAL ingress VLAN table (QVLAN).
         *
         * CDK's VLAN_TABm (0x05174000) is NOT the ingress lookup table.
         * The actual table is QVLAN at S-Channel address 0x12168000
         * (basetype=2, block=ipipe0, confirmed from Cumulus RE).
         *
         * Format (10 words, from Cumulus VLAN_TABLE_FORMAT.md):
         *   w0: PORT_BITMAP[31:0]  - bit0=CPU, bit1=xe0/swp1, ...
         *   w1: PORT_BITMAP[63:32]
         *   w2: PORT_BITMAP[65:64] + ING_PORT_BITMAP[31:2] (bits [31:2])
         *   w3: ING_PORT_BITMAP[63:32]
         *   w4: ING_PORT_BITMAP[65:64] (bits [1:0]) + STG[12:4]
         *   w5: (other fields)
         *   w6: VALID at bit 13
         *   w7: VLAN_PROFILE_PTR at bits [14:8]
         *   w8-w9: (other fields)
         */
        {
            static uint32_t qvlan[10];
            memset(qvlan, 0, sizeof(qvlan));

            /* First read current state */
            rv = cdk_xgs_mem_read(unit, 0x12168000, 1, qvlan, 10);
            syslog(LOG_INFO, "QVLAN[1] read: w0=0x%08x w2=0x%08x w4=0x%08x w6=0x%08x (rv=%d)",
                   qvlan[0], qvlan[2], qvlan[4], qvlan[6], rv);

            /* Set all 52 ports + CPU in PORT_BITMAP */
            qvlan[0] = 0xFFFFFFFF;  /* ports 0-31 (CPU + swp1-31) */
            qvlan[1] = 0x001FFFFF;  /* ports 32-52 (swp32-52) */
            qvlan[2] = (qvlan[2] & ~0x3) | 0x0;  /* PORT_BITMAP bits 65:64 = 0 */

            /* Set same for ING_PORT_BITMAP (at bits 66-131) */
            qvlan[2] |= (0xFFFFFFFF << 2);  /* ING bits 0-29 at w2[31:2] */
            qvlan[3] = 0xFFFFFFFF;           /* ING bits 30-61 */
            qvlan[4] = (qvlan[4] & ~0x3) | 0x3; /* ING bits 62-63 at w4[1:0] */

            /* STG = 1 (default spanning tree group) at w4[12:4] */
            qvlan[4] = (qvlan[4] & ~(0x1FF << 4)) | (1 << 4);

            /* VALID = 1 at w6[13] */
            qvlan[6] |= (1 << 13);

            /* VLAN_PROFILE_PTR = max (from bmd_vlan_create) at w7[14:8] */
            qvlan[7] = (qvlan[7] & ~(0x7F << 8)) | (0x7F << 8);

            rv = cdk_xgs_mem_write(unit, 0x12168000, 1, qvlan, 10);
            syslog(LOG_INFO, "QVLAN[1] write: rv=%d", rv);

            /* Verify */
            memset(qvlan, 0, sizeof(qvlan));
            cdk_xgs_mem_read(unit, 0x12168000, 1, qvlan, 10);
            syslog(LOG_INFO, "QVLAN[1] verify: w0=0x%08x w6=0x%08x valid=%d cpu=%d",
                   qvlan[0], qvlan[6], (qvlan[6] >> 13) & 1, qvlan[0] & 1);

            /* Also check and fix EGR_VLAN at Cumulus address 0x0d260000 */
            static uint32_t egr[8];
            memset(egr, 0, sizeof(egr));
            rv = cdk_xgs_mem_read(unit, 0x0d260000, 1, egr, 8);
            syslog(LOG_INFO, "EGR_VLAN[1] read: w0=0x%08x w1=0x%08x w2=0x%08x (rv=%d)",
                   egr[0], egr[1], egr[2], rv);
            /* Set PORT_BITMAP with CPU port if not already there */
            egr[0] |= 0xFFFFFFFF; /* all ports */
            egr[1] |= 0x001FFFFF;
            /* VALID + STG */
            egr[3] |= (1 << 0); /* VALID might be at different position */
            rv = cdk_xgs_mem_write(unit, 0x0d260000, 1, egr, 8);
            syslog(LOG_INFO, "EGR_VLAN[1] write: rv=%d", rv);

            /* Configure MMU output queue for CPU port.
             * Without this, the CPU port has zero buffer credits and
             * ALL L2-forwarded frames get dropped in the MMU.
             * (ARP works because protocol punt bypasses MMU queuing.)
             *
             * From Cumulus rc.datapath_0:
             *   q_shared_limit_cell=2073, q_min_cell=307, q_limit_enable=1
             */
            /* Configure MMU for CPU port (mport=0).
             * bmd_init SKIPS mport=0 in its MMU loop.
             * CPU uses MMU_THDO_CONFIG_EX (extended queue table).
             * mport=0 base = (0 - 0) * 74 = 0; queues at index 0-73.
             * Normal COS queues at index 64-73 (base + 64).
             * From Cumulus: q_shared_limit=2073, q_min=307, q_limit_enable=1
             */
            {
                MMU_THDO_CONFIG_EX_0m_t thdo_ex0;
                int q;
                /* CPU mport=0: queue base = 0, normal queues at offset 64 */
                for (q = 0; q < 10; q++) {
                    MMU_THDO_CONFIG_EX_0m_CLR(thdo_ex0);
                    MMU_THDO_CONFIG_EX_0m_Q_SHARED_LIMIT_CELLf_SET(thdo_ex0, 2073);
                    MMU_THDO_CONFIG_EX_0m_Q_MIN_CELLf_SET(thdo_ex0, 307);
                    MMU_THDO_CONFIG_EX_0m_Q_LIMIT_ENABLE_CELLf_SET(thdo_ex0, 1);
                    WRITE_MMU_THDO_CONFIG_EX_0m(unit, 64 + q, thdo_ex0);
                }
                syslog(LOG_INFO, "MMU: CPU port EX queues 64-73 configured (min=307, shared=2073)");
            }

            /* Check EGR_ENABLE for CPU port (index 0) */
            uint32_t egr_en = 0;
            cdk_xgs_mem_read(unit, EGR_ENABLEm, 0, &egr_en, 1);
            syslog(LOG_INFO, "EGR_ENABLE[0] = 0x%08x (PRT_ENABLE=%d)", egr_en, egr_en & 1);
            if (!(egr_en & 1)) {
                egr_en |= 1;
                cdk_xgs_mem_write(unit, EGR_ENABLEm, 0, &egr_en, 1);
                syslog(LOG_INFO, "EGR_ENABLE[0]: CPU port ENABLED");
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
