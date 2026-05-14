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

#include "edged.h"

#include <cdk/chip/bcm56840_a0_defs.h>
#include <cdk/arch/xgs_chip.h>

/* From bcm56840_a0_internal.h */
extern int bcm56840_a0_xlport_pbmp_get(int unit, cdk_pbmp_t *pbmp);

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

    /*
     * RX/TX drop counter disaggregation (matches Cumulus rc.soc).
     * These coupling registers route per-reason drop events into the
     * RDBGCn / TDBGCn counters so ethtool -S can report:
     *   rdbgc0 (aggregated): RIPD4+RIPD6+RDISC+RPORTD+PDISC+VLANDR
     *   rdbgc3: RIPD4+RIPD6  (IPv4/IPv6 header drops)
     *   rdbgc4: RDISC        (discard)
     *   rdbgc5: RFILDR       (filter drop)
     *   rdbgc6: RDROP        (generic RX drop)
     *   tdbgc6: TPKTD        (TX packet drop)
     * From cumulus_baseline_2013/binaries/extracted/etc/bcm.d/rc.soc.
     */
    /*
     * RDBGC*_SELECT and TDBGC6_SELECT are global registers (one per
     * chip, not per port). The Cumulus rc.soc issues exactly one
     * `setreg <reg> <val>` for each — same here.
     */
    {
        RDBGC0_SELECTr_t r0; RDBGC3_SELECTr_t r3;
        RDBGC4_SELECTr_t r4; RDBGC5_SELECTr_t r5;
        RDBGC6_SELECTr_t r6; TDBGC6_SELECTr_t t6;

        RDBGC0_SELECTr_SET(r0, 0x04000d11);
        ioerr += WRITE_RDBGC0_SELECTr(unit, r0);
        RDBGC3_SELECTr_SET(r3, 0x00000011);
        ioerr += WRITE_RDBGC3_SELECTr(unit, r3);
        RDBGC4_SELECTr_SET(r4, 0x00000100);
        ioerr += WRITE_RDBGC4_SELECTr(unit, r4);
        RDBGC5_SELECTr_SET(r5, 0x00002000);
        ioerr += WRITE_RDBGC5_SELECTr(unit, r5);
        RDBGC6_SELECTr_SET(r6, 0x00008000);
        ioerr += WRITE_RDBGC6_SELECTr(unit, r6);
        TDBGC6_SELECTr_SET(t6, 0x00040000);
        ioerr += WRITE_TDBGC6_SELECTr(unit, t6);
        syslog(LOG_INFO,
               "MAC: drop-counter select wired (rdbgc0/3/4/5/6, tdbgc6)");
    }

    return ioerr;
}

/*
 * MMU service-pool / priority-group buffer configuration.
 *
 * Captured from Cumulus rc.datapath_0 for the AS5610-52X (BCM56846,
 * 46080 total cells of buffer memory).  These settings are the minimum
 * subset required to keep CPU punt + line-rate forwarding from dropping
 * under burst load.  The full rc.datapath_0 has ~140 register writes
 * (per-port PG min cells, per-CoS shared limits, MMU scheduler weights);
 * those are TODO and the chip defaults stand in until they are ported.
 *
 * Source: cumulus_baseline_2013/switchd-generated-state/rc.datapath_0
 * Decoded: cumulus_baseline_2013/ASIC_INIT_COOKBOOK.md §6.
 */
static int datapath_buffer_init(int unit)
{
    int ioerr = 0;

    /* Disable color-aware admission globally — Cumulus default. */
    {
        COLOR_AWAREr_t v;
        COLOR_AWAREr_CLR(v);
        ioerr += WRITE_COLOR_AWAREr(unit, v);
    }

    /* Service pool cell limits (46080 total cells available):
     *   SP0 = 0        (disabled)
     *   SP1 = 1382     (main bulk traffic)
     *   SP2 = 921      (priority traffic)
     *   SP3 = 0        (disabled)
     * Plus cell_reset_limit_offset = 100 cells of hysteresis. */
    {
        BUFFER_CELL_LIMIT_SPr_t v;
        CELL_RESET_LIMIT_OFFSET_SPr_t h;
        unsigned int sp_limit[4] = { 0, 1382, 921, 0 };
        unsigned int sp_hyst[4]  = { 0,  100, 100, 0 };
        int sp;
        for (sp = 0; sp < 4; sp++) {
            BUFFER_CELL_LIMIT_SPr_CLR(v);
            BUFFER_CELL_LIMIT_SPr_LIMITf_SET(v, sp_limit[sp]);
            ioerr += WRITE_BUFFER_CELL_LIMIT_SPr(unit, sp, v);
            CELL_RESET_LIMIT_OFFSET_SPr_CLR(h);
            CELL_RESET_LIMIT_OFFSET_SPr_SET(h, sp_hyst[sp]);
            ioerr += WRITE_CELL_RESET_LIMIT_OFFSET_SPr(unit, sp, h);
        }
    }

    /* Global shared pool — 22742 cells. */
    {
        BUFFER_CELL_LIMIT_SP_SHAREDr_t v;
        BUFFER_CELL_LIMIT_SP_SHAREDr_CLR(v);
        BUFFER_CELL_LIMIT_SP_SHAREDr_SET(v, 22742);
        ioerr += WRITE_BUFFER_CELL_LIMIT_SP_SHAREDr(unit, v);
    }

    /* Global headroom buffer for absorbed bursts — 2340 cells. */
    {
        GLOBAL_HDRM_LIMITr_t v;
        GLOBAL_HDRM_LIMITr_CLR(v);
        GLOBAL_HDRM_LIMITr_SET(v, 2340);
        ioerr += WRITE_GLOBAL_HDRM_LIMITr(unit, v);
    }

    syslog(LOG_INFO,
           "MMU: SP1=1382 SP2=921 shared=22742 hdrm=2340 (Cumulus values)");
    return ioerr;
}

/*
 * Initialize datapath configuration.
 *
 * Called after bmd_switching_init() and portmap_configure_ports().
 * Applies MAC config, MMU buffer pools, CPU punt rules, and hash.
 *
 * Note: Per-port PG min/shared cell limits (the 40-port iteration in
 * rc.datapath_0) are still TODO. Chip defaults work for low-rate
 * forwarding and CPU punt; need real values for full line-rate.
 */
/*
 * CPU port (MMU port 0) buffer + queue allocation.
 *
 * Without these writes the chip's MMU has no buffer cells assigned to
 * the CMIC/CPU port, so any frame the forwarding pipeline tries to
 * enqueue for CPU is silently dropped at the MMU stage — no rx_drops
 * counter, no error, the CMICm DCB ring just never fills.  Identified
 * 2026-05-14 by adding a stat probe on chip port 0 vs swp1: swp1 chip
 * RX counters climbed, port-0 stats stayed at zero, CMICm IRQ count
 * stayed at zero.
 *
 * Values are exactly what Cumulus writes via `rc.datapath_0`:
 *   pg_min_cell[0/2/7].cpu0       = 45
 *   op_queue_config_cell[0..6].cpu0  q_min=307  q_shared_limit=2073
 *   op_queue_config1_cell[0/1/3/4/5/6].cpu0   q_spid=0 q_limit_en=1
 *   op_queue_config1_cell[2].cpu0  q_spid=2
 *   op_queue_config1_cell[7].cpu0  q_spid=1
 *   op_queue_config_cell[32/33/34].cpu0  q_min={100,1,500}
 */
static int datapath_cpu_buffer_init(int unit)
{
    int ioerr = 0;
    int mport = 0;        /* CMIC_MPORT for bcm56840 */
    int i;

    /* pg_min_cell[0/2/7].cpu0 = 45 */
    {
        PG_MIN_CELLr_t pg;
        int pg_idx[] = {0, 2, 7};
        for (i = 0; i < 3; i++) {
            PG_MIN_CELLr_CLR(pg);
            PG_MIN_CELLr_PG_MINf_SET(pg, 45);
            ioerr += WRITE_PG_MIN_CELLr(unit, mport, pg_idx[i], pg);
        }
    }

    /* op_queue_config_cell + op_queue_config1_cell for queues 0..7 */
    {
        OP_QUEUE_CONFIG_CELLr_t  qc;
        OP_QUEUE_CONFIG1_CELLr_t qc1;
        /* Queues with min=307 shared=2073 q_spid=0 q_limit_en=1:
         * idx 0, 1, 3, 4, 5, 6.  Queue 2 is q_spid=2 only,
         * queue 7 is q_spid=1 only. */
        int fwd_q[] = {0, 1, 3, 4, 5, 6};
        for (i = 0; i < (int)(sizeof(fwd_q)/sizeof(fwd_q[0])); i++) {
            int q = fwd_q[i];
            OP_QUEUE_CONFIG_CELLr_CLR(qc);
            OP_QUEUE_CONFIG_CELLr_Q_MIN_CELLf_SET(qc, 307);
            OP_QUEUE_CONFIG_CELLr_Q_SHARED_LIMIT_CELLf_SET(qc, 2073);
            ioerr += WRITE_OP_QUEUE_CONFIG_CELLr(unit, mport, q, qc);

            OP_QUEUE_CONFIG1_CELLr_CLR(qc1);
            OP_QUEUE_CONFIG1_CELLr_Q_SPIDf_SET(qc1, 0);
            OP_QUEUE_CONFIG1_CELLr_Q_LIMIT_ENABLE_CELLf_SET(qc1, 1);
            ioerr += WRITE_OP_QUEUE_CONFIG1_CELLr(unit, mport, q, qc1);
        }
        /* Queue 2: q_spid=2 only */
        OP_QUEUE_CONFIG1_CELLr_CLR(qc1);
        OP_QUEUE_CONFIG1_CELLr_Q_SPIDf_SET(qc1, 2);
        ioerr += WRITE_OP_QUEUE_CONFIG1_CELLr(unit, mport, 2, qc1);
        /* Queue 7: q_spid=1 only */
        OP_QUEUE_CONFIG1_CELLr_CLR(qc1);
        OP_QUEUE_CONFIG1_CELLr_Q_SPIDf_SET(qc1, 1);
        ioerr += WRITE_OP_QUEUE_CONFIG1_CELLr(unit, mport, 7, qc1);

        /* Queues 32, 33, 34: q_min={100, 1, 500} only */
        OP_QUEUE_CONFIG_CELLr_CLR(qc);
        OP_QUEUE_CONFIG_CELLr_Q_MIN_CELLf_SET(qc, 100);
        ioerr += WRITE_OP_QUEUE_CONFIG_CELLr(unit, mport, 32, qc);
        OP_QUEUE_CONFIG_CELLr_CLR(qc);
        OP_QUEUE_CONFIG_CELLr_Q_MIN_CELLf_SET(qc, 1);
        ioerr += WRITE_OP_QUEUE_CONFIG_CELLr(unit, mport, 33, qc);
        OP_QUEUE_CONFIG_CELLr_CLR(qc);
        OP_QUEUE_CONFIG_CELLr_Q_MIN_CELLf_SET(qc, 500);
        ioerr += WRITE_OP_QUEUE_CONFIG_CELLr(unit, mport, 34, qc);
    }

    syslog(LOG_INFO,
           "CPU buffer: pg_min[0/2/7]=45, op_q[0..7]=307/2073, q[32/33/34]=100/1/500");
    return ioerr;
}

/*
 * Disable VLAN translation on every port (including CPU port 0).
 *
 * Cumulus does this via `modify port 0 67 port_pri=0 pri_mapping=0
 * trust_incoming_vid=0 vt_enable=0` in rc.datapath_0.  Defaults
 * have VT_ENABLE=1 which forces every ingress frame through the
 * VLAN-translation lookup; with no VT rules programmed, that lookup
 * fails and the frame is silently dropped before bridging.
 */
static int datapath_disable_vt(int unit)
{
    int ioerr = 0;
    int p;
    for (p = 0; p <= 67; p++) {
        LPORT_TABm_t lpt;
        if (READ_LPORT_TABm(unit, p, &lpt) != 0) {
            continue;
        }
        LPORT_TABm_VT_ENABLEf_SET(lpt, 0);
        LPORT_TABm_VT_MISS_DROPf_SET(lpt, 0);
        LPORT_TABm_TRUST_INCOMING_VIDf_SET(lpt, 0);
        LPORT_TABm_PORT_PRIf_SET(lpt, 0);
        ioerr += WRITE_LPORT_TABm(unit, p, lpt);
    }
    syslog(LOG_INFO,
           "LPORT_TAB: VT_ENABLE=0 VT_MISS_DROP=0 TRUST_INCOMING_VID=0 "
           "PORT_PRI=0 on ports 0..67 (mirrors Cumulus)");
    return ioerr;
}

/*
 * Systematic port of Cumulus rc.datapath_0 — the bits not already
 * covered by datapath_buffer_init / datapath_cpu_buffer_init.
 *
 * Each block has the original Cumulus DSL line as a comment so the
 * mapping is auditable.  Without this set, the chip drops frames at
 * the MMU/bridge stage before they reach the CPU port (verified
 * 2026-05-14: chip RX counter increments, CMICm DCB ring never fills,
 * rx_drops=0).
 */
static int datapath_rc_full(int unit)
{
    int ioerr = 0;
    int p, port;
    cdk_pbmp_t xlpbmp;

    bcm56840_a0_xlport_pbmp_get(unit, &xlpbmp);

    /* port_pg_spid pg0_spid=0 pg1_spid=0 pg2_spid=2 pg3_spid=0
     * pg4_spid=0 pg5_spid=0 pg6_spid=0 pg7_spid=1 — per-PG service
     * pool mapping.  Default has every PG → SP3 (which we disable).
     * Without this, every ingress frame lands in SP3 → drop. */
    {
        PORT_PG_SPIDr_t v;
        PORT_PG_SPIDr_CLR(v);
        PORT_PG_SPIDr_PG0_SPIDf_SET(v, 0);
        PORT_PG_SPIDr_PG1_SPIDf_SET(v, 0);
        PORT_PG_SPIDr_PG2_SPIDf_SET(v, 2);
        PORT_PG_SPIDr_PG3_SPIDf_SET(v, 0);
        PORT_PG_SPIDr_PG4_SPIDf_SET(v, 0);
        PORT_PG_SPIDr_PG5_SPIDf_SET(v, 0);
        PORT_PG_SPIDr_PG6_SPIDf_SET(v, 0);
        PORT_PG_SPIDr_PG7_SPIDf_SET(v, 1);
        /* Apply to CPU + all XLPORT ports. */
        ioerr += WRITE_PORT_PG_SPIDr(unit, 0, v);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_PORT_PG_SPIDr(unit, port, v);
        }
    }

    /* setreg use_sp_shared 0x7 — enable shared pool fallback for SP0/1/2
     * Without this, when a PG's main SP runs out of cells (or has 0
     * cells like our SP0/SP3) the chip can't fall back to the shared
     * pool, so frames are dropped at admission. */
    {
        USE_SP_SHAREDr_t v;
        USE_SP_SHAREDr_CLR(v);
        USE_SP_SHAREDr_SET(v, 0x7);
        ioerr += WRITE_USE_SP_SHAREDr(unit, v);
    }

    /* pg_shared_limit_cell per-port per-PG.
     * Cumulus: pg_shared_limit_cell(0)=4548, (2)=909, (7)=10006
     * (with pg_shared_dynamic=0).  Apply to CPU + XLPORT ports. */
    {
        PG_SHARED_LIMIT_CELLr_t v;
        struct { int pg; uint32_t lim; } pgs[] = {
            { 0, 4548 }, { 2, 909 }, { 7, 10006 },
        };
        size_t i;
        int mport;
        for (i = 0; i < sizeof(pgs)/sizeof(pgs[0]); i++) {
            PG_SHARED_LIMIT_CELLr_CLR(v);
            PG_SHARED_LIMIT_CELLr_PG_SHARED_LIMITf_SET(v, pgs[i].lim);
            /* mport 0 (CPU) */
            ioerr += WRITE_PG_SHARED_LIMIT_CELLr(unit, 0, pgs[i].pg, v);
            CDK_PBMP_ITER(xlpbmp, port) {
                mport = port;  /* phys mport — close enough for our setup */
                ioerr += WRITE_PG_SHARED_LIMIT_CELLr(unit, mport,
                                                    pgs[i].pg, v);
            }
        }
    }

    /* op_buffer_shared_limit_cell[0..3] + resume — output buffer
     * shared limits per service pool.  Defaults are 0 → no shared
     * buffer = chip can't queue anything.
     * Cumulus: [0]=20736 [1]=41472 [2]=41472 [3]=135 (with resume
     * = limit - 100). */
    {
        OP_BUFFER_SHARED_LIMIT_CELLr_t v;
        OP_BUFFER_SHARED_LIMIT_RESUME_CELLr_t r;
        uint32_t lim[4] = { 20736, 41472, 41472, 135 };
        uint32_t res[4] = { 20636, 41372, 41372,  35 };
        int sp;
        for (sp = 0; sp < 4; sp++) {
            OP_BUFFER_SHARED_LIMIT_CELLr_CLR(v);
            OP_BUFFER_SHARED_LIMIT_CELLr_SET(v, lim[sp]);
            ioerr += WRITE_OP_BUFFER_SHARED_LIMIT_CELLr(unit, sp, v);

            OP_BUFFER_SHARED_LIMIT_RESUME_CELLr_CLR(r);
            OP_BUFFER_SHARED_LIMIT_RESUME_CELLr_SET(r, res[sp]);
            ioerr += WRITE_OP_BUFFER_SHARED_LIMIT_RESUME_CELLr(unit, sp, r);
        }
    }

    /* op_queue_config_cell[0..2].$allports — per-port output queue
     * default config (for non-CPU ports; CPU-port specifics handled
     * in datapath_cpu_buffer_init).
     *   Q0:  q_shared_limit=2073 q_min=921, q_spid=0, q_limit_en=1
     *   Q1:                                 q_spid=1
     *   Q2:                                 q_spid=2 */
    {
        OP_QUEUE_CONFIG_CELLr_t qc;
        OP_QUEUE_CONFIG1_CELLr_t qc1;

        OP_QUEUE_CONFIG_CELLr_CLR(qc);
        OP_QUEUE_CONFIG_CELLr_Q_MIN_CELLf_SET(qc, 921);
        OP_QUEUE_CONFIG_CELLr_Q_SHARED_LIMIT_CELLf_SET(qc, 2073);

        OP_QUEUE_CONFIG1_CELLr_CLR(qc1);
        OP_QUEUE_CONFIG1_CELLr_Q_SPIDf_SET(qc1, 0);
        OP_QUEUE_CONFIG1_CELLr_Q_LIMIT_ENABLE_CELLf_SET(qc1, 1);

        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_OP_QUEUE_CONFIG_CELLr(unit, port, 0, qc);
            ioerr += WRITE_OP_QUEUE_CONFIG1_CELLr(unit, port, 0, qc1);

            OP_QUEUE_CONFIG1_CELLr_CLR(qc1);
            OP_QUEUE_CONFIG1_CELLr_Q_SPIDf_SET(qc1, 1);
            ioerr += WRITE_OP_QUEUE_CONFIG1_CELLr(unit, port, 1, qc1);

            OP_QUEUE_CONFIG1_CELLr_CLR(qc1);
            OP_QUEUE_CONFIG1_CELLr_Q_SPIDf_SET(qc1, 2);
            ioerr += WRITE_OP_QUEUE_CONFIG1_CELLr(unit, port, 2, qc1);
        }
    }

    /* modreg cosmask cosmaskrxen=1 — enable the COS-mask RX gate
     * Without COSMASKRXEN, the chip may filter all priority classes
     * → ALL frames dropped at RX even though the MAC accepted them.
     * COSMASK is a per-port register; apply to CPU + every XLPORT. */
    {
        COSMASKr_t v;
        ioerr += READ_COSMASKr(unit, 0, &v);
        COSMASKr_COSMASKRXENf_SET(v, 1);
        ioerr += WRITE_COSMASKr(unit, 0, v);
        CDK_PBMP_ITER(xlpbmp, port) {
            COSMASKr_t pv;
            ioerr += READ_COSMASKr(unit, port, &pv);
            COSMASKr_COSMASKRXENf_SET(pv, 1);
            ioerr += WRITE_COSMASKr(unit, port, pv);
        }
    }

    /* modreg aux_arb_control l2_mod_fifo_enable_l2_delete=0 */
    {
        AUX_ARB_CONTROLr_t v;
        ioerr += READ_AUX_ARB_CONTROLr(unit, &v);
        AUX_ARB_CONTROLr_L2_MOD_FIFO_ENABLE_L2_DELETEf_SET(v, 0);
        ioerr += WRITE_AUX_ARB_CONTROLr(unit, v);
    }

    /* setreg ing_cos_mode queue_mode=0 cos_mode=0 — per-port. */
    {
        ING_COS_MODEr_t v;
        ING_COS_MODEr_CLR(v);
        ING_COS_MODEr_QUEUE_MODEf_SET(v, 0);
        ING_COS_MODEr_COS_MODEf_SET(v, 0);
        ioerr += WRITE_ING_COS_MODEr(unit, 0, v);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_ING_COS_MODEr(unit, port, v);
        }
    }

    /* setreg op_voq_port_config q_sel_p{34..37,1..4}=0 — disable
     * VoQ port selection on these ports (so output queues are
     * straight, not VoQ-multiplexed).  We just clear the whole reg. */
    {
        OP_VOQ_PORT_CONFIGr_t v;
        OP_VOQ_PORT_CONFIGr_CLR(v);
        ioerr += WRITE_OP_VOQ_PORT_CONFIGr(unit, v);
    }

    /* modreg ovq_flowcontrol_threshold ovq_fc_enable=0 */
    {
        OVQ_FLOWCONTROL_THRESHOLDr_t v;
        ioerr += READ_OVQ_FLOWCONTROL_THRESHOLDr(unit, &v);
        OVQ_FLOWCONTROL_THRESHOLDr_OVQ_FC_ENABLEf_SET(v, 0);
        ioerr += WRITE_OVQ_FLOWCONTROL_THRESHOLDr(unit, v);
    }

    /* modreg es_tdm_config en_cpu_slot_sharing=0 */
    {
        ES_TDM_CONFIGr_t v;
        ioerr += READ_ES_TDM_CONFIGr(unit, &v);
        ES_TDM_CONFIGr_EN_CPU_SLOT_SHARINGf_SET(v, 0);
        ioerr += WRITE_ES_TDM_CONFIGr(unit, v);
    }

    /* setreg port_max_pkt_size 45 — Cumulus sets this to 45 cells
     * for jumbo support.  bmd_init already programs this but with
     * its own value; force Cumulus's 45 explicitly. */
    {
        PORT_MAX_PKT_SIZEr_t v;
        PORT_MAX_PKT_SIZEr_CLR(v);
        PORT_MAX_PKT_SIZEr_PORT_MAX_PKT_SIZEf_SET(v, 45);
        ioerr += WRITE_PORT_MAX_PKT_SIZEr(unit, 0, v);   /* CPU port */
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_PORT_MAX_PKT_SIZEr(unit, port, v);
        }
    }

    /* setreg es_queue_to_prio prio_N=N — identity mapping */
    {
        ES_QUEUE_TO_PRIOr_t v;
        ES_QUEUE_TO_PRIOr_CLR(v);
        ES_QUEUE_TO_PRIOr_PRIO_0f_SET(v, 0);
        ES_QUEUE_TO_PRIOr_PRIO_1f_SET(v, 1);
        ES_QUEUE_TO_PRIOr_PRIO_2f_SET(v, 2);
        ES_QUEUE_TO_PRIOr_PRIO_3f_SET(v, 3);
        ES_QUEUE_TO_PRIOr_PRIO_4f_SET(v, 4);
        ES_QUEUE_TO_PRIOr_PRIO_5f_SET(v, 5);
        ES_QUEUE_TO_PRIOr_PRIO_6f_SET(v, 6);
        ioerr += WRITE_ES_QUEUE_TO_PRIOr(unit, v);
    }

    /* Egress scheduler (ES) config + cosweights — per-port (incl CPU).
     * Without these the egress scheduler may never dequeue. */
    {
        ESCONFIGr_t esc;
        COSWEIGHTSr_t cw;
        ESCONFIGr_CLR(esc);
        ESCONFIGr_SCHEDULING_SELECTf_SET(esc, 0x3);
        /* CPU + every XLPORT */
        ioerr += WRITE_ESCONFIGr(unit, 0, esc);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_ESCONFIGr(unit, port, esc);
        }

        /* cosweights(0)=16, (1)=32, (2)=0 */
        struct { int i; uint32_t w; } cws[] = {
            { 0, 16 }, { 1, 32 }, { 2, 0 },
        };
        size_t k;
        for (k = 0; k < sizeof(cws)/sizeof(cws[0]); k++) {
            COSWEIGHTSr_CLR(cw);
            COSWEIGHTSr_SET(cw, cws[k].w);
            ioerr += WRITE_COSWEIGHTSr(unit, 0, cws[k].i, cw);
            CDK_PBMP_ITER(xlpbmp, port) {
                ioerr += WRITE_COSWEIGHTSr(unit, port, cws[k].i, cw);
            }
        }
    }

    /* S3 scheduler config + minspconfig + S3_CONFIG_MC.use_mc_group=0
     * — top-level egress scheduler for each port (incl CPU). */
    {
        S3_CONFIGr_t s3c;
        S3_CONFIG_MCr_t s3mc;
        S3_CONFIGr_CLR(s3c);
        S3_CONFIGr_ROUTE_UC_TO_S2f_SET(s3c, 1);
        S3_CONFIGr_SCHEDULING_SELECTf_SET(s3c, 0xff);
        S3_CONFIG_MCr_CLR(s3mc);
        /* USE_MC_GROUP field default 0, leave it. */
        ioerr += WRITE_S3_CONFIGr(unit, 0, s3c);
        ioerr += WRITE_S3_CONFIG_MCr(unit, 0, s3mc);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_S3_CONFIGr(unit, port, s3c);
            ioerr += WRITE_S3_CONFIG_MCr(unit, port, s3mc);
        }
    }

    /* S2 scheduler config + cosweights/routing — per-port (incl CPU). */
    {
        S2_CONFIGr_t s2c;
        S2_CONFIGr_CLR(s2c);
        S2_CONFIGr_SCHEDULING_SELECTf_SET(s2c, 0x3f);
        ioerr += WRITE_S2_CONFIGr(unit, 0, s2c);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_S2_CONFIGr(unit, port, s2c);
        }
    }

    /* Suppress unused-variable warning */
    (void)p;

    syslog(LOG_INFO,
           "rc_full: PG_SPID, USE_SP_SHARED, OP_BUFFER_SHARED, "
           "OP_QUEUE_CONFIG.allports, COSMASKRXEN, AUX_ARB_CONTROL, "
           "ING_COS_MODE, OP_VOQ_PORT_CFG, OVQ_FC, ES_TDM_CONFIG, "
           "PORT_MAX_PKT_SIZE, ES_QUEUE_TO_PRIO, ESCONFIG, COSWEIGHTS, "
           "S3_CONFIG, S2_CONFIG (Cumulus rc.datapath_0 mirror)");
    return ioerr;
}

int datapath_init(void)
{
    int ioerr = 0;

    ioerr += datapath_mac_init(edged.unit);
    ioerr += datapath_buffer_init(edged.unit);
    ioerr += datapath_cpu_buffer_init(edged.unit);
    ioerr += datapath_rc_full(edged.unit);
    ioerr += datapath_disable_vt(edged.unit);
    ioerr += datapath_cpu_punt_init(edged.unit);
    ioerr += datapath_hash_init(edged.unit);

    if (ioerr) {
        syslog(LOG_ERR, "Datapath init had %d I/O errors", ioerr);
        return -1;
    }

    syslog(LOG_INFO, "Datapath configuration complete");
    return 0;
}
