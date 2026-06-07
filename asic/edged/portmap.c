/*
 * portmap.c - Port mapping for AS5610-52X
 *
 * Maps between:
 *   - Front-panel port number (swp1-52)
 *   - BCM logical port (1-52)
 *   - SerDes physical lane
 *   - I2C bus number (port N = bus 21+N for SFP, 18-21 for QSFP)
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <time.h>

#include "edged.h"
#include "portmap.h"

/* BMD headers */
#include <bmd/bmd.h>
#include <bmd/bmd_phy.h>
#include <bmd/bmd_phy_ctrl.h>

/* PHY headers - for direct PHY_CONFIG_SET access */
#include <phy/phy.h>

/* Warpcore register defs - for CL82 (40G MLD) RX status registers used by
 * the QSFP 4-lane link detection (cl82_link_get). */
#include <phy/chip/bcmi_warpcore_xgxs_defs.h>

/* Port-to-SerDes lane mapping for AS5610-52X
 * Index = front-panel port (0-based), value = SerDes lane
 */
static const int port_to_lane[EDGED_MAX_PORTS] = {
    /* SFP+ ports 1-8 */   65, 66, 67, 68, 69, 70, 71, 72,
    /* SFP+ ports 9-16 */   5,  6,  7,  8,  9, 10, 11, 12,
    /* SFP+ ports 17-24 */ 13, 14, 15, 16, 18, 17, 20, 19,
    /* SFP+ ports 25-32 */ 22, 21, 24, 23, 25, 26, 27, 28,
    /* SFP+ ports 33-40 */ 29, 30, 31, 32, 33, 34, 35, 36,
    /* SFP+ ports 41-48 */ 37, 38, 39, 40, 41, 42, 43, 44,
    /* QSFP+ ports 49-52 */ 49, 45, 61, 57,
};

/* Port-to-I2C bus mapping (EdgeNOS kernel 5.10)
 *
 * Kernel 5.10 enumerates I2C mux children depth-first (sub-mux children
 * get consecutive bus numbers immediately after parent), unlike Cumulus
 * kernel 3.2 which was breadth-first. See I2C_BUS_NUMBER_MAPPING.md.
 *
 * Bus 1 → mux 0x75 ch0 → bus 10 → sub-mux 0x74 → buses 11-18 (swp1-8)
 * Bus 1 → mux 0x75 ch1 → bus 19 → sub-mux 0x74 → buses 20-27 (swp9-16)
 * Bus 1 → mux 0x75 ch2 → bus 28 → sub-mux 0x74 → buses 29-36 (swp17-24)
 * Bus 1 → mux 0x75 ch3 → bus 37 → sub-mux 0x74 → buses 38-45 (swp25-32)
 * Bus 1 → mux 0x76 ch0 → bus 46 → sub-mux 0x74 → buses 47-54 (swp33-40)
 * Bus 1 → mux 0x76 ch1 → bus 55 → sub-mux 0x74 → buses 56-63 (swp41-48)
 * Bus 1 → mux 0x77 ch0-3 → buses 66-69 (QSFP swp49-52)
 */
static const int port_to_i2c_bus[EDGED_MAX_PORTS] = {
    11, 12, 13, 14, 15, 16, 17, 18,   /* ports 1-8   (group 1, sub-mux on bus 10) */
    20, 21, 22, 23, 24, 25, 26, 27,   /* ports 9-16  (group 2, sub-mux on bus 19) */
    29, 30, 31, 32, 33, 34, 35, 36,   /* ports 17-24 (group 3, sub-mux on bus 28) */
    38, 39, 40, 41, 42, 43, 44, 45,   /* ports 25-32 (group 4, sub-mux on bus 37) */
    47, 48, 49, 50, 51, 52, 53, 54,   /* ports 33-40 (group 5, sub-mux on bus 46) */
    56, 57, 58, 59, 60, 61, 62, 63,   /* ports 41-48 (group 6, sub-mux on bus 55) */
    66, 67, 68, 69,                    /* ports 49-52 (QSFP, mux 0x77) */
};

void portmap_parse_config(const char *key, const char *val)
{
    int port;
    int lane, speed;

    /* Parse portmap_N.0=lane:speed */
    if (sscanf(key, "portmap_%d.0", &port) != 1)
        return;
    if (sscanf(val, "%d:%d", &lane, &speed) != 2)
        return;

    if (port < 1 || port > EDGED_MAX_PORTS)
        return;

    int idx = port - 1;
    edged.ports[idx].valid = 1;
    edged.ports[idx].logical_port = port;
    edged.ports[idx].physical_lane = lane;
    edged.ports[idx].speed = speed * 1000;  /* Convert to Mbps */
    edged.ports[idx].port_type = (speed == 40) ? PORT_TYPE_QSFP : PORT_TYPE_SFP;
    snprintf(edged.ports[idx].ifname, sizeof(edged.ports[idx].ifname),
             "swp%d", port);
}

int portmap_configure_ports(void)
{
    int i;
    int configured = 0;

    /* If no ports were parsed from config, use hardcoded defaults */
    if (edged.ports[0].valid == 0) {
        syslog(LOG_INFO, "Using default port map");
        for (i = 0; i < EDGED_MAX_PORTS; i++) {
            edged.ports[i].valid = 1;
            edged.ports[i].logical_port = i + 1;
            edged.ports[i].physical_lane = port_to_lane[i];
            edged.ports[i].speed = (i >= EDGED_SFP_PORTS) ? 40000 : 10000;
            edged.ports[i].port_type = (i >= EDGED_SFP_PORTS) ?
                                         PORT_TYPE_QSFP : PORT_TYPE_SFP;
            snprintf(edged.ports[i].ifname,
                     sizeof(edged.ports[i].ifname), "swp%d", i + 1);
        }
    }

    /* Configure each port on the ASIC via BMD */
    for (i = 0; i < EDGED_MAX_PORTS; i++) {
        if (!edged.ports[i].valid)
            continue;

        /*
         * BMD functions expect CDK physical port number,
         * which is physical_lane on BCM56840.
         */
        int port = edged.ports[i].physical_lane;
        int speed = edged.ports[i].speed;
        bmd_port_mode_t mode;
        uint32_t flags = 0;
        int rv;

        /*
         * Select port mode based on speed.
         * From OpenMDK bcm56840_a0_bmd_port_mode_set.c:
         *   10G SFP+: bmdPortMode10000fd -> FV_fdr_10G_XFI (speed_val=0x25)
         *   40G QSFP: bmdPortMode40000fd -> FV_fdr_40G_KR4 (speed_val=0x27)
         *
         * This internally calls bcmi_warpcore_xgxs_speed_set() which:
         *   - Sets forced speed in MISC1r/MISC3r
         *   - Configures PLL mode (div66 for 10G, default for 40G)
         *   - Sets FIRMWARE_MODEr for SFP_DAC/XFI/KR
         *   - Restarts PLL sequencer
         *   - Configures MAC speed/duplex in XMAC_MODEr
         *   - Enables XMAC RX/TX
         *   - Updates EPC_LINK_BMAP
         */
        if (edged.ports[i].port_type == PORT_TYPE_QSFP)
            mode = bmdPortMode40000fd;
        else
            mode = bmdPortMode10000fd;

        /*
         * Set SERDES_MODE flag for single-lane SFP+ ports ONLY.
         * Without this, the Warpcore PHY driver classifies ports as
         * 4-lane (IS_4LANE_PORT) and configures 10GBASE-CX4 encoding
         * instead of 10G XFI (single-lane 64b/66b for SFP+).
         * CX4 encoding is incompatible with 10GBASE-LR/SR optics.
         *
         * QSFP ports must NOT have SERDES_MODE set — they need 4-lane
         * mode for 40G. Setting SERDES_MODE on QSFP causes
         * bmd_port_mode_set(40G) to fail with CDK_E_PARAM (-4).
         */
        if (edged.ports[i].port_type != PORT_TYPE_QSFP) {
            rv = bmd_phy_mode_set(edged.unit, port, "warpcore",
                             BMD_PHY_MODE_SERDES, 1);
        } else {
            rv = 0; /* QSFP: keep 4-lane mode */
        }
        {
            phy_ctrl_t *pc_chk = BMD_PORT_PHY_CTRL(edged.unit, port);
            if (pc_chk) {
                /*
                 * CRITICAL FIX: Clear PHY_F_CLAUSE45 flag.
                 *
                 * The Warpcore PHY init sets CLAUSE45 based on the
                 * MULTIMMDS_EN register. On BCM56846, this is enabled
                 * but the CMIC MIIM doesn't properly handle CL45 for
                 * internal SerDes. With CLAUSE45 set, phy_aer_iblk_write
                 * takes the early CL45 path (DEVAD+reg) instead of the
                 * CL22 block-select path (reg 0x1F + reg offset).
                 *
                 * Result: MISC1r, MISC3r, FIRMWARE_MODEr writes all
                 * silently fail → speed encoding never changes from
                 * default CX4 → PCS can't block-lock on XFI signal
                 * → link stays down despite TX/RX optical power.
                 *
                 * Cumulus SDK 6.3.8 likely has a different MIIM
                 * implementation that handles CL45 correctly for
                 * iProc-based switches. OpenMDK needs CL22 path.
                 */
                PHY_CTRL_FLAGS(pc_chk) &= ~PHY_F_CLAUSE45;

                syslog(LOG_INFO, "Port %s: flags=0x%x (CLAUSE45 cleared) bus=%s phy_addr=0x%03x inst=%d",
                       edged.ports[i].ifname,
                       PHY_CTRL_FLAGS(pc_chk),
                       pc_chk->bus ? pc_chk->bus->drv_name : "NULL",
                       pc_chk->bus ? pc_chk->bus->phy_addr(port) : 0,
                       PHY_CTRL_INST(pc_chk));
            } else {
                syslog(LOG_WARNING, "Port %s: no PHY ctrl for port %d",
                       edged.ports[i].ifname, port);
            }
        }

        /*
         * Force port mode re-set by first disabling, then enabling.
         * bmd_switching_init already called bmd_port_mode_set(Auto) which
         * set CX4 mode (speed_val=0x05) because CLAUSE45 was set during
         * that call. Now with CLAUSE45 cleared, a fresh port_mode_set
         * will correctly write MISC1r + MISC3r for XFI (speed_val=0x25).
         *
         * Without the Disable→Enable cycle, bmd_port_mode_set sees
         * "already at 10G" and skips the PHY speed_set entirely.
         */
        bmd_port_mode_set(edged.unit, port, bmdPortModeDisabled, 0);

        /*
         * Apply the Warpcore RX lane remap BEFORE the enabling mode_set, so the
         * subsequent reset + RX adaptation runs WITH the swap in place. This
         * mirrors board_bcm56846_svk.c's _phy_reset_cb (which applies it during
         * phy reset). Applying it AFTER adaptation (tried earlier) just disturbs
         * the RX (the handler re-selects the div/16 clock) and leaves it
         * non-adapting. File-driven via /tmp/qsfp_rxremap:
         *   "ref"  -> reference per-port (port 45 = 0x3210, others = 0x1032)
         *   <hex>  -> that value for all QSFP ports
         *   absent -> no remap (baseline)
         */
        if (edged.ports[i].port_type == PORT_TYPE_QSFP) {
            FILE *rf = fopen("/tmp/qsfp_rxremap", "r");
            if (rf) {
                char buf[32] = {0};
                if (fgets(buf, sizeof(buf), rf)) {
                    phy_ctrl_t *pcr = BMD_PORT_PHY_CTRL(edged.unit, port);
                    unsigned int rmap = 0;
                    int apply = 1;
                    if (buf[0] == 'r')                 /* "ref" */
                        rmap = (port == 45) ? 0x3210 : 0x1032;
                    else if (sscanf(buf, "%x", &rmap) != 1)
                        apply = 0;
                    if (apply && pcr) {
                        int rrv = PHY_CONFIG_SET(pcr, PhyConfig_XauiRxLaneRemap,
                                                 rmap, NULL);
                        syslog(LOG_INFO,
                               "Port %s: XauiRxLaneRemap=0x%04x rv=%d (pre-enable)",
                               edged.ports[i].ifname, rmap, rrv);
                    }
                }
                fclose(rf);
            }
        }

        rv = bmd_port_mode_set(edged.unit, port, mode, flags);
        if (rv < 0) {
            syslog(LOG_ERR, "Port %s: bmd_port_mode_set(%d, %dG) failed: %d",
                   edged.ports[i].ifname, port, speed / 1000, rv);
            continue;
        }

        /*
         * Fix: Explicitly release TX/RX ASIC reset and set IND_40BITIF.
         *
         * The Disable→Enable cycle leaves MISC6r reset_rx=1 (RX stuck
         * in reset) and MISC3r IND_40BITIF=0 (40-bit interface not
         * enabled). Without these, PCS can never lock.
         *
         * speed_set (line 1589-1594 of xgxs_drv.c) should release
         * the reset, but the write doesn't persist — possibly because
         * the Disable call's MISC6r state overrides the Enable's write.
         *
         * QSFP/40G NOTE: this fixup is 10G-XFI-specific -- it forces
         * IND_40BITIF (40-bit single-lane interface), releases TX/RX reset on
         * lane 0 ONLY, and restarts the PLL with the 10G value 0x242f.  For a
         * 40G CL82 4-lane QSFP port that is WRONG: it clobbers the 4-lane SR4
         * config that bcmi_warpcore_xgxs_speed_set() just applied and leaves
         * the SerDes TX invalid (the optic then squelches its laser -> RX_LOS,
         * no link).  So apply it only to single-lane SFP+ ports; for QSFP let
         * the speed_set 40G/SR4 configuration stand.
         */
        if (edged.ports[i].port_type != PORT_TYPE_QSFP)
        {
            phy_ctrl_t *pc_fix = BMD_PORT_PHY_CTRL(edged.unit, port);
            if (pc_fix) {
                /* Set AER lane for per-lane register access.
                 * Without this, writes go to lane 0 (swp1) instead of
                 * the correct lane for this port (e.g., lane 1 for swp2). */
                int fix_lane = PHY_CTRL_INST(pc_fix) & 0x3;
                PHY_BUS_WRITE(pc_fix, 0x1f, 0xffd0);
                PHY_BUS_WRITE(pc_fix, 0x1e, fix_lane);

                /*
                 * Release TX/RX ASIC reset via MISC6r.
                 * MISC6r = CDK 0x8345 = block 0x8340, offset 0x5, CL22 reg 0x15
                 * (NOT reg 0x19 — that's MISC7r at CDK 0x8349!)
                 * RESET_RX_ASIC = bit 15, RESET_TX_ASIC = bit 14
                 */
                PHY_BUS_WRITE(pc_fix, 0x1f, 0x8340);
                uint32_t misc6_val = 0;
                PHY_BUS_READ(pc_fix, 0x15, &misc6_val);
                misc6_val &= ~((1 << 15) | (1 << 14));  /* Clear RESET_RX_ASIC and RESET_TX_ASIC */
                PHY_BUS_WRITE(pc_fix, 0x15, misc6_val);

                /* Set IND_40BITIF=1 in MISC3r for XFI */
                PHY_BUS_WRITE(pc_fix, 0x1f, 0x8330);
                uint32_t misc3_val = 0;
                PHY_BUS_READ(pc_fix, 0x1c, &misc3_val);
                /*
                 * IND_40BITIF is bit 15 (NOT bit 6).
                 * Cross-checked against open-nos-ref + OpenMDK symbol table.
                 * Documented in memory/project_session_20260509.md.
                 */
                misc3_val |= (1 << 15);  /* IND_40BITIF = bit 15 */
                PHY_BUS_WRITE(pc_fix, 0x1c, misc3_val);

                /*
                 * Restart PLL sequencer after MISC3r/MISC6r changes.
                 *
                 * The speed_set code restarts the sequencer during port
                 * enable, but our IND_40BITIF fix above happens AFTER
                 * that. Without a sequencer restart, the PLL is running
                 * with the old (wrong) interface mode → PCS never locks.
                 *
                 * XGXSCONTROLr = block 0x8000, offset 0x00, CL22 reg 0x10
                 * START_SEQUENCER = bit 13
                 */
                /* Restart PLL sequencer — write the full known-good value.
                 * Use AER=0 (broadcast) for XGXSCONTROLr. */
                PHY_BUS_WRITE(pc_fix, 0x1f, 0xffd0);
                PHY_BUS_WRITE(pc_fix, 0x1e, 0);

                PHY_BUS_WRITE(pc_fix, 0x1f, 0x8000);
                /* Write full known-good value: 0x242f with seq=1 */
                PHY_BUS_WRITE(pc_fix, 0x10, 0x202f); /* seq=0 first */
                usleep(1000);
                PHY_BUS_WRITE(pc_fix, 0x10, 0x242f); /* seq=1 */
                usleep(10000); /* Wait for PLL lock */

                /* Read back */
                uint32_t xgxs_ctrl = 0;
                PHY_BUS_READ(pc_fix, 0x10, &xgxs_ctrl);
                syslog(LOG_INFO, "Port %s: XGXSCONTROLr after seq_restart=0x%04x",
                       edged.ports[i].ifname, xgxs_ctrl & 0xffff);

                /* Restore AER */
                PHY_BUS_WRITE(pc_fix, 0x1f, 0xffd0);
                PHY_BUS_WRITE(pc_fix, 0x1e, fix_lane);

                PHY_BUS_WRITE(pc_fix, 0x1f, 0x0000);

                syslog(LOG_INFO, "Port %s: forced reset_rx=0 IND_40BITIF=1 seq_restart",
                       edged.ports[i].ifname);
            }
        }
        else
        {
            /*
             * 40G QSFP: the SR4 speed_set already configured the 4-lane port,
             * but (like the SFP case) the per-lane TX/RX ASIC reset release in
             * MISC6r doesn't reliably persist -- observed as only 2 of 4 lanes
             * reaching CL82 AM-lock.  Release RESET_RX_ASIC/RESET_TX_ASIC on
             * ALL FOUR lanes (AER 0-3) so every lane's RX can lock.  Do NOT set
             * IND_40BITIF or rewrite the PLL value here -- those are 10G-XFI
             * settings and would break the 4-lane SR4 config.
             */
            phy_ctrl_t *pc_q = BMD_PORT_PHY_CTRL(edged.unit, port);
            if (pc_q) {
                int qlane;
                /* (RX lane remap now applied pre-enable, above — see the
                 * /tmp/qsfp_rxremap block before bmd_port_mode_set.) */
                /*
                 * EXPERIMENT (file-driven): force per-lane RX/TX polarity flip
                 * on QSFP lanes from /tmp/qsfp_polflip (hex mask: bits 0-3 =
                 * RX polarity flip per lane, bits 4-7 = TX polarity flip per
                 * lane).  Applied BEFORE the reset release below so RX comes
                 * out of reset with the forced polarity.  Diagnoses the
                 * deterministic lanes-2,3 AM-lock failure on the 40G loopback.
                 * Sweep combos by editing the file + restarting edged.
                 */
                {
                    FILE *pf = fopen("/tmp/qsfp_polflip", "r");
                    unsigned int mask = 0;
                    if (pf) { if (fscanf(pf, "%x", &mask) != 1) mask = 0; fclose(pf); }
                    if (mask) {
                        int idx;
                        for (idx = 0; idx < 4; idx++) {
                            ANARXCONTROLPCIr_t rxc;
                            ANATXACONTROL0r_t txc;
                            if ((mask & (1u << idx)) &&
                                READ_ANARXCONTROLPCIr(pc_q, idx, &rxc) >= 0) {
                                ANARXCONTROLPCIr_RX_POLARITY_Rf_SET(rxc, 1);
                                ANARXCONTROLPCIr_RX_POLARITY_FORCE_SMf_SET(rxc, 1);
                                WRITE_ANARXCONTROLPCIr(pc_q, idx, rxc);
                            }
                            if ((mask & (1u << (idx + 4))) &&
                                READ_ANATXACONTROL0r(pc_q, idx, &txc) >= 0) {
                                ANATXACONTROL0r_TXPOL_FLIPf_SET(txc, 1);
                                WRITE_ANATXACONTROL0r(pc_q, idx, txc);
                            }
                        }
                        syslog(LOG_INFO, "Port %s: QSFP polflip mask=0x%x applied",
                               edged.ports[i].ifname, mask);
                    }
                }
                for (qlane = 0; qlane < 4; qlane++) {
                    uint32_t m6 = 0;
                    PHY_BUS_WRITE(pc_q, 0x1f, 0xffd0);   /* AER */
                    PHY_BUS_WRITE(pc_q, 0x1e, qlane);
                    PHY_BUS_WRITE(pc_q, 0x1f, 0x8340);   /* MISC6r block */
                    PHY_BUS_READ(pc_q, 0x15, &m6);
                    m6 &= ~((1 << 15) | (1 << 14));      /* clear RESET_RX/TX_ASIC */
                    PHY_BUS_WRITE(pc_q, 0x15, m6);
                }
                PHY_BUS_WRITE(pc_q, 0x1f, 0x0000);
                syslog(LOG_INFO, "Port %s: 40G 4-lane TX/RX ASIC reset released",
                       edged.ports[i].ifname);

                /*
                 * Per-lane RX-EQ diagnostic (read-only).  Dump each lane's
                 * adapted VGA gain + TAP1 DFE alongside the CL82 per-lane
                 * AM-lock bitmap, so we can see WHY only some lanes lock:
                 * a lane whose VGA is railed (0 or max) or whose DFE never
                 * settles indicates weak/absent signal or lane skew, vs a
                 * locked lane whose VGA/DFE sit mid-range.  RX0-3 are the
                 * four per-lane analog-status copies; no AER juggling needed.
                 */
                {
                    RX0_ANARXASTATUSr_t e0; RX1_ANARXASTATUSr_t e1;
                    RX2_ANARXASTATUSr_t e2; RX3_ANARXASTATUSr_t e3;
                    CL82_RX_STATUS_3r_t s3;
                    uint32_t aml = 0;
                    int vga[4] = {-1,-1,-1,-1}, dfe[4] = {-1,-1,-1,-1};
                    usleep(200000);  /* allow RX EQ to adapt before sampling */
                    if (READ_CL82_RX_STATUS_3r(pc_q, &s3) >= 0)
                        aml = CL82_RX_STATUS_3r_AM_LOCK_STATEf_GET(s3);
                    if (READ_RX0_ANARXASTATUSr(pc_q, &e0) >= 0) {
                        vga[0] = RX0_ANARXASTATUSr_VGAf_GET(e0);
                        dfe[0] = RX0_ANARXASTATUSr_TAP1_DFE_GRAYf_GET(e0);
                    }
                    if (READ_RX1_ANARXASTATUSr(pc_q, &e1) >= 0) {
                        vga[1] = RX1_ANARXASTATUSr_VGAf_GET(e1);
                        dfe[1] = RX1_ANARXASTATUSr_TAP1_DFE_GRAYf_GET(e1);
                    }
                    if (READ_RX2_ANARXASTATUSr(pc_q, &e2) >= 0) {
                        vga[2] = RX2_ANARXASTATUSr_VGAf_GET(e2);
                        dfe[2] = RX2_ANARXASTATUSr_TAP1_DFE_GRAYf_GET(e2);
                    }
                    if (READ_RX3_ANARXASTATUSr(pc_q, &e3) >= 0) {
                        vga[3] = RX3_ANARXASTATUSr_VGAf_GET(e3);
                        dfe[3] = RX3_ANARXASTATUSr_TAP1_DFE_GRAYf_GET(e3);
                    }
                    syslog(LOG_INFO,
                        "Port %s: 40G EQ am_lock=0x%x L0[vga=%d dfe=%d] L1[vga=%d dfe=%d] L2[vga=%d dfe=%d] L3[vga=%d dfe=%d]",
                        edged.ports[i].ifname, (unsigned)aml,
                        vga[0],dfe[0], vga[1],dfe[1], vga[2],dfe[2], vga[3],dfe[3]);
                }
            }
        }

        /* Debug: comprehensive register dump after config */
        {
            phy_ctrl_t *pc_dbg = BMD_PORT_PHY_CTRL(edged.unit, port);
            if (pc_dbg && i < 2) {
                uint32_t val = 0;

                /* Wait 100ms for PLL and CDR to settle */
                usleep(100000);

                /* Set AER lane for per-lane register reads */
                int wc_lane = PHY_CTRL_INST(pc_dbg) & 0x3;
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0xffd0);
                PHY_BUS_WRITE(pc_dbg, 0x1e, wc_lane);
                syslog(LOG_INFO, "Port %s: AER lane=%d (inst=0x%x)",
                       edged.ports[i].ifname, wc_lane,
                       (unsigned)PHY_CTRL_INST(pc_dbg));

                /* XGXSBLK0: control/status, PLL lock */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x8000);
                PHY_BUS_READ(pc_dbg, 0x10, &val);
                syslog(LOG_INFO, "Port %s: XGXSCONTROLr=0x%04x seq=%d",
                       edged.ports[i].ifname, val & 0xffff, (val >> 13) & 1);
                PHY_BUS_READ(pc_dbg, 0x11, &val);
                syslog(LOG_INFO, "Port %s: XGXSSTATUSr=0x%04x pll_lock=%d",
                       edged.ports[i].ifname, val & 0xffff, (val >> 11) & 1);

                /* MISC1r: speed[4:0], PLL mode */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x8300);
                PHY_BUS_READ(pc_dbg, 0x18, &val);
                syslog(LOG_INFO, "Port %s: MISC1r=0x%04x spd[4:0]=%d pll=0x%x",
                       edged.ports[i].ifname, val & 0xffff,
                       val & 0x1f, (val >> 8) & 0xf);

                /* MISC3r: speed B5, IND_40BITIF */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x8330);
                PHY_BUS_READ(pc_dbg, 0x1c, &val);
                {
                    uint32_t m3 = val & 0xffff;
                    uint32_t b5 = (m3 >> 7) & 1;
                    uint32_t ind40 = (m3 >> 6) & 1;
                    syslog(LOG_INFO, "Port %s: MISC3r=0x%04x B5=%d IND40=%d speed=0x%02x",
                           edged.ports[i].ifname, m3, b5, ind40,
                           (5) | (b5 << 5));
                }

                /* MISC6r: reset, USE_BRCM6466 */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x8340);
                PHY_BUS_READ(pc_dbg, 0x15, &val);
                syslog(LOG_INFO, "Port %s: MISC6r=0x%04x rst_rx=%d rst_tx=%d brcm6466=%d",
                       edged.ports[i].ifname, val & 0xffff,
                       (val >> 15) & 1, (val >> 14) & 1, (val >> 7) & 1);

                /* MISC7r: force_oscdr, CL49 in all mode */
                PHY_BUS_READ(pc_dbg, 0x19, &val);
                syslog(LOG_INFO, "Port %s: MISC7r=0x%04x cl49_all=%d oscdr_force=%d oscdr_val=0x%x",
                       edged.ports[i].ifname, val & 0xffff,
                       (val >> 5) & 1, (val >> 9) & 1, (val >> 10) & 0xf);

                /* FIRMWARE_MODEr */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x8200);
                PHY_BUS_READ(pc_dbg, 0x1a, &val);
                syslog(LOG_INFO, "Port %s: FWMODE=0x%04x l0=%d l1=%d l2=%d l3=%d",
                       edged.ports[i].ifname, val & 0xffff,
                       val & 0xf, (val >> 4) & 0xf,
                       (val >> 8) & 0xf, (val >> 12) & 0xf);

                /* CONTROL1000X1r (CDK 0x8300): fiber mode, autoneg */
                /* Wait — CONTROL1000X1r is in DIGITAL block, CDK 0x8300 = ... */
                /* Actually, CONTROL1000X1r is at block 0x8300, offset 0x10 = reg 0x10+0x10...
                 * No — let me use the CDK address. CONTROL1000X1r CDK might be elsewhere */

                /* DIGITAL block: CONTROL1000X1r = CDK 0x8300 (wait, that's MISC1r block)
                 * Actually SERDESDIGITAL_CONTROL1000X1r = 0x8300 in some drivers,
                 * but in WC it's at a different address. Let me read from block 0x8300 regs. */

                /* CL49 LSM status (64b/66b PCS block lock) */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x8360);
                PHY_BUS_READ(pc_dbg, 0x17, &val);
                syslog(LOG_INFO, "Port %s: CL49_LSM=0x%04x block_lock=%d",
                       edged.ports[i].ifname, val & 0xffff,
                       (val >> 15) & 1);

                /* ANARXSTATUSr: signal detect, CDR status
                 * Read with multiple rxtestsel values to find actual sigdet */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x80b0);
                PHY_BUS_READ(pc_dbg, 0x11, &val);
                {
                    uint32_t rxctrl_orig = val & 0xffff;
                    int sel;
                    for (sel = 0; sel <= 7; sel++) {
                        uint32_t rxctrl_new = (rxctrl_orig & ~0x1f) | sel;
                        PHY_BUS_WRITE(pc_dbg, 0x11, rxctrl_new);
                        PHY_BUS_READ(pc_dbg, 0x10, &val);
                        syslog(LOG_INFO, "Port %s: ANARXSTAT[sel=%d]=0x%04x",
                               edged.ports[i].ifname, sel, val & 0xffff);
                    }
                    /* Restore */
                    PHY_BUS_WRITE(pc_dbg, 0x11, rxctrl_orig);
                }

                /* RX66_CONTROLr: clock compensation enable
                 * CDK 0x83C0 = block 0x83C0, offset 0x0, CL22 reg 0x10 */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x83c0);
                PHY_BUS_READ(pc_dbg, 0x10, &val);
                syslog(LOG_INFO, "Port %s: RX66_CTRL=0x%04x cc_en=%d cc_data=%d",
                       edged.ports[i].ifname, val & 0xffff,
                       val & 1, (val >> 1) & 1);

                /* COMBO_MIICNTLr (IEEE MII control, reg 0x00 page 0)
                 * autoneg_enable = bit 12 */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x0000);
                PHY_BUS_READ(pc_dbg, 0x00, &val);
                syslog(LOG_INFO, "Port %s: MII_CTRL=0x%04x an_en=%d loopback=%d",
                       edged.ports[i].ifname, val & 0xffff,
                       (val >> 12) & 1, (val >> 14) & 1);

                /* MII_STATUS (IEEE reg 1): link, AN complete */
                PHY_BUS_READ(pc_dbg, 0x01, &val);
                syslog(LOG_INFO, "Port %s: MII_STAT=0x%04x link=%d an_done=%d",
                       edged.ports[i].ifname, val & 0xffff,
                       (val >> 2) & 1, (val >> 5) & 1);

                /* AN_IEEECONTROL1r: CL73 AN enable
                 * Block 0x0010 (AN block), reg 0x10 (offset 0) */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x0010);
                PHY_BUS_READ(pc_dbg, 0x10, &val);
                syslog(LOG_INFO, "Port %s: AN_CTRL1=0x%04x cl73_an_en=%d restart=%d",
                       edged.ports[i].ifname, val & 0xffff,
                       (val >> 12) & 1, (val >> 9) & 1);

                /*
                 * Enable CL73 AN so MII_LINK follows AN_DONE.
                 * Nexus eth1/33+1/34 have AN turned on; without AN our
                 * side the chip MAC keeps link=0 even though PCS
                 * block_lock=1, and BMD never enables RX. Setting
                 * cl73_an_en=1 + restart_an=1 lets the handshake fire.
                 */
                val |= (1 << 12);    /* cl73_an_en = 1 */
                val |= (1 << 9);     /* restart_an  = 1 */
                PHY_BUS_WRITE(pc_dbg, 0x10, val & 0xffff);
                usleep(50000);       /* 50 ms for AN to start */
                PHY_BUS_READ(pc_dbg, 0x10, &val);
                syslog(LOG_INFO, "Port %s: AN_CTRL1 after enable=0x%04x",
                       edged.ports[i].ifname, val & 0xffff);

                /* TENGBASE_KR_PMD_CONTROL_150r: CL72 training enable
                 * CDK 0x0096 → IEEE CL45 device 1, reg 0x96
                 * In CL22 AER block: block 0x0090, offset 6, reg 0x16 */
                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x0090);
                PHY_BUS_READ(pc_dbg, 0x16, &val);
                syslog(LOG_INFO, "Port %s: KR_PMD_CTRL=0x%04x training_en=%d",
                       edged.ports[i].ifname, val & 0xffff,
                       (val >> 1) & 1);

                PHY_BUS_WRITE(pc_dbg, 0x1f, 0x0000);
            }
        }

        /*
         * Set TX driver current and pre-emphasis.
         *
         * Values captured from Cumulus 2.5.1 switchd via GDB breakpoints
         * on soc_miim_write (March 27, 2026).
         *
         * Cumulus writes two sets of registers:
         *
         * 1. TX0_TX_DRIVERr (block 0x8060, reg 0x07, addr 0x8067):
         *    OpenMDK PHY_CONFIG_SET API — sets IDRIVER and IPREDRIVER fields.
         *    This is the standard SDK path via PhyConfig_TxIDrv/TxPreIDrv.
         *
         * 2. TX_ANATXACONTROL (block 0x8370, regs 0x10/0x15/0x18):
         *    Analog TX pre-emphasis / FIR filter config.
         *    Written directly via CL22 block addressing.
         *    10G: reg 0x18 = 0x0ACC, reg 0x10 = 0x000E, reg 0x15 = 0x0002
         *    40G: reg 0x18 = 0x0AFF, reg 0x10 = 0x000E
         *
         * Both are needed: TX_DRIVERr sets the output current,
         * ANATXACONTROL sets the FIR/pre-emphasis shaping.
         */
        {
            phy_ctrl_t *pc = BMD_PORT_PHY_CTRL(edged.unit, port);
            if (pc) {
                int is_qsfp = (edged.ports[i].port_type == PORT_TYPE_QSFP);
                int lane;

                /*
                 * Part 1: TX_DRIVERr via OpenMDK PHY_CONFIG_SET API.
                 * Decoded from Cumulus capture of TX0_TX_DRIVERr (0x8067):
                 *   IDRIVER = 10 (0xA), IPREDRIVER = 12 (0xC) for 10G
                 *   IDRIVER = 10 (0xA), IPREDRIVER = 15 (0xF) for 40G
                 */
                rv = PHY_CONFIG_SET(pc, PhyConfig_TxIDrv, 10, NULL);
                if (rv < 0)
                    syslog(LOG_WARNING, "Port %s: PhyConfig_TxIDrv failed: %d",
                           edged.ports[i].ifname, rv);

                rv = PHY_CONFIG_SET(pc, PhyConfig_TxPreIDrv,
                                    is_qsfp ? 15 : 12, NULL);
                if (rv < 0)
                    syslog(LOG_WARNING, "Port %s: PhyConfig_TxPreIDrv failed: %d",
                           edged.ports[i].ifname, rv);

                /*
                 * Part 2: TX_ANATXACONTROL (block 0x8370) via direct
                 * CL22 block access. Per-lane via AER lane select.
                 */
                for (lane = 0; lane < 4; lane++) {
                    /* Select lane via AER */
                    PHY_BUS_WRITE(pc, 0x1f, 0xffd0);
                    PHY_BUS_WRITE(pc, 0x1e, 0x1800 + lane);

                    /* TX_ANATXACONTROL6 (reg 0x18): amplitude */
                    PHY_BUS_WRITE(pc, 0x1f, 0x8370);
                    PHY_BUS_WRITE(pc, 0x18, is_qsfp ? 0x0AFF : 0x0ACC);

                    /* TX_ANATXACONTROL0 (reg 0x10): FIR enable */
                    PHY_BUS_WRITE(pc, 0x10, 0x000E);

                    /* TX_ANATXACONTROL5 (reg 0x15): FIR coeff (10G only) */
                    if (!is_qsfp)
                        PHY_BUS_WRITE(pc, 0x15, 0x0002);
                }

                /* Restore AER to broadcast */
                PHY_BUS_WRITE(pc, 0x1f, 0xffd0);
                PHY_BUS_WRITE(pc, 0x1e, 0x0000);
                PHY_BUS_WRITE(pc, 0x1f, 0x0000);

                syslog(LOG_INFO, "Port %s: TX driver idrv=10 ipre=%d + ANATX 0x%04x (%s)",
                       edged.ports[i].ifname,
                       is_qsfp ? 15 : 12,
                       is_qsfp ? 0x0AFF : 0x0ACC,
                       is_qsfp ? "40G QSFP" : "10G SFP+");
            } else {
                syslog(LOG_WARNING, "Port %s: no PHY control (port %d)",
                       edged.ports[i].ifname, port);
            }
        }

        edged.ports[i].enabled = 1;
        configured++;

        syslog(LOG_INFO, "Port %s: logical=%d lane=%d speed=%dG type=%s mode=0x%x OK",
               edged.ports[i].ifname,
               edged.ports[i].logical_port,
               edged.ports[i].physical_lane,
               edged.ports[i].speed / 1000,
               edged.ports[i].port_type == PORT_TYPE_QSFP ? "QSFP" : "SFP",
               mode);
    }

    edged.num_ports = configured;
    syslog(LOG_INFO, "Configured %d ports", configured);
    return 0;
}

int portmap_swp_to_logical(int swp)
{
    if (swp < 1 || swp > EDGED_MAX_PORTS)
        return -1;
    return edged.ports[swp - 1].logical_port;
}

int portmap_logical_to_swp(int logical)
{
    int i;
    for (i = 0; i < EDGED_MAX_PORTS; i++) {
        if (edged.ports[i].logical_port == logical)
            return i + 1;
    }
    return -1;
}

/* Map CDK physical port number to front-panel swp number */
int portmap_phys_to_swp(int phys_port)
{
    int i;
    for (i = 0; i < EDGED_MAX_PORTS; i++) {
        if (edged.ports[i].physical_lane == phys_port)
            return i + 1;
    }
    return -1;
}

int portmap_swp_to_i2c_bus(int swp)
{
    if (swp < 1 || swp > EDGED_MAX_PORTS)
        return -1;
    return port_to_i2c_bus[swp - 1];
}

/*
 * Poll all ports for link state changes.
 *
 * Called from the main loop. Uses bmd_port_mode_update() which:
 *   1. Reads PHY link status via MIIM (MII_STATUS on WC page 0x1800)
 *   2. On link-up: enables MAC RX, sets EPC_LINK_BMAP bit
 *   3. On link-down: disables MAC RX, clears EPC_LINK_BMAP bit
 *   4. Updates LED data at LED_DATA_OFFSET (0x80) for CMIC LED processor
 *
 * Cumulus polls at ~30ms per port. We poll all ports each call,
 * called from main loop with appropriate interval.
 */
static int link_poll_count;

/* 40G QSFP auto re-init/retry state (2026-06-07).
 *
 * A QSFP port that comes up at boot before its loopback/link peer's TX is live
 * ends up at am_lock=0x3 with deskew NEVER engaging. Re-initializing the port
 * AFTER both peers' TX are live (a "hot-plug retry") engages deskew and shifts
 * to am_lock=0x6 — verified live. So we retry: any QSFP port that is up but not
 * fully AM-locked (am_lock != 0xf) gets re-initialized (bmd_port_mode_set
 * disable->40G) after QSFP_RETRY_GRACE_S of being unlocked, up to
 * QSFP_RETRY_MAX times. Wall-clock timed (poll cadence-independent). */
#define QSFP_RETRY_GRACE_S 40
#define QSFP_RETRY_MAX     8
static time_t qsfp_unlocked_since[EDGED_MAX_PORTS];
static int    qsfp_retry_count[EDGED_MAX_PORTS];

/*
 * Read CL49 PCS block_lock for a port.  Returns 1 if the chip's
 * 10G 64b/66b framer has acquired sync, else 0.  block_lock=1 is
 * the true L1 indicator for SFP+ optical links — Warpcore's MII
 * STATUS bit never asserts in forced-10G no-AN mode (which is what
 * SFP+ optical effectively is), so we have to drive link state
 * from the PCS layer directly.  This mirrors what Cumulus's
 * switchd linkscan does on the same hardware.
 */
static int pcs_block_lock_get(int unit, int port)
{
    phy_ctrl_t *pc = BMD_PORT_PHY_CTRL(unit, port);
    uint32_t val = 0;
    int lane;

    if (!pc)
        return 0;

    lane = PHY_CTRL_INST(pc) & 0x3;

    /* AER lane select (Warpcore per-lane register addressing).
     * Without this, reads from a 4-lane WC slice always hit lane 0. */
    PHY_BUS_WRITE(pc, 0x1f, 0xffd0);
    PHY_BUS_WRITE(pc, 0x1e, lane);

    /* CL49_LSM_STATUSr — block 0x8360 / reg 0x17.
     * Bit 15 is BLOCK_LOCK (PCS 64b/66b synced). */
    PHY_BUS_WRITE(pc, 0x1f, 0x8360);
    PHY_BUS_READ(pc, 0x17, &val);

    PHY_BUS_WRITE(pc, 0x1f, 0x0000);

    return (val >> 15) & 1;
}

/*
 * Read 40G CL82 (MLD/BAM) link status for a QSFP port.  Returns 1 only when
 * the full 4-lane PCS is up: alignment-marker lock on ALL 4 lanes AND the
 * lanes are deskewed/aligned.
 *
 * The CL49 single-lane block_lock check (pcs_block_lock_get) is for 10G SFP+
 * and only reflects lane 0 — it gives a false "up" for a 40G port whose other
 * 3 lanes or alignment haven't come up.  40GBASE-R4 distributes the stream
 * across 4 lanes (MLD) and the receiver must AM-lock each lane and deskew them
 * before the link is usable, so QSFP ports need this CL82 aggregate check.
 *
 *   CL82_RX_STATUS_3.AM_LOCK_STATE (bits 10:13) = per-lane AM lock; 0xF = all 4.
 *   CL82_RX_STATUS_2.DESKEW_STATE  (bit 14)     = 1 when the 4 lanes are deskewed.
 * AM-lock implies per-lane 64b/66b block_lock, so these two together are the
 * full link indicator (== IEEE 802.3 Clause 82 align_status).
 *
 * The SDK register accessors (READ_CL82_RX_STATUS_*r) handle the Warpcore AER
 * lane-select + indirect-block addressing internally via the port's phy_ctrl.
 */
static int cl82_link_get(int unit, int port, int *am_lock_out, int *deskew_out)
{
    phy_ctrl_t *pc = BMD_PORT_PHY_CTRL(unit, port);
    CL82_RX_STATUS_2r_t rx2;
    CL82_RX_STATUS_3r_t rx3;
    int am_lock = 0, deskew = 0;

    if (pc &&
        READ_CL82_RX_STATUS_3r(pc, &rx3) >= 0 &&
        READ_CL82_RX_STATUS_2r(pc, &rx2) >= 0) {
        am_lock = CL82_RX_STATUS_3r_AM_LOCK_STATEf_GET(rx3);  /* 4 bits, one per lane */
        deskew  = CL82_RX_STATUS_2r_DESKEW_STATEf_GET(rx2);   /* 1 = deskew done */
    }

    if (am_lock_out) *am_lock_out = am_lock;
    if (deskew_out)  *deskew_out = deskew;

    return (am_lock == 0xf) && deskew;
}

/*
 * Per-lane RX-EQ dump for a 40G QSFP port (read-only).  Logs the CL82
 * per-lane AM-lock bitmap + deskew alongside each lane's adapted VGA gain
 * and TAP1 DFE.  Shared by the one-shot config-time diagnostic and the
 * throttled steady-state dump in portmap_link_poll() — the steady-state
 * read is the trustworthy one (the config-time snapshot samples a port
 * 200ms after IT is configured, before a later-configured loopback partner's
 * TX is up, so the earlier port falsely reads dark).
 */
static void qsfp_eq_dump(int unit, int port, const char *ifname)
{
    phy_ctrl_t *pc = BMD_PORT_PHY_CTRL(unit, port);
    RX0_ANARXASTATUSr_t e0; RX1_ANARXASTATUSr_t e1;
    RX2_ANARXASTATUSr_t e2; RX3_ANARXASTATUSr_t e3;
    CL82_RX_STATUS_2r_t s2; CL82_RX_STATUS_3r_t s3;
    uint32_t aml = 0, dsk = 0;
    int vga[4] = {-1,-1,-1,-1}, dfe[4] = {-1,-1,-1,-1};
    if (!pc) return;
    if (READ_CL82_RX_STATUS_3r(pc, &s3) >= 0)
        aml = CL82_RX_STATUS_3r_AM_LOCK_STATEf_GET(s3);
    if (READ_CL82_RX_STATUS_2r(pc, &s2) >= 0)
        dsk = CL82_RX_STATUS_2r_DESKEW_STATEf_GET(s2);
    if (READ_RX0_ANARXASTATUSr(pc, &e0) >= 0) {
        vga[0] = RX0_ANARXASTATUSr_VGAf_GET(e0);
        dfe[0] = RX0_ANARXASTATUSr_TAP1_DFE_GRAYf_GET(e0);
    }
    if (READ_RX1_ANARXASTATUSr(pc, &e1) >= 0) {
        vga[1] = RX1_ANARXASTATUSr_VGAf_GET(e1);
        dfe[1] = RX1_ANARXASTATUSr_TAP1_DFE_GRAYf_GET(e1);
    }
    if (READ_RX2_ANARXASTATUSr(pc, &e2) >= 0) {
        vga[2] = RX2_ANARXASTATUSr_VGAf_GET(e2);
        dfe[2] = RX2_ANARXASTATUSr_TAP1_DFE_GRAYf_GET(e2);
    }
    if (READ_RX3_ANARXASTATUSr(pc, &e3) >= 0) {
        vga[3] = RX3_ANARXASTATUSr_VGAf_GET(e3);
        dfe[3] = RX3_ANARXASTATUSr_TAP1_DFE_GRAYf_GET(e3);
    }
    syslog(LOG_INFO,
        "Port %s: 40G EQ am_lock=0x%x deskew=%d L0[vga=%d dfe=%d] L1[vga=%d dfe=%d] L2[vga=%d dfe=%d] L3[vga=%d dfe=%d]",
        ifname, (unsigned)aml, (unsigned)dsk,
        vga[0],dfe[0], vga[1],dfe[1], vga[2],dfe[2], vga[3],dfe[3]);

    /* Extended CL82 PCS diagnostic (2026-06-07): why do 2/4 lanes fail AM-lock?
     * Dumps the alignment-marker error flags, AM-remove-FIFO state, pseudo-lock,
     * the RX lane-swap mux (virtual->physical), and raw status words. Compare
     * against the Cumulus 4/4 capture to find the AM-lock root cause. */
    {
        CL82_RX_STATUS_4r_t s4;
        RXLNSWAP1r_t lns;
        unsigned amspace = 0, amnonuniq = 0, pseudo = 0;
        unsigned am_status = 0, amfifo = 0, amovf = 0, amunf = 0;
        int sw[4] = {-1,-1,-1,-1};
        unsigned raw2 = (unsigned)(s2.v[0] & 0xffff);
        unsigned raw3 = (unsigned)(s3.v[0] & 0xffff);
        unsigned raw4 = 0;
        amspace   = CL82_RX_STATUS_3r_AMRKR_SPACING_ERR_LATCH_MUXf_GET(s3);
        amnonuniq = CL82_RX_STATUS_3r_NON_UNIQUE_AMRKR_ERR_MUXf_GET(s3);
        pseudo    = CL82_RX_STATUS_2r_PSEUDO_LOCKf_GET(s2);
        if (READ_CL82_RX_STATUS_4r(pc, &s4) >= 0) {
            raw4   = (unsigned)(s4.v[0] & 0xffff);
            am_status = CL82_RX_STATUS_4r_AM_STATUSf_GET(s4);
            amfifo = CL82_RX_STATUS_4r_AM_RMFIFO_STATEf_GET(s4);
            amovf  = CL82_RX_STATUS_4r_AM_RMFIFO_OVERFLOWf_GET(s4);
            amunf  = CL82_RX_STATUS_4r_AM_RMFIFO_UNDERFLOWf_GET(s4);
        }
        if (READ_RXLNSWAP1r(pc, &lns) >= 0) {
            sw[0] = RXLNSWAP1r_RX0_LNSWAP_SELf_GET(lns);
            sw[1] = RXLNSWAP1r_RX1_LNSWAP_SELf_GET(lns);
            sw[2] = RXLNSWAP1r_RX2_LNSWAP_SELf_GET(lns);
            sw[3] = RXLNSWAP1r_RX3_LNSWAP_SELf_GET(lns);
        }
        syslog(LOG_INFO,
            "Port %s: CL82DIAG amspace_err=0x%x nonuniq_am=0x%x pseudo=%u am_status=0x%x amfifo=0x%x ovf=%u unf=%u lnswap=[%d %d %d %d] raw2=0x%04x raw3=0x%04x raw4=0x%04x",
            ifname, amspace, amnonuniq, pseudo, am_status, amfifo, amovf, amunf,
            sw[0],sw[1],sw[2],sw[3], raw2, raw3, raw4);
    }

    /* Full CL82/PCS block dump (2026-06-07), gated on /tmp/cl82dump (one read
     * per poll while present). Reads 0x8100-0x816f + 0x8420-0x844f on BOTH
     * dual-block AER contexts (lane 0 and lane 2) via raw MIIM, so we can diff
     * EdgeNOS's live CL82 state register-by-register vs the Cumulus 4/4 capture
     * (docs/cumulus_wc_full_dump_2026_06_07.txt) and find the exact remaining
     * delta behind nonuniq_am. AER is restored to 0 after. */
    {
        FILE *df = fopen("/tmp/cl82dump", "r");
        if (df) {
            extern int cdk_xgs_miim_read(int, uint32_t, uint32_t, uint32_t *);
            extern int cdk_xgs_miim_write(int, uint32_t, uint32_t, uint32_t);
            int u = PHY_CTRL_UNIT(pc);
            uint32_t pa = PHY_CTRL_ADDR(pc);
            int blk, reg;
            uint32_t v;
            fclose(df);
            for (blk = 0; blk <= 2; blk += 2) {
                cdk_xgs_miim_write(u, pa, (1 << 16) | 0xFFDE, blk);
                for (reg = 0x8100; reg <= 0x844f; reg++) {
                    /* skip the 0x8170-0x841f gap (not CL82/PCS-relevant here) */
                    if (reg == 0x8170) reg = 0x8420;
                    v = 0;
                    if (cdk_xgs_miim_read(u, pa, (1 << 16) | reg, &v) < 0) continue;
                    v &= 0xffff;
                    if (v == 0 || v == (uint32_t)reg) continue; /* skip 0 + addr-echo */
                    syslog(LOG_INFO, "Port %s: CL82BLK blk%d 0x%x=0x%04x",
                           ifname, blk, reg, v);
                }
            }
            cdk_xgs_miim_write(u, pa, (1 << 16) | 0xFFDE, 0);
        }
    }
}

int portmap_link_poll(void)
{
    int i;
    int changes = 0;
    int first_poll = (link_poll_count++ == 0);

    for (i = 0; i < EDGED_MAX_PORTS; i++) {
        if (!edged.ports[i].valid || !edged.ports[i].enabled)
            continue;

        int port = edged.ports[i].physical_lane;  /* CDK port */
        int old_link = edged.ports[i].link_up;

        /*
         * Drive link state from PCS block_lock, NOT MII_STATUS.
         *
         * SFP+ optical links (our case for swp1..swp48) come up as
         * forced 10G with no autoneg.  In that mode Warpcore's
         * MII_STATUS link bit never asserts, so bmd_phy_link_get()
         * returns link=0 forever even when the wire is healthy.
         *
         * BMD supports overriding this — if BMD_PST_FORCE_LINK is set
         * on a port, bmd_link_update() skips phy_link_get and uses
         * whatever BMD_PST_LINK_UP we set.  We pre-set both flags
         * based on the PCS block_lock state read straight from the
         * Warpcore CL49 LSM register, then call bmd_port_mode_update
         * — it transitions MAC RX_EN + EPC_LINK_BMAP through the
         * normal SDK code path, just without trusting MII_STATUS.
         */
        /* 40G QSFP ports use CL82 (4-lane AM-lock + deskew); 10G SFP+ use
         * CL49 single-lane block_lock. */
        int cl82_am = 0, cl82_deskew = 0;
        int pcs_link;
        if (edged.ports[i].port_type == PORT_TYPE_QSFP)
            pcs_link = cl82_link_get(edged.unit, port, &cl82_am, &cl82_deskew);
        else
            pcs_link = pcs_block_lock_get(edged.unit, port);
        if (pcs_link) {
            BMD_PORT_STATUS_SET(edged.unit, port,
                                BMD_PST_FORCE_LINK | BMD_PST_LINK_UP);
        } else {
            BMD_PORT_STATUS_CLR(edged.unit, port,
                                BMD_PST_FORCE_LINK | BMD_PST_LINK_UP);
        }

        bmd_port_mode_update(edged.unit, port);

        /* 40G QSFP auto re-init/retry: re-init a port that's up but not fully
         * AM-locked, once its peer's TX has had time to come live. Engages
         * deskew (which never fires from the boot-order init) and re-adapts. */
        if (edged.ports[i].port_type == PORT_TYPE_QSFP) {
            if (cl82_am == 0xf) {
                qsfp_unlocked_since[i] = 0;   /* fully locked: clear timer */
                qsfp_retry_count[i] = 0;
            } else {
                time_t now = time(NULL);
                if (qsfp_unlocked_since[i] == 0) {
                    qsfp_unlocked_since[i] = now;   /* start grace window */
                } else if (qsfp_retry_count[i] < QSFP_RETRY_MAX &&
                           (now - qsfp_unlocked_since[i]) >= QSFP_RETRY_GRACE_S) {
                    bmd_port_mode_t m = (edged.ports[i].speed >= 40000)
                                        ? bmdPortMode40000fd : bmdPortMode10000fd;
                    syslog(LOG_INFO,
                        "qsfp_retry[%s]: port=%d re-init #%d (am_lock=0x%x deskew=%d)",
                        edged.ports[i].ifname, port, qsfp_retry_count[i] + 1,
                        cl82_am, cl82_deskew);
                    bmd_port_mode_set(edged.unit, port, bmdPortModeDisabled, 0);
                    bmd_port_mode_set(edged.unit, port, m, 0);
                    qsfp_retry_count[i]++;
                    qsfp_unlocked_since[i] = now;   /* restart grace for next retry */
                }
            }
        }

        if (first_poll && (i < 3 || edged.ports[i].port_type == PORT_TYPE_QSFP)) {
            if (edged.ports[i].port_type == PORT_TYPE_QSFP)
                syslog(LOG_INFO, "link_poll[%s]: port=%d CL82 am_lock=0x%x deskew=%d link=%d",
                       edged.ports[i].ifname, port, cl82_am, cl82_deskew, pcs_link);
            else
                syslog(LOG_INFO, "link_poll[%s]: port=%d pcs_link=%d",
                       edged.ports[i].ifname, port, pcs_link);
        }

        /* Steady-state per-lane EQ dump for QSFP: every 8th poll (all ports
         * are up by then, so this reflects real convergence — unlike the
         * config-time one-shot). */
        if (edged.ports[i].port_type == PORT_TYPE_QSFP &&
            (link_poll_count % 8) == 1) {
            qsfp_eq_dump(edged.unit, port, edged.ports[i].ifname);
        }

        edged.ports[i].link_up = pcs_link;
        if (pcs_link != old_link) {
            syslog(LOG_INFO, "BMD link %s: port %d (%s) via PCS block_lock",
                   pcs_link ? "UP" : "DOWN", port,
                   edged.ports[i].ifname);
            changes++;
        }
    }

    return changes;
}
