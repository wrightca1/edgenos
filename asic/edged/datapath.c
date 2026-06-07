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
#include "vlan.h"

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

    /* Send unregistered (link-local) multicast to CPU. This is what delivers
     * routing-protocol hellos — OSPF AllSPFRouters 224.0.0.5 / AllDRouters
     * 224.0.0.6 (and similar) are unregistered L2 multicast with no IGMP group,
     * so without this the daemon never hears its neighbors. Mirrors Cumulus's
     * CPU_CONTROL_1 UMC trap. */
    CPU_CONTROL_1r_UMC_TOCPUf_SET(cpu_ctrl1, 1);

    /* IP-multicast port miss to CPU. swp1/swp2 are L3 interfaces, so the chip
     * takes IPv4 link-local multicast (224.0.0.5/6) down the IPMC path, not L2 —
     * with no IPMC group it's an IPMC lookup miss and gets dropped. UMC alone
     * doesn't catch it (that's the L2 path). This is the bit that actually
     * delivers OSPF hellos on an L3 port (confirmed: Nexus stuck in INIT because
     * our hellos egress fine but its hellos never reached our CPU). Cumulus set
     * this too. */
    CPU_CONTROL_1r_IPMCPORTMISS_TOCPUf_SET(cpu_ctrl1, 1);

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

    /* L3_DEFIP TCAM enable — REQUIRED for the L3 LPM (and our local-host
     * /32 -> CPU DEFIP entries) to be consulted at all.  bmd_init leaves these
     * at 0 (DEFIP CAM disabled), so every L3-terminated IPv4 frame finds NO
     * route and is discarded as RIPD4 — this is the root cause of the cold-boot
     * ICMP-to-self drop (2026-06-04).  Broadcom 'init all' sets
     * L3_DEFIP_CAM_ENABLE=0x3ff and L3_DEFIP_128_CAM_ENABLE=0x3 (verified in the
     * Cumulus SOC reg dump).  We replicate that here. */
    {
        L3_DEFIP_CAM_ENABLEr_t ce;
        L3_DEFIP_128_CAM_ENABLEr_t ce128;
        ioerr += READ_L3_DEFIP_CAM_ENABLEr(unit, &ce);
        ioerr += READ_L3_DEFIP_128_CAM_ENABLEr(unit, &ce128);
        syslog(LOG_INFO,
               "L3_DEFIP_CAM_ENABLE before: 0x%x (128-CAM: 0x%x)",
               L3_DEFIP_CAM_ENABLEr_GET(ce),
               L3_DEFIP_128_CAM_ENABLEr_GET(ce128));
        L3_DEFIP_CAM_ENABLEr_CAM_0_ENABLEf_SET(ce, 1);
        L3_DEFIP_CAM_ENABLEr_CAM_1_ENABLEf_SET(ce, 1);
        L3_DEFIP_CAM_ENABLEr_CAM_2_ENABLEf_SET(ce, 1);
        L3_DEFIP_CAM_ENABLEr_CAM_3_ENABLEf_SET(ce, 1);
        L3_DEFIP_CAM_ENABLEr_CAM_4_ENABLEf_SET(ce, 1);
        L3_DEFIP_CAM_ENABLEr_CAM_5_ENABLEf_SET(ce, 1);
        L3_DEFIP_CAM_ENABLEr_CAM_6_ENABLEf_SET(ce, 1);
        L3_DEFIP_CAM_ENABLEr_CAM_7_ENABLEf_SET(ce, 1);
        L3_DEFIP_CAM_ENABLEr_DIP_CAMSf_SET(ce, 3);
        ioerr += WRITE_L3_DEFIP_CAM_ENABLEr(unit, ce);
        L3_DEFIP_128_CAM_ENABLEr_CAM_0_ENABLEf_SET(ce128, 1);
        L3_DEFIP_128_CAM_ENABLEr_CAM_1_ENABLEf_SET(ce128, 1);
        ioerr += WRITE_L3_DEFIP_128_CAM_ENABLEr(unit, ce128);
        ioerr += READ_L3_DEFIP_CAM_ENABLEr(unit, &ce);
        syslog(LOG_INFO, "L3_DEFIP_CAM_ENABLE after: 0x%x (target 0x3ff)",
               L3_DEFIP_CAM_ENABLEr_GET(ce));
    }

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
     * ING_CONFIG_64 — ingress pipeline master config.  soc_init FOUNDATION
     * that OpenMDK's bmd_init AND edged never set (verified 2026-06-01).
     * At its reset default the ingress pipeline produces no L2/L3 forward
     * decision, so received & CPU-injected frames are RDROP'd at ingress
     * (the core blocker).  Values from OpenBCM soc_trident_misc_init():
     *   L3SRC_HIT_ENABLE=1, L2DST_HIT_ENABLE=1, APPLY_EGR_MASK_ON_L2/L3=1,
     *   ARP_RARP_TO_FP=3, ARP_VALIDATION_EN=1.
     */
    {
        ING_CONFIG_64r_t ingc;
        int rd = READ_ING_CONFIG_64r(unit, &ingc);
        /*
         * Keep ONLY the safe hit-enable bits.  Cumulus's ING_CONFIG_64 also
         * sets APPLY_EGR_MASK_ON_L2/L3, ARP_RARP_TO_FP=3 and ARP_VALIDATION_EN
         * (full value 0x000401802080300e) -- but those depend on subsystems
         * Cumulus has up and we do NOT:
         *   - ARP_RARP_TO_FP / ARP_VALIDATION_EN divert ARP/RARP into the VFP
         *     for validation.  Our field processor is uninitialised, so ARP
         *     gets shunted to a dead FP and dropped instead of L2-flooded
         *     (this is why the swp47->swp48 ARP loopback RDROP'd).
         *   - APPLY_EGR_MASK_ON_L2/L3 AND the L2/L3 flood with the source
         *     port's EGR_MASK, which edged never populates -> flood masked to
         *     empty -> generic RDROP.
         * Until the FP and egress masks are programmed, leave these OFF so
         * broadcast/multicast fall back to plain VLAN flooding.
         */
        ING_CONFIG_64r_L3SRC_HIT_ENABLEf_SET(ingc, 1);
        ING_CONFIG_64r_L2DST_HIT_ENABLEf_SET(ingc, 1);
        ioerr += WRITE_ING_CONFIG_64r(unit, ingc);
        syslog(LOG_INFO,
               "MAC: ING_CONFIG_64 L2DST/L3SRC hit-enable only "
               "(ARP_TO_FP/EGR_MASK OFF until FP+masks init'd) (rd=%d)",
               rd);
    }

    /*
     * VLAN_PROFILE[127] L2_PFM fix — THE RDROP ROOT CAUSE (2026-06-01).
     * OpenMDK's bmd_init writes VLAN_PROFILE_TABm[127] with L2_PFM=1 (and
     * L3_*_PFM=1), and bmd_vlan_create points every VLAN at profile 127.
     * L2_PFM=1 restricts L2 flooding, so broadcast (ARP) / multicast (OSPF)
     * / unknown frames get NO flood destination and are RDROP'd at ingress
     * (never reach CPU -> punt2cpu=0, the core blocker).  Cumulus's working
     * profile (VLAN_PROFILE.ipipe0[2]) uses L2_PFM=0 (flood to VLAN members).
     * Rewrite profile 127 to match Cumulus: PFM=0, L3/IPMC enables on,
     * LEARN_DISABLE=1.
     */
    {
        VLAN_PROFILE_TABm_t vp;
        VLAN_PROFILE_TABm_CLR(vp);
        VLAN_PROFILE_TABm_L2_PFMf_SET(vp, 0);
        VLAN_PROFILE_TABm_L3_IPV4_PFMf_SET(vp, 0);
        VLAN_PROFILE_TABm_L3_IPV6_PFMf_SET(vp, 0);
        VLAN_PROFILE_TABm_IPV4L3_ENABLEf_SET(vp, 1);
        VLAN_PROFILE_TABm_IPV6L3_ENABLEf_SET(vp, 1);
        /* IPMC routing DISABLED: we don't route IP multicast, and with it enabled
         * the chip diverts IPv4 multicast (incl. OSPF 224.0.0.5) to the IPMC path
         * where, with no group, it's consumed without a CPU copy. Disabling it
         * makes IP multicast pure-L2 -> L2_PFM=0 floods to VLAN members (incl. the
         * CPU) = the L2 behaviour behind Cumulus's MCAST_FLOOD_ALL. */
        VLAN_PROFILE_TABm_IPMCV4_ENABLEf_SET(vp, 0);
        VLAN_PROFILE_TABm_IPMCV6_ENABLEf_SET(vp, 0);
        VLAN_PROFILE_TABm_IPMCV4_L2_ENABLEf_SET(vp, 0);
        VLAN_PROFILE_TABm_IPMCV6_L2_ENABLEf_SET(vp, 0);
        /* Unknown (no IPMC group) IP multicast -> CPU. With IPMCV4_ENABLE=1 the
         * chip takes IPv4 multicast down the IPMC path; 224.0.0.5/6 (OSPF) have
         * no IPMC group, so without this they hit an IPMC miss and are silently
         * dropped (arrives at the port — RMCA increments — but no CPU copy, no
         * rdbgc). This is the per-profile equivalent of Cumulus's service VIDs
         * being MCAST_FLOOD_ALL: it delivers routing-protocol hellos to the CPU. */
        VLAN_PROFILE_TABm_UNKNOWN_IPV4_MC_TOCPUf_SET(vp, 1);
        VLAN_PROFILE_TABm_UNKNOWN_IPV6_MC_TOCPUf_SET(vp, 1);
        VLAN_PROFILE_TABm_LEARN_DISABLEf_SET(vp, 1);
        ioerr += WRITE_VLAN_PROFILE_TABm(unit, VLAN_PROFILE_TABm_MAX, vp);
        syslog(LOG_INFO,
               "MAC: VLAN_PROFILE[127] L2_PFM=0 + UNKNOWN_IPV4/6_MC_TOCPU=1 (OSPF hellos to CPU)");
    }

    /* ING_MISC_CONFIG2.IPMC_MISS_AS_L2MC — the actual lever that delivers OSPF.
     * With IPMCV4_ENABLE=1 the chip sends IPv4 multicast down the IPMC path; an
     * unknown group (224.0.0.5/6 — no IPMC entry) is otherwise dropped, NOT
     * L2-flooded, so VLAN membership (incl. CPU) never sees it. This bit makes an
     * IPMC miss fall back to L2MC logic = flood to the VLAN (members incl. CPU),
     * i.e. the L2 path behind Cumulus's MCAST_FLOOD_ALL. Cumulus captured
     * ING_MISC_CONFIG2 = 0x80 (this bit). Read-modify-write to keep other bits. */
    {
        ING_MISC_CONFIG2r_t mc2;
        READ_ING_MISC_CONFIG2r(unit, &mc2);
        ING_MISC_CONFIG2r_IPMC_MISS_AS_L2MCf_SET(mc2, 1);
        ioerr += WRITE_ING_MISC_CONFIG2r(unit, mc2);
        syslog(LOG_INFO,
               "MAC: ING_MISC_CONFIG2.IPMC_MISS_AS_L2MC=1 (unknown IPMC -> L2 flood -> CPU; OSPF)");
    }

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

    /* EGR_MTU per port (CPU + all XLPORTs).
     * Default has MTU_ENABLE=0 and MTU_SIZE=0; *some* chip stages
     * test this register before queueing → with default value every
     * egress frame fails MTU check and gets silently dropped.
     * Cumulus value 0x45f2 = MTU_ENABLE=1 + MTU_SIZE=1522 (standard
     * 802.1Q-tagged Ethernet). */
    {
        EGR_MTUr_t v;
        EGR_MTUr_CLR(v);
        EGR_MTUr_MTU_ENABLEf_SET(v, 1);
        EGR_MTUr_MTU_SIZEf_SET(v, 1622);
        ioerr += WRITE_EGR_MTUr(unit, 0, v);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_EGR_MTUr(unit, port, v);
        }
    }

    /* COMMAND_CONFIG per XLPORT — XGMAC MAC command register.
     * Cumulus writes 0x11800158 on every XLPORT.  The bits cover
     * pause/length-check/SFD-checks/etc.  We write the captured
     * value verbatim. */
    {
        COMMAND_CONFIGr_t v;
        COMMAND_CONFIGr_CLR(v);
        COMMAND_CONFIGr_SET(v, 0x11800158);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_COMMAND_CONFIGr(unit, port, v);
        }
    }

    /* EGR_VLAN_CONTROL_1 per port = 0x2001 (Cumulus).
     * bit 0 = enable egress VLAN-translation lookup
     * bit 13 = ?
     * We write the literal captured value. */
    {
        EGR_VLAN_CONTROL_1r_t v;
        EGR_VLAN_CONTROL_1r_CLR(v);
        EGR_VLAN_CONTROL_1r_SET(v, 0x2001);
        ioerr += WRITE_EGR_VLAN_CONTROL_1r(unit, 0, v);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_EGR_VLAN_CONTROL_1r(unit, port, v);
        }
    }

    /* AUX_ARB_CONTROL_2 = 0x0327f863 (Cumulus capture).
     * Sibling of AUX_ARB_CONTROL — controls internal arbitration
     * timing (clk_gran + sbus_spacing).  Critical for SBUS access
     * stability; we already write AUX_ARB_CONTROL but missed _2. */
    {
        AUX_ARB_CONTROL_2r_t v;
        AUX_ARB_CONTROL_2r_CLR(v);
        AUX_ARB_CONTROL_2r_SET(v, 0x0327f863);
        ioerr += WRITE_AUX_ARB_CONTROL_2r(unit, v);
    }

    /* OP_PORT_LIMIT_COLOR_CELL per port = 0x130b (RED=0x130b).
     * MMU color-aware egress port limit.  Default 0 = no color
     * tolerance → frames with non-zero color tag dropped. */
    {
        OP_PORT_LIMIT_COLOR_CELLr_t v;
        int color;
        OP_PORT_LIMIT_COLOR_CELLr_CLR(v);
        OP_PORT_LIMIT_COLOR_CELLr_REDf_SET(v, 0x130b);
        for (color = 0; color < 2; color++) {
            ioerr += WRITE_OP_PORT_LIMIT_COLOR_CELLr(unit, 0, color, v);
            CDK_PBMP_ITER(xlpbmp, port) {
                ioerr += WRITE_OP_PORT_LIMIT_COLOR_CELLr(unit, port,
                                                        color, v);
            }
        }
    }

    /* OP_BUFFER_LIMIT_RED_CELL + RESUME_RED_CELL +
     * YELLOW_CELL + RESUME_YELLOW_CELL — global color thresholds.
     * Cumulus all = 0x130b. */
    {
        OP_BUFFER_LIMIT_RED_CELLr_t r;
        OP_BUFFER_LIMIT_RESUME_RED_CELLr_t rr;
        OP_BUFFER_LIMIT_YELLOW_CELLr_t y;
        OP_BUFFER_LIMIT_RESUME_YELLOW_CELLr_t ry;
        OP_BUFFER_LIMIT_RED_CELLr_CLR(r);
        OP_BUFFER_LIMIT_RED_CELLr_SET(r, 0x130b);
        OP_BUFFER_LIMIT_RESUME_RED_CELLr_CLR(rr);
        OP_BUFFER_LIMIT_RESUME_RED_CELLr_SET(rr, 0x130b);
        OP_BUFFER_LIMIT_YELLOW_CELLr_CLR(y);
        OP_BUFFER_LIMIT_YELLOW_CELLr_SET(y, 0x130b);
        OP_BUFFER_LIMIT_RESUME_YELLOW_CELLr_CLR(ry);
        OP_BUFFER_LIMIT_RESUME_YELLOW_CELLr_SET(ry, 0x130b);
        ioerr += WRITE_OP_BUFFER_LIMIT_RED_CELLr(unit, 0, r);
        ioerr += WRITE_OP_BUFFER_LIMIT_RESUME_RED_CELLr(unit, 0, rr);
        ioerr += WRITE_OP_BUFFER_LIMIT_YELLOW_CELLr(unit, 0, y);
        ioerr += WRITE_OP_BUFFER_LIMIT_RESUME_YELLOW_CELLr(unit, 0, ry);
    }

    /* STORM_CONTROL_METER_CONFIG per port = 0x0fa0 (Cumulus).
     * Default 0 → storm control rate-limits broadcast/unknown-ucast
     * to 0 pps → every such frame dropped at meter stage with no
     * easy-to-find counter.  0xfa0 = 4000 (allow 4000 pps). */
    {
        STORM_CONTROL_METER_CONFIGr_t v;
        STORM_CONTROL_METER_CONFIGr_CLR(v);
        STORM_CONTROL_METER_CONFIGr_SET(v, 0x0fa0);
        ioerr += WRITE_STORM_CONTROL_METER_CONFIGr(unit, 0, v);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_STORM_CONTROL_METER_CONFIGr(unit, port, v);
        }
    }

    /* XMAC_RX_MAX_SIZE per XLPORT = 0x5f2 (1522 bytes).
     * Default may be 0 → MAC rejects everything as oversize. */
    {
        XMAC_RX_MAX_SIZEr_t v;
        XMAC_RX_MAX_SIZEr_CLR(v);
        XMAC_RX_MAX_SIZEr_SET(v, 0, 1622);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_XMAC_RX_MAX_SIZEr(unit, port, v);
        }
    }

    /* XMAC_CTRL per XLPORT = 0x3 (TX_EN + RX_EN).
     * BMD's port_enable_set should set RX_EN already, but
     * Cumulus's captured value also has TX_EN; force both. */
    {
        XMAC_CTRLr_t v;
        XMAC_CTRLr_CLR(v);
        XMAC_CTRLr_SET(v, 0, 0x3);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_XMAC_CTRLr(unit, port, v);
        }
    }

    /* Surfaced by chip-state vs Cumulus regdump diff
     * (commit ebf43a8) — these have THE biggest gaps and are very
     * likely to be the silent-drop point.
     *
     * OP_QUEUE_LIMIT_COLOR_CELL: per (port, queue-idx).  Cumulus
     * has 313 scopes set to 0x7; ours all default to 0.  Default 0
     * is interpreted as "drop every frame entering this queue at
     * color-check stage" → matches our chip-RX-OK / CPU-tx=0 symptom.
     * Write 0x7 to queue indices 0..7 per port. */
    {
        OP_QUEUE_LIMIT_COLOR_CELLr_t v;
        int q;
        OP_QUEUE_LIMIT_COLOR_CELLr_CLR(v);
        OP_QUEUE_LIMIT_COLOR_CELLr_SET(v, 0x7);
        for (q = 0; q < 8; q++) {
            ioerr += WRITE_OP_QUEUE_LIMIT_COLOR_CELLr(unit, 0, q, v);
            CDK_PBMP_ITER(xlpbmp, port) {
                ioerr += WRITE_OP_QUEUE_LIMIT_COLOR_CELLr(unit, port, q, v);
            }
        }
    }

    /* OP_QUEUE_RESET_OFFSET_CELL: per (port, queue-idx) = 0x3 in
     * Cumulus, default 0x1.  Reset offset for queue draining.  Apply
     * same as above. */
    {
        OP_QUEUE_RESET_OFFSET_CELLr_t v;
        int q;
        OP_QUEUE_RESET_OFFSET_CELLr_CLR(v);
        OP_QUEUE_RESET_OFFSET_CELLr_SET(v, 0x3);
        for (q = 0; q < 8; q++) {
            ioerr += WRITE_OP_QUEUE_RESET_OFFSET_CELLr(unit, 0, q, v);
            CDK_PBMP_ITER(xlpbmp, port) {
                ioerr += WRITE_OP_QUEUE_RESET_OFFSET_CELLr(unit, port, q, v);
            }
        }
    }

    /* XLPORT_CONFIG per XLPORT = 0x00010040.
     * bit 6  = xpause_rx_en (enable receive of pause frames)
     * bit 16 = ?? (some XLPORT-level enable Cumulus sets)
     * Cumulus has 0x10040 on every XLPORT; ours are all 0.
     * This is THE most-likely-critical per-XLPORT write we missed. */
    {
        XLPORT_CONFIGr_t v;
        XLPORT_CONFIGr_CLR(v);
        XLPORT_CONFIGr_SET(v, 0x00010040);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_XLPORT_CONFIGr(unit, port, v);
        }
    }

    /* OP_UC_PORT_LIMIT_COLOR_CELL per port = 0x0261730b.
     * Unicast egress port color limit.  All 52 ports have ours=0
     * vs Cumulus's 0x0261730b — could drop unicast traffic
     * destined to / from us. */
    {
        OP_UC_PORT_LIMIT_COLOR_CELLr_t v;
        OP_UC_PORT_LIMIT_COLOR_CELLr_CLR(v);
        OP_UC_PORT_LIMIT_COLOR_CELLr_SET(v, 0x0261730b);
        ioerr += WRITE_OP_UC_PORT_LIMIT_COLOR_CELLr(unit, 0, 0, v);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_OP_UC_PORT_LIMIT_COLOR_CELLr(unit, port, 0, v);
        }
    }

    /* XMODID_DUAL_EN per XLPORT = 0x1 (Cumulus).  Default 0. */
    {
        XMODID_DUAL_ENr_t v;
        XMODID_DUAL_ENr_CLR(v);
        XMODID_DUAL_ENr_SET(v, 0x1);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_XMODID_DUAL_ENr(unit, port, v);
        }
    }

    /* Round 3: more per-port gaps from regdump-diff.
     *
     * Per-port chip MAC addresses (MAC_0/MAC_1/XMAC_RX_MAC_SA/
     * XMAC_TX_MAC_SA).  We literally never wrote these, leaving the
     * chip's MAC unit with addr=0.  Compute per-port MAC = base + swpN
     * the same way packet_io.c does for TAP devices. */
    {
        uint8_t base[6];
        FILE *f = fopen("/sys/class/net/eth0/address", "r");
        int got = 0;
        if (f) {
            unsigned int m[6];
            if (fscanf(f, "%x:%x:%x:%x:%x:%x",
                       &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]) == 6) {
                int j;
                for (j = 0; j < 6; j++) base[j] = (uint8_t)m[j];
                got = 1;
            }
            fclose(f);
        }
        if (!got) {
            /* Cumulus captured default. */
            base[0]=0x80; base[1]=0xa2; base[2]=0x35;
            base[3]=0x81; base[4]=0xca; base[5]=0xae;
        }

        int swp;
        for (swp = 1; swp <= EDGED_MAX_PORTS; swp++) {
            if (!edged.ports[swp-1].valid) continue;
            int phys = edged.ports[swp-1].physical_lane;
            if (phys <= 0) continue;

            uint8_t mac[6];
            int j;
            for (j = 0; j < 6; j++) mac[j] = base[j];
            unsigned int low = (unsigned int)mac[5] + (unsigned int)swp;
            mac[5] = low & 0xff;
            if (low > 0xff) mac[4] = (mac[4] + 1) & 0xff;

            uint32_t mac_hi = ((uint32_t)mac[0] << 24)
                            | ((uint32_t)mac[1] << 16)
                            | ((uint32_t)mac[2] << 8)
                            |  (uint32_t)mac[3];
            uint32_t mac_lo16 = ((uint32_t)mac[4] << 8) | mac[5];
            uint32_t mac_low32 = ((uint32_t)mac[2] << 24)
                               | ((uint32_t)mac[3] << 16)
                               | ((uint32_t)mac[4] << 8)
                               |  (uint32_t)mac[5];
            uint32_t mac_high16 = ((uint32_t)mac[0] << 8)
                                |  (uint32_t)mac[1];

            MAC_0r_t v0; MAC_0r_CLR(v0); MAC_0r_SET(v0, mac_hi);
            ioerr += WRITE_MAC_0r(unit, phys, v0);
            MAC_1r_t v1; MAC_1r_CLR(v1); MAC_1r_SET(v1, mac_lo16);
            ioerr += WRITE_MAC_1r(unit, phys, v1);

            XMAC_RX_MAC_SAr_t rx; XMAC_RX_MAC_SAr_CLR(rx);
            XMAC_RX_MAC_SAr_SET(rx, 0, mac_low32);
            XMAC_RX_MAC_SAr_SET(rx, 1, mac_high16);
            ioerr += WRITE_XMAC_RX_MAC_SAr(unit, phys, rx);

            XMAC_TX_MAC_SAr_t tx; XMAC_TX_MAC_SAr_CLR(tx);
            XMAC_TX_MAC_SAr_SET(tx, 0, mac_low32);
            XMAC_TX_MAC_SAr_SET(tx, 1, mac_high16);
            ioerr += WRITE_XMAC_TX_MAC_SAr(unit, phys, tx);
        }
    }

    /* XMAC_RX_CTRL per XLPORT = 0x408 (Cumulus); ours is 0x8.
     * Bit 10 controls a key RX feature (likely STRIP_CRC or
     * RUNT_FILTER); flipping it to match Cumulus exactly. */
    {
        XMAC_RX_CTRLr_t v;
        XMAC_RX_CTRLr_CLR(v);
        XMAC_RX_CTRLr_SET(v, 0, 0x408);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_XMAC_RX_CTRLr(unit, port, v);
        }
    }

    /* OP_UC_PORT_LIMIT_RESUME_COLOR_CELL per port = 0x0261530a.
     * Sibling of OP_UC_PORT_LIMIT_COLOR_CELL we already write. */
    {
        OP_UC_PORT_LIMIT_RESUME_COLOR_CELLr_t v;
        OP_UC_PORT_LIMIT_RESUME_COLOR_CELLr_CLR(v);
        OP_UC_PORT_LIMIT_RESUME_COLOR_CELLr_SET(v, 0x0261530a);
        ioerr += WRITE_OP_UC_PORT_LIMIT_RESUME_COLOR_CELLr(unit, 0, 0, v);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_OP_UC_PORT_LIMIT_RESUME_COLOR_CELLr(unit,
                                                              port, 0, v);
        }
    }

    /* OP_UC_PORT_CONFIG1_CELL per port = 0x8040.  Unicast egress
     * port config — sibling of the OP_UC_PORT_CONFIG_CELL that
     * bmd_init already programs.  We never touched the _1 variant. */
    {
        OP_UC_PORT_CONFIG1_CELLr_t v;
        OP_UC_PORT_CONFIG1_CELLr_CLR(v);
        OP_UC_PORT_CONFIG1_CELLr_SET(v, 0x8040);
        ioerr += WRITE_OP_UC_PORT_CONFIG1_CELLr(unit, 0, v);
        CDK_PBMP_ITER(xlpbmp, port) {
            ioerr += WRITE_OP_UC_PORT_CONFIG1_CELLr(unit, port, v);
        }
    }

    /* OP_PORT_LIMIT_RESUME_COLOR_CELL per (port, color) = 0x130a. */
    {
        OP_PORT_LIMIT_RESUME_COLOR_CELLr_t v;
        int color;
        OP_PORT_LIMIT_RESUME_COLOR_CELLr_CLR(v);
        OP_PORT_LIMIT_RESUME_COLOR_CELLr_SET(v, 0x130a);
        for (color = 0; color < 2; color++) {
            ioerr += WRITE_OP_PORT_LIMIT_RESUME_COLOR_CELLr(unit, 0,
                                                            color, v);
            CDK_PBMP_ITER(xlpbmp, port) {
                ioerr += WRITE_OP_PORT_LIMIT_RESUME_COLOR_CELLr(unit,
                                                        port, color, v);
            }
        }
    }

    /* Suppress unused-variable warning */
    (void)p;

    syslog(LOG_INFO,
           "rc_full: ... + per-port chip MAC (MAC_0/1, XMAC_RX/TX_MAC_SA), "
           "XMAC_RX_CTRL=0x408, OP_UC_PORT_LIMIT_RESUME_COLOR_CELL=0x261530a, "
           "OP_UC_PORT_CONFIG1_CELL=0x8040, OP_PORT_LIMIT_RESUME_COLOR_CELL=0x130a "
           "(round 3 of regdump-diff fixes)");
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

    /* Replicate Cumulus chip-memory state: EPC_LINK_BMAP, L2_USER_ENTRY,
     * EGR_VLAN/STG, FP_TCAM/POLICY.  See cumulus_replicate.c. */
    if (cumulus_replicate_init() < 0) {
        ioerr++;
    }

    if (ioerr) {
        syslog(LOG_ERR, "Datapath init had %d I/O errors", ioerr);
        return -1;
    }

    syslog(LOG_INFO, "Datapath configuration complete");
    return 0;
}

/*
 * datapath_rx_diag() - on-demand chip RX-path dump for a physical port.
 *
 * Wired to SIGUSR1 (see edged.c).  Localises where ingress frames are
 * dropped on the RX→CPU path by reading the per-stage RX debug drop
 * counters (RDBGCn, selects programmed in datapath_mac_init) plus the
 * port's classification config (PVID) and the service VLAN membership.
 *
 * Drop-counter SELECTs (from datapath_mac_init):
 *   rdbgc0 = aggregate (RIPD4+RIPD6+RDISC+RPORTD+PDISC+VLANDR)
 *   rdbgc3 = RIPD4+RIPD6  (IPv4/IPv6 header / L3 lookup drops)
 *   rdbgc4 = RDISC        (general discard)
 *   rdbgc5 = RFILDR       (ingress filter / VLAN-membership drop)
 *   rdbgc6 = RDROP        (generic RX drop)
 */
void datapath_rx_diag(void)
{
    int unit = edged.unit;
    int i;

    extern unsigned g_tx_calls, g_tx_ok, g_tx_fail, g_tx_lastrv;

    syslog(LOG_INFO, "=== RX-DIAG (SIGUSR1) ===");

    /* L3_DEFIP[2560/2561] readback: confirm our local-host /32 -> CPU TCAM
     * entries actually landed (raw mem_write may only write the RAM half of a
     * TCAM and not load the key/mask CAM). */
    {
        int slots[] = {2560, 2561, 8000};
        unsigned k;
        for (k = 0; k < sizeof(slots)/sizeof(slots[0]); k++) {
            L3_DEFIPm_t d;
            if (READ_L3_DEFIPm(unit, slots[k], &d) == 0) {
                syslog(LOG_INFO,
                       "RX-DIAG L3_DEFIP[%d]: VALID0=%d IP_ADDR0=0x%x "
                       "IP_ADDR_MASK0=0x%x VRF_MASK0=0x%x NHI0=%d MODE_MASK0=%d "
                       "HIT0=%d  <-- HIT=1 means the route lookup matched",
                       slots[k],
                       L3_DEFIPm_VALID0f_GET(d),
                       L3_DEFIPm_IP_ADDR0f_GET(d),
                       L3_DEFIPm_IP_ADDR_MASK0f_GET(d),
                       L3_DEFIPm_VRF_ID_MASK0f_GET(d),
                       L3_DEFIPm_NEXT_HOP_INDEX0f_GET(d),
                       L3_DEFIPm_MODE_MASK0f_GET(d),
                       L3_DEFIPm_HIT0f_GET(d));
            }
        }
    }

    /* L3_IIF / ingress-VRF readout: the datapath derives the ingress VRF for
     * an L3-terminated frame from L3_IIFm[iif].VRF.  Our L3_ENTRY host is
     * written with VRF=0; if the ingress VRF != 0 the HW lookup misses (RIPD4)
     * even though the SW schan lookup (which we force to VRF=0) finds it.
     * Dump a few L3_IIF indices to see what VRF / ALLOW_GLOBAL_ROUTE they carry. */
    {
        int iifs[] = {0, 1, 2, 3, 100, 101};
        unsigned k;
        for (k = 0; k < sizeof(iifs)/sizeof(iifs[0]); k++) {
            L3_IIFm_t iif;
            if (READ_L3_IIFm(unit, iifs[k], &iif) == 0) {
                syslog(LOG_INFO,
                       "RX-DIAG L3_IIF[%d]: VRF=%d ALLOW_GLOBAL_ROUTE=%d "
                       "URPF_MODE=%d",
                       iifs[k],
                       L3_IIFm_VRFf_GET(iif),
                       L3_IIFm_ALLOW_GLOBAL_ROUTEf_GET(iif),
                       L3_IIFm_URPF_MODEf_GET(iif));
            }
        }
    }
    syslog(LOG_INFO, "RX-DIAG TX: calls=%u ok=%u fail=%u lastrv=%u",
           g_tx_calls, g_tx_ok, g_tx_fail, g_tx_lastrv);

    /* Port-index sweep: RUC (RX unicast) + rdbgc0 (aggregate drop) across
     * every possible device port.  phys/SerDes-lane numbering != the MMU
     * port index these counters use, so print whichever indices are
     * non-zero — a before/after-ping diff shows exactly where the reply
     * lands (or proves it never arrives at any MAC). */
    {
        int pidx;
        for (pidx = 0; pidx <= 72; pidx++) {
            RUCr_t ru; RDBGC0r_t d0; RDBGC3r_t d3; RDBGC4r_t d4;
            RDBGC5r_t d5; RDBGC6r_t d6;
            uint32_t ruv, d0v;
            READ_RUCr(unit, pidx, &ru);
            READ_RDBGC0r(unit, pidx, &d0);
            ruv = RUCr_GET(ru); d0v = RDBGC0r_GET(d0);
            if (ruv || d0v) {
                READ_RDBGC3r(unit, pidx, &d3);
                READ_RDBGC4r(unit, pidx, &d4);
                READ_RDBGC5r(unit, pidx, &d5);
                READ_RDBGC6r(unit, pidx, &d6);
                syslog(LOG_INFO,
                       "RX-DIAG sweep port[%d]: RUC=%u rdbgc0(agg)=%u "
                       "rdbgc3(L3hdr)=%u rdbgc4(disc)=%u rdbgc5(filt)=%u "
                       "rdbgc6(drop)=%u",
                       pidx, ruv, d0v,
                       RDBGC3r_GET(d3), RDBGC4r_GET(d4),
                       RDBGC5r_GET(d5), RDBGC6r_GET(d6));
            }
        }
    }

    for (i = 0; i < EDGED_MAX_PORTS; i++) {
        struct port_state *p = &edged.ports[i];
        if (!p->valid)
            continue;
        /* Only the two Nexus uplinks to keep the dump readable. */
        if (p->logical_port != 1 && p->logical_port != 2)
            continue;
        int phys = p->physical_lane;
        int vid  = edged_resv_vid_for_port(p->logical_port);

        RUCr_t ruc; RMCAr_t rmc;
        RDBGC0r_t d0; RDBGC3r_t d3; RDBGC4r_t d4; RDBGC5r_t d5; RDBGC6r_t d6;
        PORT_TABm_t pt; VLAN_TABm_t vt;

        READ_RUCr(unit, phys, &ruc);
        READ_RMCAr(unit, phys, &rmc);
        READ_RDBGC0r(unit, phys, &d0);
        READ_RDBGC3r(unit, phys, &d3);
        READ_RDBGC4r(unit, phys, &d4);
        READ_RDBGC5r(unit, phys, &d5);
        READ_RDBGC6r(unit, phys, &d6);

        syslog(LOG_INFO,
               "RX-DIAG %s phys=%d: RUC(ucast)=%u RMCA(mcast)=%u | rdbgc0(agg)=%u "
               "rdbgc3(L3hdr)=%u rdbgc4(disc)=%u rdbgc5(filt)=%u rdbgc6(drop)=%u",
               p->ifname, phys,
               RUCr_GET(ruc), RMCAr_GET(rmc, 0),
               RDBGC0r_GET(d0), RDBGC3r_GET(d3), RDBGC4r_GET(d4),
               RDBGC5r_GET(d5), RDBGC6r_GET(d6));

        /* PORT_TABm is LOGICAL-port indexed (bmd_port_vlan_set writes
         * PORT_TABm[P2L(port)]).  Read both logical and physical index
         * so a port-map mismatch is visible. */
        if (READ_PORT_TABm(unit, p->logical_port, &pt) == 0) {
            syslog(LOG_INFO,
                   "RX-DIAG %s PORT_TAB[lport=%d]: PORT_VID=%d (expect %d) "
                   "TRUST_INCOMING_VID=%d FILTER_ENABLE=%d",
                   p->ifname, p->logical_port,
                   PORT_TABm_PORT_VIDf_GET(pt), vid,
                   PORT_TABm_TRUST_INCOMING_VIDf_GET(pt),
                   PORT_TABm_FILTER_ENABLEf_GET(pt));
        }
        if (READ_PORT_TABm(unit, phys, &pt) == 0) {
            syslog(LOG_INFO,
                   "RX-DIAG %s PORT_TAB[phys=%d]: PORT_VID=%d",
                   p->ifname, phys, PORT_TABm_PORT_VIDf_GET(pt));
        }
        if (READ_VLAN_TABm(unit, vid, &vt) == 0) {
            int pp = VLAN_TABm_VLAN_PROFILE_PTRf_GET(vt);
            VLAN_PROFILE_TABm_t vpd;
            int l2pfm = -1, unkmc = -1, ipmc4 = -1;
            if (READ_VLAN_PROFILE_TABm(unit, pp, &vpd) == 0) {
                l2pfm = VLAN_PROFILE_TABm_L2_PFMf_GET(vpd);
                unkmc = VLAN_PROFILE_TABm_UNKNOWN_IPV4_MC_TOCPUf_GET(vpd);
                ipmc4 = VLAN_PROFILE_TABm_IPMCV4_ENABLEf_GET(vpd);
            }
            syslog(LOG_INFO,
                   "RX-DIAG %s VLAN_TAB[%d]: VALID=%d STG=%d PROFILE_PTR=%d "
                   "-> [L2_PFM=%d IPMCV4_EN=%d UNKNOWN_IPV4_MC_TOCPU=%d]",
                   p->ifname, vid,
                   VLAN_TABm_VALIDf_GET(vt), VLAN_TABm_STGf_GET(vt),
                   pp, l2pfm, ipmc4, unkmc);
        }
        syslog(LOG_INFO, "RX-DIAG %s sw-counters: tx_pkts=%llu rx_pkts=%llu",
               p->ifname,
               (unsigned long long)p->tx_packets,
               (unsigned long long)p->rx_packets);
    }
    syslog(LOG_INFO, "=== RX-DIAG end ===");
}
