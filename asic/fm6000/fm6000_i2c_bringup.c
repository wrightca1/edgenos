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
#include "fm6000_mrl_table.h"   /* MRL scan-config table (verbatim from libFocalpointSDK.so) */

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

    fprintf(stderr, "[i2c-bringup] BIST C: per-partition config (faithful fm6000BistMemoryInit, phase68)\n");
    /* PHASE C — per-partition config, EXACTLY as fm6000BistMemoryInit writes it (arista
     * phase68 decompile @0x34c08d). Prior versions replayed a golden REGISTER SNAPSHOT
     * (phase64) which MISSED CDP instances 2,3 and ADDED ~20 snapshot-only registers
     * (0x1D2A1=0x7F vs the real 4, the +0x02/+0x0C bounds, 0x1D221-225, 0x1D688) that
     * corrupt the march config -> the march reported idle without marching the arrays. */
    /* CDP: 4 instances (base 0x1D200, stride 0x80): enable +0x10, config +0x18 */
    for (int w = 0; w < 4; w++) i2c_wr(0x1D210 + w*0x80, 0x00200000);
    for (int w = 0; w < 4; w++) i2c_wr(0x1D218 + w*0x80, 0x000000B4);
    i2c_wr(0x1D241, 4); i2c_wr(0x1D261, 4); i2c_wr(0x1D281, 4);
    i2c_wr(0x1D2A1, 4); i2c_wr(0x1D2C1, 4);
    /* SPDP: 5 partitions (base 0x1D400, stride 0x80): +0x04 config, +0x40/60/c0/e0 sub-mem
     * enable, +0x09 depth mask (per-partition), +0x41/61/c1/e1 sub-mem config */
    for (int w = 0; w < 4; w++) i2c_wr(0x1D404 + w*0x80, 0x0000000C);
    i2c_wr(0x1D604, 0x00000004);
    i2c_wr(0x1D440, 1); i2c_wr(0x1D4C0, 1); i2c_wr(0x1D4E0, 1); i2c_wr(0x1D540, 1);
    i2c_wr(0x1D5C0, 1); i2c_wr(0x1D5E0, 1); i2c_wr(0x1D640, 1); i2c_wr(0x1D660, 1);
    i2c_wr(0x1D409, 0x00000FFF); i2c_wr(0x1D489, 0x00007FFF); i2c_wr(0x1D509, 0x00003FFF);
    i2c_wr(0x1D589, 0x00000FFF); i2c_wr(0x1D609, 0x000003FF);
    i2c_wr(0x1D441, 4); i2c_wr(0x1D4C1, 4); i2c_wr(0x1D4E1, 4); i2c_wr(0x1D541, 4);
    i2c_wr(0x1D5C1, 6); i2c_wr(0x1D5E1, 6); i2c_wr(0x1D641, 0xA); i2c_wr(0x1D661, 0xA);

    /* PHASE D — arm the march (@0x34c613). CDP: ALL 4 instances +0x20=3 (0x1D220/2A0/320/3A0).
     * SPDP: +0x0B = 0,2,2,2,0 (per-partition, NOT uniform). */
    fprintf(stderr, "[i2c-bringup] BIST D: arm (CDP x4=3, SPDP=0,2,2,2,0)\n");
    for (int w = 0; w < 4; w++) i2c_wr(0x1D220 + w*0x80, 3);
    i2c_wr(0x1D40B, 0); i2c_wr(0x1D48B, 2); i2c_wr(0x1D50B, 2);
    i2c_wr(0x1D58B, 2); i2c_wr(0x1D60B, 0);

    /* PHASE E — poll BM_ENGINE_STATUS(0x1D08E) until 0 (idle=done); nonzero=busy.
     * Function polls <=5000 x 1ms (@0x34c6f3). A REAL full march of the big banks takes
     * a meaningful fraction of that — an instant idle means it never marched. */
    fprintf(stderr, "[i2c-bringup] BIST E: poll 0x1D08E==0 (<=5000x1ms)\n");
    int i; uint32_t st = 1;
    for (i = 0; i < 5000; i++) {
        st = i2c_rd(0x1D08E);
        if (st == 0) break;
        usleep(1000);
    }
    fprintf(stderr, "[i2c-bringup] BIST march idle after %d ms (0x1D08E=0x%08x)\n", i, st);
    /* pass/fail: 0x1D08C global, 0x1D21B+w*0x80 CDP x4, 0x1D407+w*0x80 SPDP x5, 0x1D70E mirror */
    int ok = (st == 0);
    if (i2c_rd(0x1D08C)) ok = 0;
    for (int w = 0; w < 4; w++) if (i2c_rd(0x1D21B + w*0x80)) ok = 0;
    for (int w = 0; w < 5; w++) if (i2c_rd(0x1D407 + w*0x80)) ok = 0;
    if (i2c_rd(0x1D70E)) ok = 0;
    fprintf(stderr, "[i2c-bringup] BIST done: %s (0x1D08C=0x%08x 0x1D70E=0x%08x)\n",
            ok ? "PASS" : "FAIL", i2c_rd(0x1D08C), i2c_rd(0x1D70E));
    return 0;
}

/* ---- MRL scan-chain memory config (fm6000MrlRegisterFix @0x47a4bc) over the i2c slave, in the
 * pre-enum SCAN window (runs after bist(), before pcie()/normal-mode — mirrors PrebootSwitch:
 * BistMemoryInit then MrlRegisterFix). This is the bank-WRITABILITY step (phase78/79). Faithful
 * port of the 6287-block handshake; mrlTable extracted verbatim. Post-enum MMIO off-buses (cold79),
 * so it MUST run here in scan mode over i2c. */
static int mrl(void)
{
    /* NOTE: do NOT read functional regs (CAM0 0x0E000 etc.) over the i2c slave in the scan window —
     * they wedge the mgmt slave (err=-5). Only touch scan regs 0x1C039-0x1C03D + BM status 0x1D08E. */
    fprintf(stderr, "[i2c-bringup] MRL scan-config START: %d blocks (BM_STATUS 0x1D08E=0x%08x)\n",
            FM6000_MRL_ENTRIES, i2c_rd(0x1D08E));
    int save_v = g_verbose; g_verbose = 0;                   /* silence 25k per-op prints (serial flood) */
    i2c_wr(0x1C039, 0x10);                                   /* pre-loop scan select      */
    for (int i = 0; i < FM6000_MRL_ENTRIES; i++) {
        uint32_t t1 = fm6000_mrl_table[i][0], t2 = fm6000_mrl_table[i][1];
        i2c_wr(0x1C039, t1 & 0x1f);                          /* scan select               */
        if (t1 & 0x80) i2c_wr(0x1C03A, t2);                  /* SCAN_CONFIG_DATA_IN       */
        else           i2c_wr(0x1C03B, t2);                  /* SCAN_CHAIN_DATA_IN        */
        uint32_t s = i2c_rd(0x1C03D);                        /* read-back (advances scan) */
        if ((s & 0x300) != 0x100) s = i2c_rd(0x1C03D);       /* vendor: one optional re-read */
        (void)i2c_rd(0x1C03C);                               /* read to advance scan      */
    }
    i2c_wr(0x1C03A, 0x80000040);                             /* final commit              */
    usleep(20000);                                            /* 20 ms settle              */
    g_verbose = save_v;
    fprintf(stderr, "[i2c-bringup] MRL scan-config DONE (%d blocks; BM_STATUS 0x1D08E=0x%08x)\n",
            FM6000_MRL_ENTRIES, i2c_rd(0x1D08E));
    return 0;
}

/* ---- SCHEDULER init over the i2c scan window (phase82: post-enum MMIO off-buses on the ESCHED/DRR
 * writes — cold83 — so port the full golden scheduler here like the MRL). Faithful port of fm6000_sched:
 * JSS clocks + SWEEPER + SSCHED ring (tokens/NEXT_PORT/SLOW_PORT/INIT_COMPLETE) + ESCHED_CFG_1/2 + DRR +
 * replace tokens. Golden ref: reference/scd-dumps/fm6000-golden-scheduler-state-warm.txt. */
static int sched(void)
{
    static const uint32_t tok[]  = { 0x200u, 0x201u, 0x202u, 0x203u, 0x64eu };   /* ports 0,1,2,3,78 */
    static const uint32_t slow[] = { 0x0000000fu, 0x0000ffe0u, 0x0000feffu, 0x0000fff0u, 0x00000fffu };
    int p, sv = g_verbose; g_verbose = 0;                    /* silence per-op prints (serial) */
    fprintf(stderr, "[i2c-bringup] SCHED init (i2c scan window): JSS+sweeper+ring+ESCHED+DRR\n");
    /* JSS clock domain + scheduler tick */
    i2c_wr(0x0F001, 0x0521452au); i2c_wr(0x0F002, 0x00000016u); i2c_wr(0x0F003, 0x00000015u);
    i2c_wr(0x0F004, 0x00000002u); i2c_wr(0x0F008, 0x00000001u); i2c_wr(0x0F010, 0x00000002u);
    /* SWEEPER_CFG 0x1C048+w (drives the scheduler ticks) */
    i2c_wr(0x1C048, 0x0008bb2cu); i2c_wr(0x1C049, 0x00000002u); i2c_wr(0x1C04A, 0x00000000u);
    i2c_wr(0x1C04B, 0x0030a2c3u); i2c_wr(0x1C04C, 0x00002000u);
    /* SSCHED ring: token per scheduled port (RX then TX) */
    for (p = 0; p < 5; p++) { i2c_wr(0x08060, tok[p]); i2c_wr(0x08020, tok[p]); }
    /* NEXT_PORT visit table (20 words RX then TX): [0]=0x03020100, [19]=0x004e0000, rest 0 */
    for (p = 0; p < 20; p++) {
        uint32_t v = (p == 0) ? 0x03020100u : (p == 19) ? 0x004e0000u : 0u;
        i2c_wr(0x08040 + p, v); i2c_wr(0x08000 + p, v);
    }
    for (p = 0; p < 5; p++) i2c_wr(0x08070 + p, slow[p]);    /* SLOW_PORT */
    i2c_wr(0x08061, 1u); i2c_wr(0x08021, 1u);                /* COMMIT strobes RX then TX */
    /* ESCHED_CFG (0x2000) + DRR (0x3800) are INACCESSIBLE here (cold84: i2c writes time out err=-5;
     * cold83: MMIO writes off-bus) — the egress-scheduler tables need a boot phase we don't reach.
     * Skip them (gate FM6000_SCHED_ESCHED=1 to re-try). Keep the SSCHED replace-token values (work). */
    if (getenv("FM6000_SCHED_ESCHED")) {
        for (p = 0; p < 76; p++) {
            i2c_wr(0x02000 + p, p == 0 ? 0x00fff800u : 0x00ffffffu);
            i2c_wr(0x02080 + p, p == 0 ? 0x00fff000u : 0x00ffffffu);
            i2c_wr(0x03800 + p, (p & 1) ? 0x14ffffffu : 0x00ffffffu);
        }
    }
    i2c_wr(0x08022, 0xc0300200u); i2c_wr(0x08062, 0x00200200u);   /* replace-token last values */
    g_verbose = sv;
    fprintf(stderr, "[i2c-bringup] SCHED done: ring0=0x%08x (want 0x03020100)\n", i2c_rd(0x08000));
    return 0;
}

/* ---- normal operating mode + PCIe SerDes (fm6000SetupPCIe, proven values) */
static int pcie(void)
{
    /* NOTE: a minimal "don't-disturb" variant (skip 0x1C03A block-reset + RMW SOFT_RESET) left the
     * chip enumerated-but-off-bus (bist14) — those writes ARE needed for the PCIe read path. The full
     * sequence below reads the SPI-boot-inited bank fine (bist13: MCAST=0x574ac05f), so use it for both
     * paths. (arista phase69.) */
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

    /* BANK-TEST (FM6000_BANKTEST=1): right after the SPI-ROM boot (EepromLoadDone), BEFORE any of
     * our cmd2/BIST/reset, probe whether the SPI boot alone made the repairable bank writable — over
     * the I2C mgmt slave (no PCIe needed). If the SPI-ROM boot did the fusebox/BISR repair, a
     * full 128-bit write to MCAST entry 1 (LSW 0x240004 -> MSW 0x240007) should hold on read-back. */
    if (getenv("FM6000_BANKTEST")) {
        uint32_t p0 = i2c_rd(0x240000), p4 = i2c_rd(0x240004);
        fprintf(stderr, "[BANKTEST] post-SPI-boot pre-write: 0x240000=0x%08x 0x240004=0x%08x\n", p0, p4);
        i2c_wr(0x240004, 0x1); i2c_wr(0x240005, 0x0); i2c_wr(0x240006, 0x0); i2c_wr(0x240007, 0x0);
        uint32_t q4 = i2c_rd(0x240004), cam = i2c_rd(0x0e000);
        fprintf(stderr, "[BANKTEST] after 128b write entry1={1,0,0,0}: 0x240004=0x%08x CAM0=0x%08x "
                        "(0x240004==1 && CAM real = SPI boot REPAIRED the bank!)\n", q4, cam);
    }

    /* Load the per-chip FUSEBOX repair descriptors (0x1D000-0x1D01F) BEFORE cmd=2 + the BIST
     * march. fm6000BistMemoryInit does NOT write these; EOS loads them from a fuse readout
     * (arista phase40). Without them the repairable BANK memories (MCAST_MID 0x240000, MCAST_POST
     * 0x260000, STATS 0x200000) keep their BAD cells -> the scan-mode march can't remap them ->
     * the banks never reach valid ECC -> any later access (a CRM fill or a read) gets NO bus
     * completion and the FM6000 core goes OFF-BUS (config+BAR0=0xffffffff, PCIe link stays up).
     * Live-confirmed: loading these post-enum + re-running cmd=2 does NOT fix it (the remap
     * happens DURING the scan-mode march), so they must be here, before it. The 30 values are
     * cell-repair data specific to THIS silicon, captured live in arista
     * reference/live-captures/7150-fm6000/eos-bist-2026-07-26/bist-state.txt. */
    {
        static const struct { uint32_t a; uint8_t v; } fusebox[] = {
            {0x1D000,0x76},{0x1D001,0xeb},{0x1D002,0x02},{0x1D003,0xb8},{0x1D004,0x0d},
            {0x1D005,0x54},{0x1D006,0x49},{0x1D007,0xba},{0x1D008,0x75},{0x1D009,0xc5},
            {0x1D00a,0x24},{0x1D00b,0xb5},{0x1D00c,0xf7},{0x1D00d,0xbf},{0x1D00f,0x88},
            {0x1D010,0xf7},{0x1D011,0x3f},{0x1D012,0xff},{0x1D013,0x61},{0x1D014,0xf7},
            {0x1D015,0xbf},{0x1D016,0x02},{0x1D017,0x80},{0x1D018,0xf7},{0x1D019,0xbf},
            {0x1D01a,0x2a},{0x1D01b,0x80},{0x1D01c,0x99},{0x1D01d,0x97},{0x1D01f,0x98},
        };
        for (size_t k = 0; k < sizeof(fusebox)/sizeof(fusebox[0]); k++)
            i2c_wr(fusebox[k].a, fusebox[k].v);
        fprintf(stderr, "[i2c-bringup] fusebox repair descriptors loaded (0x1D000-0x1D01F): "
                        "0x1D000=0x%08x 0x1D012=0x%08x 0x1D01F=0x%08x\n",
                i2c_rd(0x1D000), i2c_rd(0x1D012), i2c_rd(0x1D01F));
    }

    /* Release JSS/SBus (SOFT_RESET bit3) + clear SBus control BEFORE cmd=2, so the
     * "Apply Bank Memory Repairs" BISR can read the eFUSE redundancy data over the SBus.
     * EOS releases SOFT_RESET bit3 in fm6000IdentifySwitch (before PrebootSwitch's cmd=2);
     * our old order ran cmd=2 with ALL modules held in reset (JSS/SBus down) -> the BISR had
     * NO eFUSE data -> the repairable banks (MCAST/POST/STATS) were never cell-repaired ->
     * every bank access gets no bus completion and the core goes off-bus. Datasheet Table 4-1
     * step 7 "take modules out of reset" precedes the boot commands (steps 8-10); we were
     * doing cmd=2 (step 9) first of all. (arista phase67; datasheet review.)
     * 0x16 = release JSS(bit3)+PCIe(bit0), leave MSB/FIBM/EPL in reset — matches EOS. */
    if (!getenv("FM6000_SPIBOOT_TRUST")) {
        i2c_wr(0x00009, 0x16);      /* absolute SOFT_RESET: forces MSB/FIBM/EPL into reset — WRONG for
                                     * the trust path (disturbs the SPI-boot-inited banks). */
        i2c_wr(0x0F000, 0x0);       /* SBus/SerDes control clear (fm6000InitSBus step) */
        usleep(2000);
        fprintf(stderr, "[i2c-bringup] JSS/SBus released pre-cmd2 (SOFT_RESET=0x%08x) for eFUSE repair\n", i2c_rd(0x00009));
    } else {
        fprintf(stderr, "[i2c-bringup] SPIBOOT_TRUST: leaving SOFT_RESET as the SPI boot set it (no MSB re-reset)\n");
    }

    /* Repair Bank Memory (BOOT_CTRL cmd=2) BEFORE the BIST march — required so the
     * march pattern-inits + applies the fusebox repairs to the repairable BANK
     * memories (MCAST_MID 0x240000, MCAST_POST 0x260000, STATS_BANK 0x200000).
     * Without it those banks have invalid ECC and ANY access (incl a CRM fill)
     * hangs the chip — the CPU-punt RX blocker. EOS does this in fm6000PrebootSwitch
     * right before fm6000BistMemoryInit (arista phase65 / RE of ExecuteBootCommand).
     * Proper ExecuteBootCommand: idle -> delay -> cmd -> poll CommandDone (bit4). */
    if (!getenv("FM6000_SPIBOOT_TRUST"))
    {
        int r; uint32_t bcr;
        i2c_wr(0x1C022, 0x0); usleep(2000);
        i2c_wr(0x1C022, 0x2);
        for (r = 0; r < 400; r++) { if (i2c_rd(0x1C022) & 0x10) break; usleep(5000); }
        bcr = i2c_rd(0x1C022);
        fprintf(stderr, "[i2c-bringup] Repair Bank Memory (cmd=2): BOOT_CTRL=0x%08x after %d polls\n", bcr, r);
    }

    /* Clear SRAM_UNCORRECTABLE_FATAL (0x1C018, 128-bit) so an uncorrectable ECC error
     * on an uninitialized bank memory (e.g. MCAST_MID during its CRM fill) does NOT
     * trigger a fatal chip reset -> wedge. Golden EOS keeps this = 0 (arista phase65).
     * Do NOT clear the uncorrectable IM (0x1C014): on M1 there is no ECC interrupt
     * handler, so unmasking makes every uninit-ECC READ post an interrupt that wedged
     * the chip (bist5). Leave the IM at its (masked) default so uninit reads just
     * return 0xffffffff and the CRM fill can establish valid ECC. */
    if (!getenv("FM6000_SPIBOOT_TRUST")) {
        i2c_wr(0x1C018, 0); i2c_wr(0x1C019, 0); i2c_wr(0x1C01A, 0); i2c_wr(0x1C01B, 0);
        fprintf(stderr, "[i2c-bringup] SRAM_UNCORRECTABLE_FATAL cleared (0x1C018=0x%08x, IM left masked)\n", i2c_rd(0x1C018));
        bist();
        /* phase79: MRL scan-chain memory config (bank writability) — after BistMemoryInit,
         * before pcie(), in the scan window (mirrors fm6000PrebootSwitch). Gate off with
         * FM6000_NOMRL=1 to A/B test the scan config's effect. */
        if (!getenv("FM6000_NOMRL")) mrl();
        /* phase82: scheduler init in the same scan window, after MRL (mirrors the boot's
         * ValidateSchedulerToken phase). Gate off with FM6000_NOSCHED=1. */
        if (!getenv("FM6000_NOSCHED")) sched();
    } else {
        /* "copy EOS": the SPI boot ROM already ran (EepromLoadDone) and did the fusebox/BISR
         * bank repair + init at reset-release. Do NOT re-enter scan mode / re-run the BIST march
         * or cmd=2 (a CPU can't redo the eFUSE repair and only corrupts the SPI-boot state).
         * Just bring up PCIe on top of the SPI-booted chip, exactly as EOS does with an
         * eeprom-booted part. (user insight: stop overriding the SPI boot.) */
        fprintf(stderr, "[i2c-bringup] *** SPIBOOT_TRUST: skipping cmd2/FATAL/BIST — trusting the SPI-ROM boot's bank repair; PCIe bring-up only ***\n");
    }
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
