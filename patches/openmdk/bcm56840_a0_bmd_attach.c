#include <bmd_config.h>
#if CDK_CONFIG_INCLUDE_BCM56840_A0 == 1

/*
 * 
 *
 * This software is governed by the Broadcom Switch APIs license.
 * This license is set out in https://raw.githubusercontent.com/Broadcom-Network-Switching-Software/OpenMDK/master/Legal/LICENSE file.
 * 
 * Copyright 2007-2020 Broadcom Inc. All rights reserved.
 */

#include <bmd/bmd.h>
#include <bmd/bmd_device.h>
#include <bmd/bmd_phy_ctrl.h>

#include <cdk/chip/bcm56840_a0_defs.h>
#include <cdk/arch/xgs_chip.h>
#include <cdk/cdk_debug.h>
#include <cdk/cdk_assert.h>

#include "bcm56840_a0_bmd.h"
#include "bcm56840_a0_internal.h"

#if BMD_CONFIG_INCLUDE_PHY == 1

#include <phy/phy_buslist.h>

static phy_bus_t *bcm56840_phy_bus[] = {
#ifdef PHY_BUS_BCM56840_MIIM_INT_INSTALLED
    &phy_bus_bcm56840_miim_int,
#endif
#ifdef PHY_BUS_BCM956840K_MIIM_EXT_INSTALLED
    &phy_bus_bcm956840k_miim_ext,
#endif
    NULL
};

#define PHY_BUS_SET(_u,_p,_b) bmd_phy_bus_set(_u,_p,_b)

#else

#define PHY_BUS_SET(_u,_p,_b)

#endif

#if CDK_CONFIG_INCLUDE_PORT_MAP == 1

static struct _port_map_s {
    cdk_port_map_port_t map[BMD_CONFIG_MAX_PORTS];
} _port_map[BMD_CONFIG_MAX_UNITS];

static void
_init_port_map(int unit)
{
    cdk_pbmp_t pbmp;
    int lport, lport_max = 0;
    int port;

    CDK_MEMSET(&_port_map[unit], -1, sizeof(_port_map[unit]));

    _port_map[unit].map[0] = CMIC_PORT;

    bcm56840_a0_xlport_pbmp_get(unit, &pbmp);
    CDK_PBMP_ITER(pbmp, port) {
        lport = P2L(unit, port);
        _port_map[unit].map[lport] = port;
        if (lport > lport_max) {
            lport_max = lport;
        }
    }

    CDK_PORT_MAP_SET(unit, _port_map[unit].map, lport_max + 1);
}

#endif

/*
 * The MMU port mappings must be derived from the individual
 * port configurations such as maximum speed and queueing
 * capabilities.
 */
static struct _mmu_port_map_s {
    cdk_port_map_port_t map[BMD_CONFIG_MAX_PORTS];
} _mmu_port_map[BMD_CONFIG_MAX_UNITS];

static int
_check_port_config(int unit)
{
    int pipe, group, blk, pdx, num_lanes;
    int port, sub_port, pg_base, blk_base;
    uint32_t tot_bw, pg_bw, blk_bw;
    uint32_t blk_mask, mask;

    tot_bw = 480000;
    if (CDK_XGS_FLAGS(unit) & CHIP_FLAG_BW640G) {
        tot_bw = 640000;
    } else if (CDK_XGS_FLAGS(unit) & CHIP_FLAG_BW320G) {
        tot_bw = 320000;
    }

    for (pipe = 0; pipe < 2; pipe++) {
        for (group = 0; group < 2; group++) {
            pg_bw = 0;
            pg_base = 1 + pipe * 36 + group * 20;
            blk_base = 0;
            for (blk = 0; blk < (group ? 4 : 5); blk++) {
                blk_bw = 0;
                blk_base = pg_base + blk * 4;
                blk_mask = 0;
                /* Skip QGPORT (1G x 4) in XLPORT0 block (if present) */
                if (blk_base == 1 && bcm56840_a0_has_qgport(unit)) {
                    continue;
                }
                for (pdx = 0; pdx < 4; pdx++) {
                    port = blk_base + pdx;
                    blk_bw += bcm56840_a0_port_speed_max(unit, port);
                    num_lanes = bcm56840_a0_port_num_lanes(unit, port);
                    sub_port = XLPORT_SUBPORT(port);
                    if (num_lanes == 4 && sub_port != 0) {
                        CDK_WARN(("Port %d cannot be a 4-lane port\n",
                                  port));
                        return CDK_E_CONFIG;
                    }
                    if (num_lanes == 2 && (sub_port & 1) != 0) {
                        CDK_WARN(("Port %d cannot be a 2-lane port\n",
                                  port));
                        return CDK_E_CONFIG;
                    }
                    mask = ((1 << num_lanes) - 1) << pdx;
                    if ((mask & blk_mask) != 0) {
                        CDK_WARN(("Overlapping lanes (ports %d-%d)\n",
                                  blk_base, blk_base + 3));
                        return CDK_E_CONFIG;
                    }
                    blk_mask |= mask;
                }
                if (blk_bw > 40000) {
                    CDK_WARN(("Port block bandwidth exceeded (ports %d-%d)\n",
                              blk_base, blk_base + 3));
                    return CDK_E_CONFIG;
                }
                pg_bw += blk_bw;
            }
            if (pg_bw > (tot_bw / 4)) {
                CDK_WARN(("Port group bandwidth exceeded (ports %d-%d)\n",
                          pg_base, blk_base + 3));
                return CDK_E_CONFIG;
            }
        }
    }

    return CDK_E_NONE;
}

static void
_init_mmu_port_map(int unit)
{
    uint32_t port_flags;
    cdk_pbmp_t pbmp;
    cdk_pbmp_t pbmp_1fifo[2], pbmp_2fifo[2], pbmp_4fifo[2];
    int eq_port[2][4], port_count[2], eq_port_count[2];
    int mmu_port_base[2];
    int fifo_idx, fifo_count[2], extra_fifo_count[2], unused_fifo[12];
    int num_lanes, idx, count, pipe, port, mmu_port;

    /* All configured physical ports */
    bcm56840_a0_xlport_pbmp_get(unit, &pbmp);

    /* Clear MMU port map */
    CDK_MEMSET(&_mmu_port_map[unit], -1, sizeof(_mmu_port_map[unit]));

    /* Count ports in each pipe */
    port_count[0] = port_count[1] = 0;
    eq_port_count[0] = eq_port_count[1] = 0;
    CDK_PBMP_ITER(pbmp, port) {
        pipe = (port < 37) ? 0 : 1;
        port_count[pipe]++;
        port_flags = CDK_PORT_CONFIG_PORT_FLAGS(unit, port);
        if (port_flags & CDK_DCFG_PORT_F_EXT_QUEUE) {
            pipe = port < 37 ? 0 : 1;
            eq_port[pipe][eq_port_count[pipe]] = port;
            eq_port_count[pipe]++;
        }
    }
    /* Sanity check */
    for (pipe = 0; pipe < 2; pipe++) {
        if (port_count[pipe] > 32 || eq_port_count[pipe] > 4) {
            CDK_WARN(("bcm56840_a0_bmd_attach[%d]: MMU map error (%d %d %d)\n",
                      unit, pipe, port_count[pipe], eq_port_count[pipe]));
        }
    }

    mmu_port_base[0] = 1;
    mmu_port_base[1] = 34;
    fifo_count[0] = fifo_count[1] = 0;

    /*
     * To improve multicast performance, try assigning 4 fifo (instead of 1)
     * for ports in single mode or 2 fifo for ports in dual mode. It is not
     * always possible to assign such extra fifo when special GE port in
     * XLPORT0 is in user portmap.
     */
    CDK_MEMSET(pbmp_1fifo, 0, sizeof(pbmp_1fifo));
    CDK_MEMSET(pbmp_2fifo, 0, sizeof(pbmp_2fifo));
    CDK_MEMSET(pbmp_4fifo, 0, sizeof(pbmp_4fifo));
    extra_fifo_count[0] = extra_fifo_count[1] = 0;

    CDK_PBMP_ITER(pbmp, port) {
        pipe = (port < 37) ? 0 : 1;
        num_lanes = bcm56840_a0_port_num_lanes(unit, port);
        if (num_lanes == 4) {
            if (port_count[pipe] + extra_fifo_count[pipe] + 3 <= 32) {
                /* More than 3 unused fifo can be assigned to this port */
                CDK_PBMP_PORT_ADD(pbmp_4fifo[pipe], port);
                extra_fifo_count[pipe] += 3;
            } else {
                CDK_PBMP_PORT_ADD(pbmp_1fifo[pipe], port);
            }
        } else if (num_lanes == 2) {
            /* More than 1 unused fifo can be assigned to this port */
            if (port_count[pipe] + extra_fifo_count[pipe] + 1 <= 32) {
                CDK_PBMP_PORT_ADD(pbmp_2fifo[pipe], port);
                extra_fifo_count[pipe] += 1;
            } else {
                CDK_PBMP_PORT_ADD(pbmp_1fifo[pipe], port);
            }
        } else if (num_lanes == 1) {
            CDK_PBMP_PORT_ADD(pbmp_1fifo[pipe], port);
        }
    }

    /* User specified extended queuing ports */
    for (pipe = 0; pipe < 2; pipe++) {
        for (idx = 0; idx < eq_port_count[pipe]; idx++) {
            port = eq_port[pipe][idx];
            mmu_port = mmu_port_base[pipe] + idx;
            fifo_count[pipe] += 4;
            _mmu_port_map[unit].map[port] = mmu_port;
        }
    }

    /* Assign all 4 FIFOs in a MCQ group to each single mode XLPORT */
    for (pipe = 0; pipe < 2; pipe++) {
        CDK_PBMP_ITER(pbmp_4fifo[pipe], port) {
            if (_mmu_port_map[unit].map[port] != -1) {
                continue;
            }
            if (fifo_count[pipe] == 32) {
                break;
            }
            fifo_idx = fifo_count[pipe];
            /* 1, 2, 3, 4, 17, 18, 19, 20 */
            mmu_port = mmu_port_base[pipe] +
                ((fifo_idx & 0x10) | ((fifo_idx & 0xc) >> 2));
            fifo_count[pipe] += 4;
            _mmu_port_map[unit].map[port] = mmu_port;
        }
    }

    /* Assign 2 consecutive FIFOs in a MCQ group to each dual mode XLPORT */
    for (pipe = 0; pipe < 2; pipe++) {
        /* Find unused FIFO from the user-specified extended queue ports */
        count = 0;
        for (idx = 0; idx < eq_port_count[pipe]; idx++) {
            port = eq_port[pipe][idx];
            if (!CDK_PBMP_MEMBER(pbmp_4fifo[pipe], port)) {
                unused_fifo[count] = idx + 8;
                count++;
            }
        }

        idx = 0;
        CDK_PBMP_ITER(pbmp_2fifo[pipe], port) {
            if (_mmu_port_map[unit].map[port] != -1) {
                continue;
            }
            if (idx < count) {
                mmu_port = mmu_port_base[pipe] + unused_fifo[idx];
                idx++;
            } else if (fifo_count[pipe] == 32) {
                break;
            } else {
                fifo_idx = fifo_count[pipe];
                /* 1, 9, 2, 10, 3, 11, 4, 12, 17, 25, 18, 26, 19, 27, 20, 28 */
                mmu_port =  mmu_port_base[pipe] +
                    ((fifo_idx & 0x10) | ((fifo_idx & 0xc) >> 2) |
                     ((fifo_idx & 0x2) << 2));
                fifo_count[pipe] += 2;
            }
            _mmu_port_map[unit].map[port] = mmu_port;
        }
    }

    /* Assign fifo to each quad mode XLPORT */
    for (pipe = 0; pipe < 2; pipe++) {
        /* Find unused FIFO from the user-specified extended queue ports */
        count = 0;
        for (idx = 0; idx < eq_port_count[pipe]; idx++) {
            port = eq_port[pipe][idx];
            if (CDK_PBMP_MEMBER(pbmp_1fifo[pipe], port)) {
                unused_fifo[count] = idx + 4;
                count++;
            }
            if (!CDK_PBMP_MEMBER(pbmp_4fifo[pipe], port) &&
                M2P(unit, mmu_port_base[pipe] + idx + 8) == -1) {
                unused_fifo[count] = idx + 8;
                count++;
                unused_fifo[count] = idx + 12;
                count++;
            }
        }

        idx = 0;
        CDK_PBMP_ITER(pbmp_1fifo[pipe], port) {
            if (_mmu_port_map[unit].map[port] != -1) {
                continue;
            }
            if (idx < count) {
                mmu_port = mmu_port_base[pipe] + unused_fifo[idx];
                idx++;
            } else  if (fifo_count[pipe] == 32) {
                break;
            } else {
                fifo_idx = fifo_count[pipe];
                /* 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15, 4, 8, 12, 16,
                 * 17, 21, 25, 29, 18, 22, 26, 30, 19, 23, 27, 31, 20, 24,
                 * 28, 32
                 */
                mmu_port = mmu_port_base[pipe] +
                    ((fifo_idx & 0x10) | ((fifo_idx & 0xc) >> 2) |
                     ((fifo_idx & 0x3) << 2));
                fifo_count[pipe]++;
            }
            _mmu_port_map[unit].map[port] = mmu_port;
        }
    }
}

static void
_remove_xlports(cdk_pbmp_t *pbmp, const int *xlp_blk_excl, int size)
{
    int idx;
    int port;

    for (idx = 0; idx < size; idx++) {
        port = (xlp_blk_excl[idx] * 4) + 1;
        CDK_PBMP_PORT_REMOVE(*pbmp, port);
        CDK_PBMP_PORT_REMOVE(*pbmp, port + 1);
        CDK_PBMP_PORT_REMOVE(*pbmp, port + 2);
        CDK_PBMP_PORT_REMOVE(*pbmp, port + 3);
    }
}

static uint32_t
_port_speed_max(int unit, int port)
{
    cdk_pbmp_t pbmp;

    /* Use per-port config if available */
    if (CDK_NUM_PORT_CONFIGS(unit) != 0) {
        return CDK_PORT_CONFIG_SPEED_MAX(unit, port);
    }

    /* Default port configurations */
    CDK_XGS_BLKTYPE_PBMP_GET(unit, BLKTYPE_XLPORT, &pbmp);
    if (CDK_XGS_FLAGS(unit) & CHIP_FLAG_HGONLY) {
        const int xlp_blk_excl[] = { 0, 13 };
        _remove_xlports(&pbmp, xlp_blk_excl, COUNTOF(xlp_blk_excl));
    } else if (CDK_XGS_FLAGS(unit) & CHIP_FLAG_BW640G) {
        const int xlp_blk_excl[] = { 0, 13 };
        _remove_xlports(&pbmp, xlp_blk_excl, COUNTOF(xlp_blk_excl));
    } else if (CDK_XGS_FLAGS(unit) & CHIP_FLAG_BW320G) {
        const int xlp_blk_excl[] = { 1, 2, 5, 6, 11, 12, 13, 16, 17 };
        _remove_xlports(&pbmp, xlp_blk_excl, COUNTOF(xlp_blk_excl));
    } else {
        const int xlp_blk_excl[] = { 1, 5, 12, 13, 17 };
        _remove_xlports(&pbmp, xlp_blk_excl, COUNTOF(xlp_blk_excl));
    }
    if (CDK_PBMP_MEMBER(pbmp, port) == 0) {
        return 0;
    }

    /* Default port speeds for fixed configurations */
    if (port >= 1 && port <= 4) {
        return 1000;
    }
    if (CDK_XGS_FLAGS(unit) & CHIP_FLAG_HGONLY) {
        if (XLPORT_SUBPORT(port) == 0) {
            if (CDK_XGS_FLAGS(unit) & CHIP_FLAG_BW640G) {
                return 40000;
            }
            return 30000;
        }
        return 0;
    }
    if (CDK_CHIP_CONFIG(unit) & DCFG_40G) {
        if (XLPORT_SUBPORT(port) == 0) {
            return 40000;
        }
        return 0;
    }
    return 10000;
}

int
bcm56840_a0_xlport_pbmp_get(int unit, cdk_pbmp_t *pbmp)
{
    int port;

    CDK_XGS_BLKTYPE_PBMP_GET(unit, BLKTYPE_XLPORT, pbmp);
    CDK_PBMP_ITER(*pbmp, port) {
        if (BMD_PORT_PROPERTIES(unit, port) == 0) {
            CDK_PBMP_PORT_REMOVE(*pbmp, port);
        }
    }
    return 0;
}

int
bcm56840_a0_has_qgport(int unit)
{
    int has_qgport = 1;
    int port;

    for (port = 1; port <= 4; port++) {
        if (bcm56840_a0_port_speed_max(unit, port) > 1000) {
            has_qgport = 0;
        }
    }
    return has_qgport;
}

/*
 * Physical->logical port map captured from the working Cumulus Linux
 * baseline on this exact AS5610-52X board (ING_PHYSICAL_TO_LOGICAL_PORT_
 * NUMBER_MAPPING_TABLE, 2026-05-13 dump).  Index = physical port, value =
 * logical port; 0x7f = unmapped.  This REPLACES OpenMDK's default
 * contiguous-by-physical-order fallback, which produced a different
 * logical numbering (e.g. swp2/phys66 -> logical 58 instead of 2) and so
 * disagreed with both edged.ports[].logical_port (i+1) and the Cumulus
 * tables replicated verbatim by cumulus_replicate.c.  Note the non-trivial
 * lane swizzle at phys 17-24 -- a contiguous fallback can never reproduce it.
 *   phys 65-72 -> swp1-8 (logical 1-8); phys 5-44 -> swp9-48 (logical 9-48);
 *   QSFP phys 49/45/61/57 -> swp49/50/51/52; phys 0 -> CPU; phys 73 -> LB(65).
 */
#define P2L_UNMAPPED 0x7f
static const unsigned char bcm56840_a0_cumulus_p2l[NUM_PHYS_PORTS] = {
    /* 0*/   0, 0x7f, 0x7f, 0x7f, 0x7f,    9,   10,   11,   12,   13,
    /*10*/  14,   15,   16,   17,   18,   19,   20,   22,   21,   24,
    /*20*/  23,   26,   25,   28,   27,   29,   30,   31,   32,   33,
    /*30*/  34,   35,   36,   37,   38,   39,   40,   41,   42,   43,
    /*40*/  44,   45,   46,   47,   48,   50, 0x7f, 0x7f, 0x7f,   49,
    /*50*/0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,   52, 0x7f, 0x7f,
    /*60*/0x7f,   51, 0x7f, 0x7f, 0x7f,    1,    2,    3,    4,    5,
    /*70*/   6,    7,    8,   65,
};

int
bcm56840_a0_p2l(int unit, int port, int inverse)
{
    int pp;

    /* Fixed mappings */
    if (port == CMIC_PORT) {
        return CMIC_LPORT;
    }
    if (port == LB_PORT) {
        return LB_LPORT;
    }
    if (inverse && port == LB_LPORT) {
        return LB_PORT;
    }

    /*
     * Use the captured Cumulus map.  This is authoritative for this board;
     * it keeps ING_PHYS_TO_LOGIC_MAPm / EGR_LOGIC_TO_PHYS_MAPr / the VLAN/L2
     * port bitmaps / and cumulus_replicate.c all in one logical numbering.
     */
    if (inverse) {
        /* logical -> physical */
        for (pp = 0; pp < NUM_PHYS_PORTS; pp++) {
            if (bcm56840_a0_cumulus_p2l[pp] == port) {
                return pp;
            }
        }
        return -1;
    }
    /* physical -> logical */
    if (port >= 0 && port < NUM_PHYS_PORTS) {
        int lp = bcm56840_a0_cumulus_p2l[port];
        if (lp != P2L_UNMAPPED) {
            return lp;
        }
    }
    return -1;
}

int
bcm56840_a0_p2m(int unit, int port, int inverse)
{
    int pp;

    /* Fixed mappings */
    if (port == CMIC_PORT) {
        return CMIC_MPORT;
    }
    if (port == LB_PORT) {
        return LB_MPORT;
    }
    if (inverse && port == LB_MPORT) {
        return LB_PORT;
    }

    if (inverse) {
        for (pp = 0; pp < NUM_PHYS_PORTS; pp++) {
            if (port == _mmu_port_map[unit].map[pp]) {
                return pp;
            }
        }
        return -1;
    }
    return _mmu_port_map[unit].map[port];
}


uint32_t
bcm56840_a0_port_speed_max(int unit, int port)
{
    uint32_t speed_max;

    speed_max = _port_speed_max(unit, port);

    /*
     * The configured maximum speed for a port also determines the
     * number of lanes assigned to the port based on the following
     * rule:
     *
     *   if (config_speed > 20000) then port is a 4-lane port
     *   else if (config_speed > 10000) then port is a 2-lane port
     *   else if (config_speed > 0) then port is a 1-lane port
     *   else port is not used
     *
     * The rule permits the following configurations:
     *
     *   config_speed           port configuration
     *   -----------------------------------------------
     *        1000            1-lane, 1 Gbps maxmimum
     *        2500            1-lane, 2.5 Gbps maxmimum
     *       10000            1-lane, 10 Gbps maxmimum
     *       10001            2-lane, 10 Gbps maxmimum
     *       20000            2-lane, 20 Gbps maxmimum
     *       20001            4-lane, 20 Gbps maxmimum
     *       30000            4-lane, 30 Gbps maxmimum
     *       40000            4-lane, 40 Gbps maxmimum
     */

    /* Round odd configuration speeds (10001 and 20001) to valid speeds */
    return (speed_max  / 10) * 10;
}

uint32_t
bcm56840_a0_port_num_lanes(int unit, int port)
{
    uint32_t speed_max;

    /* Treat CMIC port as high-speed port */
    if (port == CMIC_PORT) {
        return 4;
    }

    speed_max = _port_speed_max(unit, port);

    if (speed_max > 20000) {
        return 4;
    }
    if (speed_max > 10000) {
        return 2;
    }
    if (speed_max > 0) {
        return 1;
    }
    return 0;
}

int
bcm56840_a0_bmd_attach(int unit)
{
    int rv;
    int port;
    int port_mode;
    uint32_t speed_max;
    cdk_pbmp_t pbmp;
#if BMD_CONFIG_INCLUDE_PHY == 1
    phy_bus_t **phy_bus = bcm56840_phy_bus;
#endif

    if(!CDK_DEV_EXISTS(unit)) {
        return CDK_E_UNIT;
    }

#ifdef VENDOR_BROADCOM
    if (CDK_CHIP_CONFIG(unit) & DCFG_PLL_MOD) {
        CDK_XGS_FLAGS(unit) |= CHIP_FLAG_BW640G;
    }
#endif

    CDK_XGS_BLKTYPE_PBMP_GET(unit, BLKTYPE_XLPORT, &pbmp);

    CDK_PBMP_ITER(pbmp, port) {
        CDK_ASSERT(port < BMD_CONFIG_MAX_PORTS);
        speed_max = bcm56840_a0_port_speed_max(unit, port);
        if (speed_max == 0) {
            continue;
        }
        if (speed_max <= 2500) {
            BMD_PORT_PROPERTIES(unit, port) = BMD_PORT_GE;
        } else {
            BMD_PORT_PROPERTIES(unit, port) = BMD_PORT_XE;
            port_mode = CDK_PORT_CONFIG_PORT_MODE(unit, port);
            if (port_mode == CDK_DCFG_PORT_MODE_HIGIG ||
                port_mode == CDK_DCFG_PORT_MODE_HIGIG2) {
                BMD_PORT_PROPERTIES(unit, port) = BMD_PORT_HG;
            }
            if (CDK_CHIP_CONFIG(unit) & DCFG_40G) {
                if (XLPORT_SUBPORT(port) != 0) {
                    BMD_PORT_PROPERTIES(unit, port) = 0;
                }
            }
        }
        PHY_BUS_SET(unit, port, phy_bus);
    }

    /* Sanity check of port configuration */
    rv = _check_port_config(unit);
    if (CDK_FAILURE(rv)) {
        return rv;
    }

    /* Initialize MMU port mappings */
    _init_mmu_port_map(unit);

#if CDK_CONFIG_INCLUDE_PORT_MAP == 1
    /* Match default API port map to configured logical ports */
    _init_port_map(unit);
#endif

    port = CMIC_PORT;
    CDK_ASSERT(port < BMD_CONFIG_MAX_PORTS);
    BMD_PORT_PROPERTIES(unit, port) = BMD_PORT_CPU;

    /* Initialize debug functions */
    BMD_PORT_SPEED_MAX(unit) = bcm56840_a0_port_speed_max;
    BMD_PORT_P2L(unit) = bcm56840_a0_p2l;
    BMD_PORT_P2M(unit) = bcm56840_a0_p2m;

    BMD_DEV_FLAGS(unit) |= BMD_DEV_ATTACHED;

    return 0; 
}
#endif /* CDK_CONFIG_INCLUDE_BCM56840_A0 */
