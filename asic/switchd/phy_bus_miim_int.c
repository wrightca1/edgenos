/*
 * phy_bus_miim_int.c - Custom internal MIIM bus for BCM56846
 *
 * The default OpenMDK xgs_miim_int bus uses port + CDK_XGS_MIIM_INTERNAL
 * (0x80) which puts ports < 64 on the wrong MIIM bus (bus 0 instead of
 * bus 1). The BCM56840/56846 maps ports to MIIM buses as:
 *
 *   Ports  0-23: CDK_XGS_MIIM_IBUS(0) = internal bus 0
 *   Ports 24-47: CDK_XGS_MIIM_IBUS(1) = internal bus 1 (offset -24)
 *   Ports 48+:   CDK_XGS_MIIM_IBUS(2) = internal bus 2 (offset -48)
 *
 * This matches bcm56840_a0_bmd_reset.c:_phy_addr_get() and the Cumulus
 * SDK 6.3.8 MIIM encoding verified via GDB capture (March 2026).
 *
 * Without this fix, Warpcore registers beyond block 0x8000 (MISC1r,
 * MISC3r, etc.) are inaccessible because MIIM writes go to the wrong
 * bus and the block select reaches a phantom device.
 */

#include <cdk_config.h>

#ifdef CDK_CONFIG_ARCH_XGS_INSTALLED

#include <cdk/cdk_device.h>
#include <cdk/arch/xgs_miim.h>
#include <phy/phy.h>

static uint32_t
_phy_addr(int port)
{
    if (port >= 48) {
        return (port - 48) + CDK_XGS_MIIM_IBUS(2);
    } else if (port >= 24) {
        return (port - 24) + CDK_XGS_MIIM_IBUS(1);
    }
    return port + CDK_XGS_MIIM_IBUS(0);
}

static int
_read(int unit, uint32_t addr, uint32_t reg, uint32_t *val)
{
    return cdk_xgs_miim_read(unit, addr, reg, val);
}

static int
_write(int unit, uint32_t addr, uint32_t reg, uint32_t val)
{
    return cdk_xgs_miim_write(unit, addr, reg, val);
}

phy_bus_t phy_bus_bcm56846_miim_int = {
    "bcm56846_miim_int",
    _phy_addr,
    _read,
    _write
};

#else
int bcm56846_miim_int_not_empty;
#endif
