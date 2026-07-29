/*
 * fm6000_i2c_bringup.c - deterministic FM6000 pre-enum bring-up over the mgmt i2c slave.
 *
 * Replaces the slow/flaky shell+i2ctransfer path (fm6000-pcie-bist-init.sh) with a
 * single compiled tool that drives the FM6000 management I2C slave ("master 0 bus 2",
 * addr 0x40) via raw I2C_RDWR ioctls, back-to-back with correct ordering. Does the
 * whole pre-enumeration sequence recovered from fm6000BootSwitch/PrebootSwitch:
 *
 *   wait SPI-ROM boot  ->  COLD-BIST memory init (makes table RAM parity-valid; the
 *   thing whose absence wedges MCAST/MOD/L2F writes)  ->  normal operating mode  ->
 *   PCIe SerDes bring-up  ->  PCI rescan + verify enumeration.
 *
 * BIST values: libFocalpointSDK.so:fm6000BistMemoryInit (HW-validated vs the EOS
 * capture; see arista notes/analysis/phase40 + phase41). Chip must already be clocked
 * (Cotati Si5338) and out of reset, and SCD accel#0 registered so i2c-10 exists.
 *
 * After this returns "ENUMERATED", drive the rest over PCIe (fm6000reg/fm6000load):
 * boot-ctrl, SOFT_RESET release, microcode, GLORT->L2F config, then fpdma inject.
 *
 * Usage: fm6000_i2c_bringup [-b <busnum>] [-v] [-n]     (-n = skip PCI rescan)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define SLAVE 0x40u

static int g_fd = -1;
static int g_verbose = 0;

/* ---- raw i2c register access (word-addressed, 24-bit addr / 32-bit big-endian data) */
static int i2c_wr(uint32_t a, uint32_t v)
{
    uint8_t buf[7] = { a >> 16, a >> 8, a, v >> 24, v >> 16, v >> 8, v };
    struct i2c_msg m = { .addr = SLAVE, .flags = 0, .len = 7, .buf = buf };
    struct i2c_rdwr_ioctl_data io = { .msgs = &m, .nmsgs = 1 };
    if (g_verbose)
        fprintf(stderr, "  wr 0x%05x <= 0x%08x\n", a, v);
    if (ioctl(g_fd, I2C_RDWR, &io) < 0) {
        fprintf(stderr, "  wr 0x%05x FAILED: %s\n", a, strerror(errno));
        return -1;
    }
    return 0;
}

static uint32_t i2c_rd(uint32_t a)
{
    uint8_t ab[3] = { a >> 16, a >> 8, a }, vb[4] = { 0 };
    struct i2c_msg m[2] = {
        { .addr = SLAVE, .flags = 0,        .len = 3, .buf = ab },
        { .addr = SLAVE, .flags = I2C_M_RD, .len = 4, .buf = vb },
    };
    struct i2c_rdwr_ioctl_data io = { .msgs = m, .nmsgs = 2 };
    if (ioctl(g_fd, I2C_RDWR, &io) < 0)
        return 0xFFFFFFFFu;
    return ((uint32_t)vb[0] << 24) | (vb[1] << 16) | (vb[2] << 8) | vb[3];
}

/* Convenience: write a constant to a base + stride*count run. */
static int wr_run(uint32_t base, uint32_t stride, int count, uint32_t val)
{
    int i;
    for (i = 0; i < count; i++)
        if (i2c_wr(base + (uint32_t)i * stride, val) < 0)
            return -1;
    return 0;
}

/* ---- locate the FM6000 mgmt slave bus (SCD "master 0 bus 2") ------------- */
static int find_bus(void)
{
    DIR *d = opendir("/sys/class/i2c-dev");
    struct dirent *e;
    int bus = -1;
    if (!d)
        return -1;
    while ((e = readdir(d))) {
        char p[320], name[128] = { 0 };
        int f;
        if (strncmp(e->d_name, "i2c-", 4))
            continue;
        snprintf(p, sizeof p, "/sys/class/i2c-dev/%s/name", e->d_name);
        f = open(p, O_RDONLY);
        if (f < 0)
            continue;
        if (read(f, name, sizeof name - 1) > 0 && strstr(name, "master 0 bus 2"))
            bus = atoi(e->d_name + 4);
        close(f);
        if (bus >= 0)
            break;
    }
    closedir(d);
    return bus;
}

/* ---- cold-BIST memory init (fm6000BistMemoryInit) ----------------------- */
static int bist(void)
{
    fprintf(stderr, "[i2c-bringup] BIST: scan/PLL setup\n");
    i2c_wr(0x1C022, 0x0);                       /* BOOT_CTRL clear            */
    i2c_wr(0x1C03A, 0x00000063); usleep(700);
    (void)i2c_rd(0x1C03C);                      /* SDK dummy read             */
    i2c_wr(0x1C03A, 0x80000063); usleep(700);
    i2c_wr(0x1C03A, 0x88D55555); usleep(700);
    i2c_wr(0x1C03A, 0x88009555); usleep(1700);

    fprintf(stderr, "[i2c-bringup] BIST: gate BM_ENGINE_STATUS(0x1D08E)=0x%08x\n", i2c_rd(0x1D08E));

    fprintf(stderr, "[i2c-bringup] BIST: BM march table (0x1D080 / mirror 0x1D708)\n");
    static const uint32_t march[4] = { 0x6529EDA9, 0x9B8ED9B1, 0xEFCA952B, 0x000FCA99 };
    for (int w = 0; w < 4; w++) { i2c_wr(0x1D080 + w, march[w]); i2c_wr(0x1D708 + w, march[w]); }

    fprintf(stderr, "[i2c-bringup] BIST: per-partition config (golden-EOS match, phase64)\n");
    /* CORRECTED to exactly match golden EOS BIST march config (arista phase64,
     * golden-bist-march-config-2026-07-28.txt). The old partial config left the
     * MCAST_MID (0x240000) and MOD (0x150000) RAM ECC uninitialized -> those
     * blocks hang on any access (the CPU-punt RX blocker). The missing pieces were
     * the SPDP per-partition +0x0C upper-address bounds and +0x02 per-memory config,
     * the full CDP inst0/inst1 depth config (0x1D2A1 must be 0x7F, not 4), and the
     * SPDP march-seq mirror at 0x1D688. Removed the old erroneous writes
     * (0x1D220=3, 0x1D48B/50B/58B=2, and the extra CDP instances 0x1D310/390) that
     * golden leaves at 0. */
    /* base enables (bit21 = Enabled) */
    i2c_wr(0x1D210, 0x00200000); i2c_wr(0x1D290, 0x00200000);          /* CDP inst 0,1 */
    i2c_wr(0x1D400, 0x00200000); i2c_wr(0x1D480, 0x00200000);
    i2c_wr(0x1D500, 0x00200000); i2c_wr(0x1D580, 0x00200000);
    i2c_wr(0x1D600, 0x00200000);                                       /* SPDP 0..4 */
    /* CDP inst0 */
    i2c_wr(0x1D218, 0x000000B4); i2c_wr(0x1D219, 0x00000100);
    i2c_wr(0x1D221, 0x0000017F); i2c_wr(0x1D222, 0x0002BFF7); i2c_wr(0x1D223, 0x0003BFE3);
    i2c_wr(0x1D224, 0x20000000); i2c_wr(0x1D225, 0x000203FF);
    i2c_wr(0x1D241, 0x4); i2c_wr(0x1D242, 0x300);
    i2c_wr(0x1D261, 0x4); i2c_wr(0x1D262, 0x300);
    i2c_wr(0x1D281, 0x4);
    /* CDP inst1 */
    i2c_wr(0x1D298, 0x000000B4); i2c_wr(0x1D299, 0x00000100);
    i2c_wr(0x1D2A1, 0x0000007F); i2c_wr(0x1D2A2, 0x00023FF7); i2c_wr(0x1D2A3, 0x00033FE3);
    i2c_wr(0x1D2A4, 0x20000000); i2c_wr(0x1D2A5, 0x000203FF);
    i2c_wr(0x1D2C1, 0x4); i2c_wr(0x1D2C2, 0x300);
    /* SPDP partition 0 (0x1D400) */
    i2c_wr(0x1D404, 0xC); i2c_wr(0x1D409, 0x00000FFF); i2c_wr(0x1D40C, 0x00000FFF);
    i2c_wr(0x1D440, 1); i2c_wr(0x1D441, 4); i2c_wr(0x1D442, 0x300);
    /* SPDP partition 1 (0x1D480) — two memories (+0x40, +0x60) */
    i2c_wr(0x1D484, 0xC); i2c_wr(0x1D489, 0x00007FFF); i2c_wr(0x1D48C, 0x00007FFF);
    i2c_wr(0x1D4C0, 1); i2c_wr(0x1D4C1, 4); i2c_wr(0x1D4C2, 0x3F8);
    i2c_wr(0x1D4E0, 1); i2c_wr(0x1D4E1, 4); i2c_wr(0x1D4E2, 0x380);
    /* SPDP partition 2 (0x1D500) */
    i2c_wr(0x1D504, 0xC); i2c_wr(0x1D509, 0x00003FFF); i2c_wr(0x1D50C, 0x00003FFF);
    i2c_wr(0x1D540, 1); i2c_wr(0x1D541, 4); i2c_wr(0x1D542, 0x300);
    /* SPDP partition 3 (0x1D580) — two memories */
    i2c_wr(0x1D584, 0xC); i2c_wr(0x1D589, 0x00000FFF); i2c_wr(0x1D58C, 0x00000FFF);
    i2c_wr(0x1D5C0, 1); i2c_wr(0x1D5C1, 6); i2c_wr(0x1D5C2, 0x3A0);
    i2c_wr(0x1D5E0, 1); i2c_wr(0x1D5E1, 6); i2c_wr(0x1D5E2, 0x3FA);
    /* SPDP partition 4 (0x1D600) — two memories */
    i2c_wr(0x1D604, 0x4); i2c_wr(0x1D609, 0x000003FF); i2c_wr(0x1D60C, 0x000003FF);
    i2c_wr(0x1D640, 1); i2c_wr(0x1D641, 0xA); i2c_wr(0x1D642, 0x300);
    i2c_wr(0x1D660, 1); i2c_wr(0x1D661, 0xA); i2c_wr(0x1D662, 0x3E0);
    /* SPDP march-seq mirror + config */
    i2c_wr(0x1D688, 0x6529EDA9); i2c_wr(0x1D689, 0x9B8ED9B1);
    i2c_wr(0x1D68A, 0xEFCA952B); i2c_wr(0x1D68B, 0x000FCA99); i2c_wr(0x1D68C, 0x00000200);
    /* march START triggers — write-and-run, SELF-CLEARING (they read 0 in a post-march
     * snapshot, like the scheduler INIT strobes), so they MUST be replayed AFTER all
     * config or the march never fires (symptom: "BIST idle after 0s", RAM ECC stays
     * uninitialized -> MCAST_MID/MOD hang). CDP 2 inst + SPDP 5 partitions. */
    i2c_wr(0x1D220, 0x3); i2c_wr(0x1D2A0, 0x3);
    i2c_wr(0x1D40B, 0x0); i2c_wr(0x1D48B, 0x2); i2c_wr(0x1D50B, 0x2);
    i2c_wr(0x1D58B, 0x2); i2c_wr(0x1D60B, 0x0);

    fprintf(stderr, "[i2c-bringup] BIST: march running; poll BM_ENGINE_STATUS(0x1D08E)==0\n");
    for (int i = 0; i < 20; i++) {
        uint32_t st = i2c_rd(0x1D08E);
        if (st == 0) { fprintf(stderr, "[i2c-bringup] BIST idle after %ds\n", i); break; }
        usleep(300000);
    }
    fprintf(stderr, "[i2c-bringup] BIST done: BM_STATUS=0x%08x result(0x1D70E)=0x%08x\n",
            i2c_rd(0x1D08E), i2c_rd(0x1D70E));
    return 0;
}

/* ---- normal operating mode + PCIe SerDes (fm6000SetupPCIe, proven values) */
static int pcie(void)
{
    fprintf(stderr, "[i2c-bringup] normal operating mode\n");
    i2c_wr(0x1C03A, 0x88800000); usleep(1000);
    i2c_wr(0x1C03A, 0x88008000); usleep(1000);
    i2c_wr(0x1C03A, 0x80000040);
    i2c_wr(0x1C03B, 0xFFFFFFFF);
    i2c_wr(0x1C045, 0x3); usleep(1000);
    fprintf(stderr, "[i2c-bringup] fmPlatformSetupPCIe (SerDes lanes on)\n");
    usleep(1000);
    i2c_wr(0x00009, 0x17);          /* release JSS (keep PCIe)                */
    i2c_wr(0x0F000, 0x0); usleep(1000);
    i2c_wr(0x00004, 0x1);
    i2c_wr(0x00009, 0x16);          /* release PCIe                           */
    i2c_wr(0x0F002, 0x4);
    i2c_wr(0x0F001, 0x0);
    i2c_wr(0x0F001, 0x121FE0A);     /* SBus command execute                   */
    i2c_wr(0x01400, 0x0);
    i2c_wr(0x01002, 0x2000000);
    i2c_wr(0x01418, 0x35);
    i2c_wr(0x0140C, 0xFFFFFFFF);
    i2c_wr(0x0140D, 0xFFFFFFFF);
    i2c_wr(0x01435, 0xF121F34);     /* PCI_SERDES_CTRL_1: TxOutputEn + RefSel  */
    i2c_wr(0x1C002, 0x3FFF);
    i2c_wr(0x0141D, 0x1C01F);       /* PCI_CORE_CTRL_1 (core enable bit16)     */
    usleep(2000000);
    return 0;
}

int main(int argc, char **argv)
{
    int bus = -1, do_rescan = 1, c;
    char dev[32];

    while ((c = getopt(argc, argv, "b:vn")) != -1) {
        if (c == 'b') bus = atoi(optarg);
        else if (c == 'v') g_verbose = 1;
        else if (c == 'n') do_rescan = 0;
    }

    if (bus < 0) bus = find_bus();
    if (bus < 0) { fprintf(stderr, "[i2c-bringup] FM6000 slave 'master 0 bus 2' not found "
                                   "(clocked? accel#0 registered?)\n"); return 1; }
    snprintf(dev, sizeof dev, "/dev/i2c-%d", bus);
    g_fd = open(dev, O_RDWR);
    if (g_fd < 0) { fprintf(stderr, "[i2c-bringup] open %s: %s\n", dev, strerror(errno)); return 1; }
    fprintf(stderr, "[i2c-bringup] FM6000 mgmt slave on %s addr 0x%02x\n", dev, SLAVE);

    /* wait SPI-ROM boot (BOOT_CTRL 0x1C022 bit5 = EepromLoadDone) */
    int n; uint32_t bc = 0;
    for (n = 0; n < 20; n++) { bc = i2c_rd(0x1C022); if (bc & 0x20) break; usleep(1000000); }
    fprintf(stderr, "[i2c-bringup] boot after %ds (BOOT_CTRL=0x%08x)\n", n, bc);

    /* Repair Bank Memory (BOOT_CTRL cmd=2) BEFORE the BIST march — required so the
     * march pattern-inits + applies the fusebox repairs to the repairable BANK
     * memories (MCAST_MID 0x240000, MCAST_POST 0x260000, STATS_BANK 0x200000).
     * Without it those banks have invalid ECC and ANY access (incl a CRM fill)
     * hangs the chip — the CPU-punt RX blocker. EOS does this in fm6000PrebootSwitch
     * right before fm6000BistMemoryInit (arista phase65 / RE of ExecuteBootCommand).
     * Proper ExecuteBootCommand: idle -> delay -> cmd -> poll CommandDone (bit4). */
    {
        int r; uint32_t bcr;
        i2c_wr(0x1C022, 0x0); usleep(2000);
        i2c_wr(0x1C022, 0x2);
        for (r = 0; r < 400; r++) { if (i2c_rd(0x1C022) & 0x10) break; usleep(5000); }
        bcr = i2c_rd(0x1C022);
        fprintf(stderr, "[i2c-bringup] Repair Bank Memory (cmd=2): BOOT_CTRL=0x%08x after %d polls\n", bcr, r);
    }

    bist();
    pcie();

    if (do_rescan) {
        int rf = open("/sys/bus/pci/rescan", O_WRONLY);
        if (rf >= 0) { if (write(rf, "1", 1) < 0) {} close(rf); }
        usleep(2000000);
        int vf = open("/sys/bus/pci/devices/0000:02:00.0/vendor", O_RDONLY);
        if (vf >= 0) {
            char vv[16] = { 0 }; if (read(vf, vv, sizeof vv - 1) < 0) {} close(vf);
            fprintf(stderr, "[i2c-bringup] *** FM6000 ENUMERATED (BIST'd): vendor=%s ***\n", vv);
        } else {
            fprintf(stderr, "[i2c-bringup] FM6000 NOT enumerated (retry: reprogram clock + reset)\n");
            close(g_fd); return 2;
        }
    }
    close(g_fd);
    return 0;
}
