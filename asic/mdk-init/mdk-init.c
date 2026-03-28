/*
 * mdk-init.c - BCM56846 ASIC initialization using OpenMDK
 *
 * Uses /dev/mem mmap to access CMIC registers directly from userspace.
 * This is the official Broadcom-supported approach from OpenMDK/examples/linux-user.
 *
 * Usage: mdk-init [-v] [-p]
 *   -v  Verbose output
 *   -p  Print port status and exit (don't stay resident)
 *
 * What it does:
 *   1. mmap BCM56846 PCIe BAR0 (0xa0000000)
 *   2. cdk_dev_create() - register device with CDK
 *   3. bmd_attach() - attach BMD driver
 *   4. bmd_init() - full ASIC init (reset, port blocks, SerDes)
 *   5. bmd_switching_init() - L2 tables, default VLAN, MAC learning
 *   6. Set port modes and check link status
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * Based on OpenMDK examples (Broadcom Switch APIs License).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <signal.h>

/* CDK headers */
#include <cdk_config.h>
#include <cdk/cdk_device.h>
#include <cdk/cdk_error.h>
#include <cdk/cdk_debug.h>
#include <cdk/cdk_string.h>
#include <cdk/chip/bcm56840_a0_defs.h>
#include <cdk/arch/xgs_miim.h>

/* BMD headers */
#include <bmd_config.h>
#include <bmd/bmd.h>
#include <bmd/bmd_phy_ctrl.h>

/* PHY headers */
#include <phy_config.h>
#include <phy/phy_drvlist.h>
#include <phy/phy_buslist.h>

/*
 * DMA stub implementations for BMD.
 * mdk-init uses /dev/mem mmap (not BDE kernel module) so DMA goes
 * through a simple malloc pool. The BDE kernel module provides the
 * real DMA coherent pool; these stubs allow mdk-init to link and
 * run standalone for testing.
 */
#include <bmd/bmd_dma.h>

static char dma_pool[4 * 1024 * 1024] __attribute__((aligned(4096)));
static size_t dma_offset;

void *_bde_dma_alloc(size_t size, dma_addr_t *baddr)
{
    void *ptr;
    size = (size + 0xf) & ~0xf; /* 16-byte align */
    if (dma_offset + size > sizeof(dma_pool))
        return NULL;
    ptr = dma_pool + dma_offset;
    if (baddr)
        *baddr = (dma_addr_t)(uintptr_t)ptr;
    dma_offset += size;
    return ptr;
}

void _bde_dma_free(void *dvc, size_t size, void *laddr, dma_addr_t baddr)
{
    /* Bump allocator: no-op free */
    (void)dvc; (void)size; (void)laddr; (void)baddr;
}

/* BCM56846 on AS5610-52X */
#define BCM56846_VENDOR_ID  0x14e4
#define BCM56846_DEVICE_ID  0xb846
#define BCM56846_REVISION   0x02
#define BCM56846_BAR0_ADDR  0xa0000000
#define BCM56846_BAR0_SIZE  (256 * 1024)

/*
 * AS5610-52X port configuration derived from Cumulus config.bcm
 * /etc/switchd/config.bcm on the switch.
 *
 * The SVK board config (board_bcm56846_svk.c) has a WRONG port map for
 * the AS5610-52X. The actual PCB routes front-panel SFP+ ports to
 * completely different SerDes lanes than the SVK.
 *
 * Format: { speed_max, flags, mode, sys_port, app_port(=BCM physical port) }
 * app_port = SerDes lane number from Cumulus portmap_N.0=LANE:SPEED
 *
 * 48x SFP+ (10G) + 4x QSFP+ (40G, configured as 40G base port)
 */
static cdk_port_config_t as5610_port_configs[] = {
    /* idx  speed  flags mode sys  app(=SerDes lane) */
    {      0,    0,   0,  -1,  -1  }, /* entry 0: unused */
    /* SFP+ ports 1-8 → SerDes lanes 65-72 */
    {  10000,    0,   0,   1,  65  }, /* front-panel 1  → lane 65 */
    {  10000,    0,   0,   2,  66  }, /* front-panel 2  → lane 66 */
    {  10000,    0,   0,   3,  67  }, /* front-panel 3  → lane 67 */
    {  10000,    0,   0,   4,  68  },
    {  10000,    0,   0,   5,  69  },
    {  10000,    0,   0,   6,  70  },
    {  10000,    0,   0,   7,  71  },
    {  10000,    0,   0,   8,  72  },
    /* SFP+ ports 9-16 → SerDes lanes 5-12 */
    {  10000,    0,   0,   9,   5  },
    {  10000,    0,   0,  10,   6  },
    {  10000,    0,   0,  11,   7  },
    {  10000,    0,   0,  12,   8  },
    {  10000,    0,   0,  13,   9  },
    {  10000,    0,   0,  14,  10  },
    {  10000,    0,   0,  15,  11  },
    {  10000,    0,   0,  16,  12  },
    /* SFP+ ports 17-24 → lanes 13-20 (non-sequential, PCB trace swaps) */
    {  10000,    0,   0,  17,  13  },
    {  10000,    0,   0,  18,  14  },
    {  10000,    0,   0,  19,  15  },
    {  10000,    0,   0,  20,  16  },
    {  10000,    0,   0,  21,  18  }, /* swapped: 21→18, 22→17 */
    {  10000,    0,   0,  22,  17  },
    {  10000,    0,   0,  23,  20  }, /* swapped: 23→20, 24→19 */
    {  10000,    0,   0,  24,  19  },
    /* SFP+ ports 25-32 → lanes 21-28 (non-sequential) */
    {  10000,    0,   0,  25,  22  }, /* swapped: 25→22, 26→21 */
    {  10000,    0,   0,  26,  21  },
    {  10000,    0,   0,  27,  24  }, /* swapped: 27→24, 28→23 */
    {  10000,    0,   0,  28,  23  },
    {  10000,    0,   0,  29,  25  },
    {  10000,    0,   0,  30,  26  },
    {  10000,    0,   0,  31,  27  },
    {  10000,    0,   0,  32,  28  },
    /* SFP+ ports 33-40 → lanes 29-36 */
    {  10000,    0,   0,  33,  29  },
    {  10000,    0,   0,  34,  30  },
    {  10000,    0,   0,  35,  31  },
    {  10000,    0,   0,  36,  32  },
    {  10000,    0,   0,  37,  33  },
    {  10000,    0,   0,  38,  34  },
    {  10000,    0,   0,  39,  35  },
    {  10000,    0,   0,  40,  36  },
    /* SFP+ ports 41-48 → lanes 37-44 */
    {  10000,    0,   0,  41,  37  },
    {  10000,    0,   0,  42,  38  },
    {  10000,    0,   0,  43,  39  },
    {  10000,    0,   0,  44,  40  },
    {  10000,    0,   0,  45,  41  },
    {  10000,    0,   0,  46,  42  },
    {  10000,    0,   0,  47,  43  },
    {  10000,    0,   0,  48,  44  },
    /* QSFP+ ports 49-52 (40G base ports) */
    {  40000,    0,   0,  49,  49  },
    {  40000,    0,   0,  50,  45  },
    {  40000,    0,   0,  51,  61  },
    {  40000,    0,   0,  52,  57  },
};

/*
 * AS5610-52X board-specific PHY bus (from board_bcm56846_svk.c)
 *
 * The BCM56846 uses WARPcore internal SerDes for all SFP+/QSFP+ ports.
 * The internal MIIM bus (phy_bus_bcm56840_miim_int) talks to the WARPcore.
 * The external MIIM bus below is for the SVK board's external PHYs
 * (BCM84834/84740) - not present on AS5610-52X, but included for
 * compatibility with the reference board config.
 */
#if BMD_CONFIG_INCLUDE_PHY == 1

static uint32_t
_ext_phy_addr(int port)
{
    if (port > 60) {
        return (port - 41) + CDK_XGS_MIIM_EBUS(2);
    }
    if (port > 56) {
        return (port - 44) + CDK_XGS_MIIM_EBUS(2);
    }
    if (port > 40) {
        return (port - 40) + CDK_XGS_MIIM_EBUS(2);
    }
    if (port > 24) {
        return (port - 24) + CDK_XGS_MIIM_EBUS(1);
    }
    return port - 8 + CDK_XGS_MIIM_EBUS(0);
}

static int
_ext_read(int unit, uint32_t addr, uint32_t reg, uint32_t *val)
{
    return cdk_xgs_miim_read(unit, addr, reg, val);
}

static int
_ext_write(int unit, uint32_t addr, uint32_t reg, uint32_t val)
{
    return cdk_xgs_miim_write(unit, addr, reg, val);
}

static int
_ext_phy_inst(int port)
{
    return (port - 1) & 3;
}

static phy_bus_t _phy_bus_miim_ext = {
    "as5610_miim_ext",
    _ext_phy_addr,
    _ext_read,
    _ext_write,
    _ext_phy_inst
};

static phy_bus_t *as5610_phy_bus[] = {
#ifdef PHY_BUS_BCM56840_MIIM_INT_INSTALLED
    &phy_bus_bcm56840_miim_int,
#endif
    &_phy_bus_miim_ext,
    NULL
};

/*
 * PHY reset callback.
 *
 * The AS5610-52X does NOT need WARPcore lane remapping when the correct
 * port map from config.bcm is used (lanes 65-72 for ports 1-8, etc.).
 * The SVK board config's 0x1032 remap was for the SVK PCB layout, not
 * the AS5610. With the correct port map, default lane assignment works.
 */
static int
_phy_reset_cb(phy_ctrl_t *pc)
{
    (void)pc;
    return CDK_E_NONE;
}

static int
_phy_init_cb(phy_ctrl_t *pc)
{
    return CDK_E_NONE;
}

#endif /* BMD_CONFIG_INCLUDE_PHY */

/* BMD/PHY sleep function (name set via -DBMD_SYS_USLEEP=_usleep) */
int
_usleep(uint32_t usecs)
{
    return usleep(usecs);
}

static volatile int running = 1;
static int mem_fd = -1;

static void
sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

static uint32_t *
mmap_bar(off_t phys_addr, int size)
{
    uint32_t *va;

    if (mem_fd < 0) {
        mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (mem_fd < 0) {
            perror("open /dev/mem");
            return NULL;
        }
    }

    va = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, phys_addr);
    if (va == MAP_FAILED) {
        perror("mmap BAR0");
        return NULL;
    }

    return va;
}

/*
 * Get bus endian flags for P2020 (big-endian PPC with PCI).
 * SYS_BE_PIO, SYS_BE_PACKET, SYS_BE_OTHER are set via build flags.
 */
static uint32_t
bus_flags(void)
{
    uint32_t flags = CDK_DEV_MBUS_PCI;
#if SYS_BE_PIO == 1
    flags |= CDK_DEV_BE_PIO;
#endif
#if SYS_BE_PACKET == 1
    flags |= CDK_DEV_BE_PACKET;
#endif
#if SYS_BE_OTHER == 1
    flags |= CDK_DEV_BE_OTHER;
#endif
    return flags;
}

static void
print_port_status(int unit)
{
    int port, rv;
    bmd_port_mode_t mode;
    uint32_t flags;
    int link_up = 0, total = 0;

    printf("\nPort Status:\n");
    printf("  %-6s %-12s %-6s\n", "Port", "Mode", "Link");
    printf("  %-6s %-12s %-6s\n", "----", "----", "----");

    for (port = 0; port < 128; port++) {
        rv = bmd_port_mode_get(unit, port, &mode, &flags);
        if (rv < 0)
            continue;

        total++;
        const char *mode_str;
        switch (mode) {
        case bmdPortModeDisabled: mode_str = "disabled"; break;
        case bmdPortMode10fd:     mode_str = "10M"; break;
        case bmdPortMode100fd:    mode_str = "100M"; break;
        case bmdPortMode1000fd:   mode_str = "1G"; break;
        case bmdPortMode10000fd:  mode_str = "10G"; break;
        case bmdPortMode10000XFI: mode_str = "10G-XFI"; break;
        case bmdPortMode10000SFI: mode_str = "10G-SFI"; break;
        case bmdPortMode10000KR:  mode_str = "10G-KR"; break;
        case bmdPortMode40000fd:  mode_str = "40G"; break;
        case bmdPortMode40000KR:  mode_str = "40G-KR"; break;
        case bmdPortMode40000CR:  mode_str = "40G-CR"; break;
        default:                  mode_str = "unknown"; break;
        }

        int is_up = (flags & BMD_PORT_MODE_F_LINK_UP) ? 1 : 0;
        if (is_up) link_up++;

        printf("  %-6d %-12s %-6s\n", port, mode_str,
               is_up ? "UP" : "down");
    }

    printf("\n  %d ports total, %d link up\n", total, link_up);
}

#if BMD_CONFIG_INCLUDE_PHY == 1
/*
 * Dump WARPcore PHY registers for a port via the BMD PHY API.
 * Reads key registers using PHY_CONFIG_GET and direct MIIM reads.
 */
static void
dump_phy_regs(int unit, int port)
{
    phy_ctrl_t *pc = BMD_PORT_PHY_CTRL(unit, port);
    uint32_t val;
    int rv;

    if (!pc) {
        printf("  port %d: no PHY attached\n", port);
        return;
    }

    printf("  port %d: PHY driver=%s inst=%d addr=0x%x\n",
           port,
           (pc->drv && pc->drv->drv_name) ? pc->drv->drv_name : "unknown",
           PHY_CTRL_INST(pc),
           PHY_CTRL_ADDR(pc));

    /* Read speed */
    rv = PHY_SPEED_GET(pc, &val);
    printf("    speed: %s (%u)\n", CDK_SUCCESS(rv) ? "OK" : "FAIL", val);

    /* Read link */
    {
        int link = 0, an_done = 0;
        rv = PHY_LINK_GET(pc, &link, &an_done);
        printf("    link: %s (link=%d an_done=%d)\n",
               CDK_SUCCESS(rv) ? "OK" : "FAIL", link, an_done);
    }

    /* Read line interface */
    rv = PHY_CONFIG_GET(pc, PhyConfig_Mode, &val, NULL);
    printf("    mode: %s (val=0x%x)\n", CDK_SUCCESS(rv) ? "OK" : "FAIL", val);

    /* Try reading some raw registers via the bus */
    if (pc->bus && pc->bus->read) {
        uint32_t addr = PHY_CTRL_ADDR(pc);
        uint32_t regval;

        /* MII Control (reg 0) */
        rv = pc->bus->read(unit, addr, 0x0000, &regval);
        printf("    MII_CTRL (0x0000): %s 0x%04x\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval);

        /* MII Status (reg 1) */
        rv = pc->bus->read(unit, addr, 0x0001, &regval);
        printf("    MII_STAT (0x0001): %s 0x%04x\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval);

        /* XGXSCONTROL (0x8000) */
        rv = pc->bus->read(unit, addr, 0x8000, &regval);
        printf("    XGXSCTRL (0x8000): %s 0x%04x\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval);

        /* XGXSSTATUS1 (0x8001) - link status per lane */
        rv = pc->bus->read(unit, addr, 0x8001, &regval);
        printf("    XGXSSTAT (0x8001): %s 0x%04x\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval);

        /* MISC1 (0x8308) - force speed, PLL mode */
        rv = pc->bus->read(unit, addr, 0x8308, &regval);
        printf("    MISC1    (0x8308): %s 0x%04x\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval);

        /* MISC3 (0x833c) - lane disable */
        rv = pc->bus->read(unit, addr, 0x833c, &regval);
        printf("    MISC3    (0x833c): %s 0x%04x (LANEDISABLE in bits)\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval);

        /* LANECTL2 (0x8372) - loopback */
        rv = pc->bus->read(unit, addr, 0x8372, &regval);
        printf("    LANECTL2 (0x8372): %s 0x%04x\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval);

        /* TXLNSWAP1 - TX lane swap */
        rv = pc->bus->read(unit, addr, 0x8169, &regval);
        printf("    TXLNSWP  (0x8169): %s 0x%04x\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval);

        /* RXLNSWAP1 - RX lane swap */
        rv = pc->bus->read(unit, addr, 0x816b, &regval);
        printf("    RXLNSWP  (0x816b): %s 0x%04x\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval);

        /* TX0_TX_DRIVERr (0x8067) - lane 0 TX driver */
        rv = pc->bus->read(unit, addr, 0x8067, &regval);
        printf("    TX0_DRV  (0x8067): %s 0x%04x [ELECIDLE=%d idrv=%d pdrv=%d]\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval,
               (regval >> 15) & 1, (regval >> 8) & 0xf, (regval >> 4) & 0xf);

        /* TXB_TX_DRIVERr (0x80a7) - broadcast/per-lane TX driver */
        rv = pc->bus->read(unit, addr, 0x80a7, &regval);
        printf("    TXB_DRV  (0x80a7): %s 0x%04x [ELECIDLE=%d idrv=%d pdrv=%d]\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval,
               (regval >> 15) & 1, (regval >> 8) & 0xf, (regval >> 4) & 0xf);

        /* CL45 reads via MIIM_CL45: (devad << 16) | reg */
        /* PMA/PMD Control 1 (1.0) - low power, loopback */
        cdk_xgs_miim_read(unit, addr, (1 << 16) | 0x0000, &regval);
        printf("    CL45 1.0 PMA_CTRL: 0x%04x [lpwr=%d lpbk=%d]\n",
               regval, (regval >> 11) & 1, regval & 1);

        /* PMA/PMD Status 2 (1.8) - tx_fault, rx_fault */
        cdk_xgs_miim_read(unit, addr, (1 << 16) | 0x0008, &regval);
        printf("    CL45 1.8 PMA_ST2:  0x%04x [txflt=%d rxflt=%d lclflt=%d]\n",
               regval, (regval >> 13) & 1, (regval >> 14) & 1, (regval >> 10) & 1);

        /* PMD TX Disable (1.9) - per-lane TX disable */
        cdk_xgs_miim_read(unit, addr, (1 << 16) | 0x0009, &regval);
        printf("    CL45 1.9 TX_DIS:   0x%04x [global=%d ln0=%d ln1=%d ln2=%d ln3=%d]\n",
               regval, regval & 1, (regval >> 1) & 1, (regval >> 2) & 1,
               (regval >> 3) & 1, (regval >> 4) & 1);

        /* PMD Signal Detect (1.10) */
        cdk_xgs_miim_read(unit, addr, (1 << 16) | 0x000A, &regval);
        printf("    CL45 1.A SIG_DET:  0x%04x [global=%d ln0=%d ln1=%d ln2=%d ln3=%d]\n",
               regval, regval & 1, (regval >> 1) & 1, (regval >> 2) & 1,
               (regval >> 3) & 1, (regval >> 4) & 1);

        /* ANATXACONTROL0 (0x8061) - analog TX control 0 */
        rv = pc->bus->read(unit, addr, 0x8061, &regval);
        printf("    ANATX0   (0x8061): %s 0x%04x\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval);

        /* ANATXACONTROL1 (0x8065) - analog TX control 1 (DRIVERMODE, VCM) */
        rv = pc->bus->read(unit, addr, 0x8065, &regval);
        printf("    ANATX1   (0x8065): %s 0x%04x [drv_mode=%d vcm=%d]\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval,
               (regval >> 9) & 1, (regval >> 4) & 3);

        /* ANATXACONTROL2 (0x8066) - analog TX control 2 */
        rv = pc->bus->read(unit, addr, 0x8066, &regval);
        printf("    ANATX2   (0x8066): %s 0x%04x\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval);

        /* FIRMWARE_MODEr (0x81F2) - OpenMDK firmware mode */
        rv = pc->bus->read(unit, addr, 0x81F2, &regval);
        printf("    FW_MODE  (0x81F2): %s 0x%04x\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval);

        /* UC_INFO_B1 at 0x81F7 - SDK firmware mode / GPIO_STS_LN0 */
        rv = pc->bus->read(unit, addr, 0x81F7, &regval);
        printf("    UC_B1_F7 (0x81F7): %s 0x%04x\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval);

        /* UC_CTRLr (0x820E) - firmware handshake */
        rv = pc->bus->read(unit, addr, 0x820E, &regval);
        printf("    UC_CTRL  (0x820E): %s 0x%04x [rdy=%d err=%d cmd=%d supp=%d]\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval,
               (regval >> 7) & 1, (regval >> 6) & 1,
               regval & 0xf, (regval >> 8) & 0xff);

        /* VERSIONr (0x81F0) - firmware version */
        rv = pc->bus->read(unit, addr, 0x81F0, &regval);
        printf("    FW_VER   (0x81F0): %s 0x%04x\n",
               CDK_SUCCESS(rv) ? "" : "ERR", regval);
    }
}
#endif

int
main(int argc, char *argv[])
{
    int unit, rv;
    int verbose = 0;
    int print_only = 0;
    cdk_dev_id_t dev_id;
    cdk_dev_vectors_t dv;
    uint32_t *cmic_base;

    int link_only = 0;
    int miim_dump = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = 1;
        if (strcmp(argv[i], "-p") == 0) print_only = 1;
        if (strcmp(argv[i], "-l") == 0) link_only = 1;
        if (strcmp(argv[i], "-m") == 0) miim_dump = 1;
    }

    printf("mdk-init: BCM56846 ASIC initialization (OpenMDK)\n");

    /* Map CMIC registers */
    printf("  Mapping BAR0 at 0x%x (%d KB)...\n",
           BCM56846_BAR0_ADDR, BCM56846_BAR0_SIZE / 1024);

    cmic_base = mmap_bar(BCM56846_BAR0_ADDR, BCM56846_BAR0_SIZE);
    if (!cmic_base) {
        fprintf(stderr, "ERROR: Cannot map CMIC registers\n");
        return 1;
    }

    /* Read CMIC device ID to verify access */
    uint32_t cmic_devid = cmic_base[0x178 / 4]; /* CMIC_DEV_REV_ID */
    printf("  CMIC device ID register: 0x%08x\n", cmic_devid);

    /* Setup CDK device */
    memset(&dev_id, 0, sizeof(dev_id));
    dev_id.vendor_id = BCM56846_VENDOR_ID;
    dev_id.device_id = BCM56846_DEVICE_ID;
    dev_id.revision = BCM56846_REVISION;

    memset(&dv, 0, sizeof(dv));
    dv.base_addr = cmic_base;

    printf("  Creating CDK device...\n");
    unit = cdk_dev_create(&dev_id, &dv, bus_flags());
    if (unit < 0) {
        fprintf(stderr, "ERROR: cdk_dev_create failed: %s (%d)\n",
                CDK_ERRMSG(unit), unit);
        return 1;
    }
    printf("  CDK unit %d created (vendor=%04x device=%04x rev=%02x)\n",
           unit, dev_id.vendor_id, dev_id.device_id, dev_id.revision);

    /* Set chip config: AS5610-52X uses 156.25 MHz LCPLL reference clock */
    CDK_CHIP_CONFIG_SET(unit, DCFG_LCPLL_156);
    printf("  Chip config set: DCFG_LCPLL_156 (0x%08x)\n", DCFG_LCPLL_156);

    /*
     * Set port configs from AS5610-52X board layout.
     *
     * CDK_PORT_CONFIGS_SET stores the port config array on the device.
     * CDK_PORT_CONFIG_ID_SET maps each BCM physical port to its index
     * in the config array. Without the ID mapping, all ports map to
     * index 0 (speed 0) and bmd_port_mode_get() fails for all ports.
     * This was the bug in our previous attempt at setting port configs.
     */
    {
        int num_configs = sizeof(as5610_port_configs) /
                          sizeof(as5610_port_configs[0]);
        int i;

        CDK_PORT_CONFIGS_SET(unit, as5610_port_configs, num_configs);

        for (i = 1; i < num_configs; i++) {
            int app_port = as5610_port_configs[i].app_port;
            if (app_port >= 0) {
                CDK_PORT_CONFIG_ID_SET(unit, app_port, i);
            }
        }
        printf("  Port configs set: %d ports mapped\n", num_configs - 1);
    }

    /* Initialize PHY probe and register board callbacks */
#if BMD_CONFIG_INCLUDE_PHY == 1
    printf("  Initializing PHY probe...\n");
    bmd_phy_probe_init(bmd_phy_probe_default, bmd_phy_drv_list);

    /*
     * Register board-specific PHY callbacks BEFORE bmd_init().
     *
     * bmd_init() calls bmd_phy_staged_init() which calls bmd_phy_init()
     * per port. bmd_phy_init() calls PHY_RESET, then phy_reset_cb (our
     * lane remap), then PHY_INIT, then phy_init_cb.
     *
     * The reset callback applies WARPcore RX lane remap 0x1032 which
     * is CRITICAL for the AS5610-52X SFP+ cage pin assignment.
     */
    printf("  Registering PHY reset callback (WARPcore lane remap)...\n");
    bmd_phy_reset_cb_register(_phy_reset_cb);
    bmd_phy_init_cb_register(_phy_init_cb);
#endif

    /* Attach BMD */
    printf("  Attaching BMD driver...\n");
    rv = bmd_attach(unit);
    if (rv < 0) {
        fprintf(stderr, "ERROR: bmd_attach failed: %d\n", rv);
        return 1;
    }

    /*
     * Override PHY bus for each active port to use the board-specific
     * bus list (internal WARPcore MIIM + external MIIM).
     *
     * bmd_attach() sets the default bcm56840_phy_bus (internal only).
     * The board config adds an external MIIM bus with AS5610-52X
     * specific address mapping. This must happen after bmd_attach()
     * but before bmd_init() which probes PHYs.
     */
#if BMD_CONFIG_INCLUDE_PHY == 1
    {
        int port;
        bmd_port_mode_t mode;
        uint32_t flags;

        printf("  Setting board-specific PHY bus...\n");
        for (port = 0; port < BMD_CONFIG_MAX_PORTS; port++) {
            if (bmd_port_mode_get(unit, port, &mode, &flags) == 0) {
                bmd_phy_bus_set(unit, port, as5610_phy_bus);
            }
        }
    }
#endif

    if (!link_only) {
        /* Reset ASIC (CPS reset, SBUS ring map, PLL init, port blocks) */
        printf("  Running bmd_reset()...\n");
        rv = bmd_reset(unit);
        if (rv < 0) {
            fprintf(stderr, "ERROR: bmd_reset failed: %d\n", rv);
            return 1;
        }
        printf("  ASIC reset complete\n");

        /* Initialize ASIC (soft reset IP/EP/MMU, configure port blocks) */
        printf("  Running bmd_init()...\n");
        rv = bmd_init(unit);
        if (rv < 0) {
            fprintf(stderr, "ERROR: bmd_init failed: %d\n", rv);
            return 1;
        }
        printf("  ASIC initialized\n");

        /* Initialize L2 switching */
        printf("  Running bmd_switching_init()...\n");
        rv = bmd_switching_init(unit);
        if (rv < 0) {
            fprintf(stderr, "ERROR: bmd_switching_init failed: %d\n", rv);
            return 1;
        }
        printf("  L2 switching initialized (default VLAN 1, MAC learning ON)\n");

        /* Set port modes - 10G for SFP+ ports */
        printf("  Configuring ports...\n");
        {
            int port;
            bmd_port_mode_t mode;
            uint32_t flags;
            int configured = 0;

            for (port = 0; port < 128; port++) {
                rv = bmd_port_mode_get(unit, port, &mode, &flags);
                if (rv < 0)
                    continue;

                rv = bmd_port_mode_set(unit, port, bmdPortMode10000fd, 0);
                if (rv == 0) {
                    configured++;
                } else {
                    rv = bmd_port_mode_set(unit, port, bmdPortMode1000fd, 0);
                    if (rv == 0)
                        configured++;
                }
            }
            printf("  %d ports configured\n", configured);
        }

        /*
         * Clear WARPcore TX electrical idle and set TX driver current.
         *
         * The OpenMDK WARPcore PHY driver does NOT configure the SerDes
         * TX analog driver. The full Memory SDK (wc40.c) calls
         * _phy_wc40_tx_control_set() and _phy_wc40_tx_disable() which:
         *   1. Write TX driver current (idriver=0x9, ipredrive=0x9)
         *   2. Clear TX electrical idle (ELECIDLE bit in TX_DRIVERr)
         *
         * Without this, the WARPcore SerDes TX is in electrical idle
         * and does not output any signal, even though the digital side
         * (PCS, XMAC) is fully configured.
         *
         * TX_DRIVERr register layout (0x8067 + 0x10*lane):
         *   bits [15]   = ELECIDLE (1=idle/off, 0=active)
         *   bits [14:12] = post2 (post-cursor tap 2)
         *   bits [11:8]  = idriver (main TX driver current)
         *   bits [7:4]   = ipredrive (pre-driver current)
         *   bits [3:0]   = preemph (pre-emphasis)
         *
         * We set: ELECIDLE=0, idriver=0x9, ipredrive=0x9, rest=0
         * Value: 0x0990
         */
        /*
         * Configure WARPcore TX driver via PHY config API and direct
         * MIIM through the CDK xgs_miim functions.
         *
         * The OpenMDK WARPcore PHY driver does NOT configure the SerDes
         * TX analog driver. The full Memory SDK (wc40.c) sets:
         *   - TX driver current (idriver=0x9, ipredrive=0x9)
         *   - Clears TX electrical idle (ELECIDLE bit in TX_DRIVERr)
         *
         * TX_DRIVERr is at WARPcore register 0x8067 + 0x10*lane.
         * Access via CL45: DEVAD=1 (PMA/PMD), register offset.
         */
#if BMD_CONFIG_INCLUDE_PHY == 1
        printf("  Configuring WARPcore TX drivers...\n");
        {
            int port;

            for (port = 0; port < BMD_CONFIG_MAX_PORTS; port++) {
                phy_ctrl_t *pc = BMD_PORT_PHY_CTRL(unit, port);
                if (!pc)
                    continue;

                /* Set TX pre-emphasis via PHY config API (goes through driver) */
                PHY_CONFIG_SET(pc, PhyConfig_TxPreemp, 0x0990, NULL);
            }
        }

        /* Set TX driver current via PHY config API (per-lane, AER-aware) */
        printf("  Setting TX driver via PhyConfig API...\n");
        {
            int port;

            for (port = 0; port < BMD_CONFIG_MAX_PORTS; port++) {
                phy_ctrl_t *pc = BMD_PORT_PHY_CTRL(unit, port);
                if (!pc)
                    continue;

                /* PhyConfig_TxIDrv and PhyConfig_TxPreIDrv go through
                 * the WC driver's config_set handler which writes to
                 * TXB_TX_DRIVERr using AER for the current lane */
                PHY_CONFIG_SET(pc, PhyConfig_TxIDrv, 0x9, NULL);
                PHY_CONFIG_SET(pc, PhyConfig_TxPreIDrv, 0x9, NULL);
            }
        }
#endif
    } else {
        printf("  Link-check mode: skipping ASIC reset/init\n");
    }

    /* Update port link status (triggers PHY link check + MAC enable) */
    printf("  Updating link status...\n");
    {
        int port;
        int links = 0;
        for (port = 0; port < 128; port++) {
            rv = bmd_port_mode_update(unit, port);
            if (rv == 0) {
                bmd_port_mode_t mode;
                uint32_t flags;
                if (bmd_port_mode_get(unit, port, &mode, &flags) == 0) {
                    if (flags & BMD_PORT_MODE_F_LINK_UP)
                        links++;
                }
            }
        }
        printf("  %d links up\n", links);
    }

        /*
         * Replay Cumulus CL22 TX init sequence (from GDB capture).
         *
         * The Memory SDK writes TX amplitude via CL22 extended registers,
         * NOT via the CL45/WC-internal TX_DRIVERr (0x8067). The key write
         * is page 0, reg 0x18 = 0x8370 (TX drive amplitude).
         *
         * Also clear CL45 PMD TX Disable (1.9) in case it's set.
         */
#if BMD_CONFIG_INCLUDE_PHY == 1
        printf("  Applying TX fixes (amplitude + fault clear)...\n");
        {
            int port;

            for (port = 0; port < BMD_CONFIG_MAX_PORTS; port++) {
                phy_ctrl_t *pc = BMD_PORT_PHY_CTRL(unit, port);
                if (!pc)
                    continue;

                /* Only primary lane per WARPcore */
                if ((PHY_CTRL_INST(pc) & 0x3) != 0)
                    continue;

                uint32_t addr = PHY_CTRL_ADDR(pc);

                /* CL22: TX amplitude (Cumulus GDB capture) */
                cdk_xgs_miim_write(unit, addr, 0x1f, 0x0000);
                cdk_xgs_miim_write(unit, addr, 0x17, 0x8010);
                cdk_xgs_miim_write(unit, addr, 0x18, 0x8370);
                cdk_xgs_miim_write(unit, addr, 0x18, 0x8370);

                /* CL45: Clear PMD TX Disable (1.9) */
                cdk_xgs_miim_write(unit, addr,
                    (1 << 16) | 0x0009, 0x0000);

                /* NOTE: PMA reset (1.0 bit 15) was tried but doesn't clear
                 * TX fault — the fault is caused by the underlying TX driver
                 * configuration issue, not a transient condition. */

                /* CL22: Restore page 0 */
                cdk_xgs_miim_write(unit, addr, 0x1f, 0x0000);
            }
        }
#endif

#if BMD_CONFIG_INCLUDE_PHY == 1
    if (miim_dump) {
        printf("\nPHY Register Dump (ports 65-68, SFP 1-4):\n");
        dump_phy_regs(unit, 65);
        dump_phy_regs(unit, 66);
        dump_phy_regs(unit, 67);
        dump_phy_regs(unit, 68);
        printf("\nPHY Register Dump (ports 5-8, SFP 9-12):\n");
        dump_phy_regs(unit, 5);
        dump_phy_regs(unit, 6);
        dump_phy_regs(unit, 7);
        dump_phy_regs(unit, 8);
    }
#endif

    print_port_status(unit);

    printf("\nmdk-init: ASIC ready. L2 switching active.\n");

    if (print_only)
        return 0;

    /* Stay resident to keep mmap alive */
    printf("Staying resident (Ctrl-C to exit)...\n");
    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);
    while (running)
        sleep(1);

    printf("mdk-init: exiting\n");
    munmap(cmic_base, BCM56846_BAR0_SIZE);
    if (mem_fd >= 0)
        close(mem_fd);

    return 0;
}
