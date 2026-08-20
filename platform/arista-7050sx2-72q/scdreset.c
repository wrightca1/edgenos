/* SPDX-License-Identifier: GPL-2.0-only
 *
 * GPL-2.0 BECAUSE OF THE SMBUS MASTER, NOT BY CHOICE.
 *
 * The SCD SMBus section in this file is transcribed from Arista's GPL-2.0
 * driver (scd-smbus.c) -- its bitfield layout and transaction protocol, not
 * merely its register addresses. Transcription produces a derivative work, so
 * the licence follows it and this whole file carries GPL-2.0 even where the
 * surrounding project is permissively licensed.
 *
 * The other Arista GPL drivers this file draws on -- scd-reset.c, scd-led.c,
 * raven-fan-driver.c, crow-fan-driver.c -- were read for register MAPS only.
 * Those are facts and carry no licence; every offset used here is published by
 * Arista in the GPL-2.0 aristanetworks/sonic tree.
 *
 * If this file is ever wanted under a permissive licence, the SMBus master has
 * to be rewritten from the register map alone, without reference to the driver
 * source.
 *
 * NOTE TO ANYONE SYNCING THIS FILE ELSEWHERE: this header is load-bearing.
 * Copying the file over a downstream copy without it silently relicenses
 * GPL code, which is exactly what happened once on 2026-08-20 and was caught
 * only by checking the first line after the copy.
 */
/* scdreset -- Arista SCD + BCM56860 bring-up tool for EdgeNOS.
 *
 * Static x86-64, no dependencies: runs in the busybox initramfs.
 *
 * SCD register semantics from Arista's GPL scd-reset.c / arista/core/asic.py:
 *   0x4000  read state, write-1-to-SET   (1 = reset asserted)
 *   0x4010  write-1-to-CLEAR             (release)
 *   0x4020  status
 *   0x0120  watchdog: (1<<31)|(action<<29)|(deciseconds<<16), action 2 = power cycle
 *
 * The watchdog timeout field was measured live: bits [28:16] in 100 ms units.
 * Aboot hands over with the watchdog DISARMED, so a custom NOS starts with no
 * recovery net -- arm it and pet it before touching anything.
 *
 * S-Channel layout per docs/RE-METHOD-AND-SCHAN-FACTS.md.  Note the CMC0 guard
 * in tools/schan.py does NOT apply here: that guard exists because EOS's Strata
 * agents drive CMC0 under a mutex we cannot join.  Under EdgeNOS nothing else
 * touches the chip, so CMC0 is the correct default.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <stdint.h>
#include <signal.h>

#define SCD_RES  "/sys/bus/pci/devices/0000:05:00.0/resource0"
#define ASIC_RES "/sys/bus/pci/devices/0000:01:00.0/resource0"
#define ASIC_DEV "/sys/bus/pci/devices/0000:01:00.0"

/* The SCD's BAR0 is 512 KB -- confirmed from sysfs:
 *   resource0 is 524288 bytes, 0xfc000000-0xfc07ffff
 * This was 0x10000 for a long time, and do_scdscan() clamps its upper bound to
 * it, so every "scan to 0x20000" silently covered only the first 64 KB and the
 * conclusion "the fan registers are not in the SCD" was drawn over one eighth
 * of the register space. */
#define SCD_MAP_SIZE  0x80000
#define ASIC_MAP_SIZE 0x40000

#define RESET_READ   0x4000
#define RESET_CLEAR  0x4010
#define RESET_STATUS 0x4020
#define WD_REG       0x0120

/* SCHAN */
#define OFF_CTRL 0x000
#define OFF_ACK  0x004
#define OFF_ERR  0x008
#define OFF_MSG0 0x00c
#define MSG_START      (1u << 0)
#define MSG_DONE       (1u << 1)
#define SER_CHECK_FAIL (1u << 20)
#define SCHAN_NACK     (1u << 21)
#define SCHAN_TIMEOUT  (1u << 22)
#define SCHAN_ERROR    (1u << 23)
/* CMIC_CMCx_SCHAN_CTRL abort bit. The SDK toggles this to recover a timed-out
 * S-Channel (_soc_cmicm_schan_reset, cmicm_schan.c:118, SC_CMCx_SCHAN_ABORT
 * in cmicm.h:109). Without it a single timeout leaves the channel wedged and
 * every following op times out too. */
#define SCHAN_ABORT    (1u << 2)

static int core_bit = 0, pcie_bit = 1;
static volatile uint32_t *scd, *asic;

static volatile uint32_t *map_res(const char *path, size_t len, int write)
{
    int fd = open(path, write ? O_RDWR : O_RDONLY);
    void *p;
    if (fd < 0) { perror(path); return NULL; }
    p = mmap(NULL, len, PROT_READ | (write ? PROT_WRITE : 0), MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); return NULL; }
    return (volatile uint32_t *)p;
}

static uint32_t rd(volatile uint32_t *b, uint32_t off) { return b[off / 4]; }
static void wr(volatile uint32_t *b, uint32_t off, uint32_t v)
{
    b[off / 4] = v;
    __sync_synchronize();
}

static void msleep(long ms) { usleep(ms * 1000); }

static uint32_t cmc_base(int cmc)
{
    switch (cmc) {
    case 0: return 0x031000;
    case 1: return 0x032000;
    default: return 0x033000;
    }
}

static void decode_ctrl(uint32_t v)
{
    printf("[");
    if (v & MSG_START)      printf("MSG_START ");
    if (v & MSG_DONE)       printf("MSG_DONE ");
    if (v & SER_CHECK_FAIL) printf("SER_CHECK_FAIL ");
    if (v & SCHAN_NACK)     printf("NACK ");
    if (v & SCHAN_TIMEOUT)  printf("TIMEOUT ");
    if (v & SCHAN_ERROR)    printf("SCHAN_ERROR ");
    printf("]");
}

static void show_reset(void)
{
    uint32_t v = rd(scd, RESET_READ);
    printf("SCD 0x%04x = 0x%08x   (bit set = reset ASSERTED)\n", RESET_READ, v);
    printf("  bit %d core : %s\n", core_bit,
           (v >> core_bit) & 1 ? "ASSERTED" : "released");
    printf("  bit %d pcie : %s\n", pcie_bit,
           (v >> pcie_bit) & 1 ? "ASSERTED" : "released");
    printf("SCD 0x%04x = 0x%08x   (status)\n", RESET_STATUS, rd(scd, RESET_STATUS));
    printf("ASIC 0000:01:00.0 on bus: %s\n",
           access(ASIC_DEV, F_OK) == 0 ? "YES" : "no");
}

static void show_wd(void)
{
    uint32_t v = rd(scd, WD_REG);
    printf("SCD watchdog 0x%04x = 0x%08x  enabled=%u action=%u "
           "timeout[28:16]=%u (%.1fs) low16=%u\n",
           WD_REG, v, (v >> 31) & 1, (v >> 29) & 3, (v >> 16) & 0x1fff,
           ((v >> 16) & 0x1fff) / 10.0, v & 0xffff);
}

/* The low 16 bits are NOT the timeout on this SCD -- Aboot leaves 0x1770 there
 * while the [28:16] field (500) is what actually expired, at 100 ms units.
 * Their meaning is unknown, so preserve Aboot's value rather than guess. */
#define WD_LOW16 0x1770

static uint32_t wd_value(unsigned deciseconds)
{
    if (!deciseconds) return 0;
    return (1u << 31) | (2u << 29) | ((deciseconds & 0x1fff) << 16) | WD_LOW16;
}

static int do_release(void)
{
    uint32_t v = rd(scd, RESET_READ);
    printf("before: 0x%08x\n", v);
    if (((v >> core_bit) & 1) || ((v >> pcie_bit) & 1)) {
        printf("clearing core reset (bit %d)\n", core_bit);
        wr(scd, RESET_CLEAR, 1u << core_bit);
        printf("  0x4000 = 0x%08x\n", rd(scd, RESET_READ));
        msleep(500);
        printf("clearing pcie reset (bit %d)\n", pcie_bit);
        wr(scd, RESET_CLEAR, 1u << pcie_bit);
        printf("  0x4000 = 0x%08x\n", rd(scd, RESET_READ));
    } else {
        printf("already out of reset\n");
    }

    msleep(1000);
    if (access(ASIC_DEV, F_OK) != 0) {
        int fd = open("/sys/bus/pci/rescan", O_WRONLY);
        printf("rescanning PCI\n");
        if (fd >= 0) { if (write(fd, "1\n", 2) < 0) perror("rescan"); close(fd); }
        else perror("open rescan");
    }
    for (int i = 0; i < 600; i++) {
        if (access(ASIC_DEV, F_OK) == 0) {
            int fd;
            printf("*** ASIC 0000:01:00.0 IS ON THE BUS ***\n");
            msleep(2000);
            /* Nothing binds a driver here, so pci_enable_device() never runs
             * and Memory Space Enable stays 0 -- every BAR read would come
             * back 0xffffffff. Enable it explicitly. */
            fd = open(ASIC_DEV "/enable", O_WRONLY);
            if (fd >= 0) {
                if (write(fd, "1\n", 2) < 0) perror("enable");
                else printf("PCI memory space enabled\n");
                close(fd);
            } else perror("open enable");
            return 0;
        }
        msleep(100);
    }
    printf("timed out waiting for 0000:01:00.0\n");
    return 1;
}

/* CMIC_SBUS_RING_MAP_0_7 .. _56_63 at 0x010098..0x0100b4.
 * 4 bits per block id, 8 block ids per register, LSB nibble = lowest id.
 * Captured from a running EOS on this exact board (artifacts/eos-ringmap.txt);
 * unprogrammed the CMIC has no route to any block and every S-Channel
 * transaction times out. */
#define RING_MAP_BASE 0x010098
/* EXACTLY eight registers, 0x010098..0x0100b4 (block ids 0..63) -- that is all
 * soc_trident2_chip_reset writes (trident2.c:9404-9416) and all
 * soc_trident2_cmic_ring_map_check reads back (9207-9294).
 *
 * We used to carry sixteen entries here, zero-filling 0x0100b8..0x0100d4 on the
 * assumption they were simply unused ring-map registers. They are not
 * registers at all on this part: reading 0x0100b8 returns 0xffffffff (the chip
 * has stopped answering) and the very next read hard-hangs the host. The zeros
 * EOS reports for that range come from an already-initialised chip and mean
 * nothing here. Do not extend this table.
 *
 * The last entry is the TD2P/TT2P value; plain TD2 uses 0x00000550 (9412).
 */
#define RING_MAP_N 8
static const uint32_t RING_MAP[RING_MAP_N] = {
    0x33052100, 0x33776644, 0x33333333, 0x44444444,
    0x66666644, 0x77776666, 0x00777777, 0x00005550,
};

#define SBUS_TIMEOUT   0x010094   /* WRITE_CMIC_SBUS_TIMEOUTr(0x7d0) */
#define PIO_ENDIANESS  0x0101ec   /* CMIC_COMMON_PCIE_PIO_ENDIANESS  */
#define CPS_RESET      0x010220   /* CMIC_CPS_RESET_OFFSET           */

/* Phase P -- soc_pcie_host_intf_init (drv.c:8638), called from soc_do_init
 * BEFORE soc_reset. This is the piece we were missing entirely.
 *
 * CMIC_PCIE_USERIF_PURGE_CONTROL enables PURGING of pending PCIe user-interface
 * transactions on timeout or reset. Without it a PIO access the interface
 * cannot complete never returns a completion -- and the CPU stalls forever
 * waiting for it. That is exactly the hang we kept hitting on read-back.
 *
 *   bit 0 ENABLE_PURGE_IF_USERIF_TIMESOUT     (sic, SDK's spelling)
 *   bit 1 ENABLE_PURGE_IF_USERIF_RESET
 *   bit 2 ENABLE_PURGE_SW_PROGRAMMABLE
 *   bit 3 ENABLE_PIO_PURGE_SW_PROGRAMMABLE
 *   bit 4 ENABLE_PIO_PURGE_IF_USERIF_RESET
 *
 * SDK sets bits 0,1,4 = 0x13. Live EOS reads 0x13 -- predicted, then confirmed.
 * Live EOS USERIF_TIMEOUT reads 0x00100000 (the SDK's own default property is
 * 50000000 usec, so Arista overrides it; we use the board's actual value).
 */
#define USERIF_TIMEOUT       0x010250
#define USERIF_STATUS        0x010254
#define USERIF_PURGE_CONTROL 0x010260
#define USERIF_PURGE_STATUS  0x010264

static int phaseP(void)
{
    printf("phaseP: USERIF timeout  -- write 0x%06x = 0x00100000\n",
           USERIF_TIMEOUT);
    wr(asic, USERIF_TIMEOUT, 0x00100000);
    printf("phaseP: USERIF purge    -- write 0x%06x = 0x00000013\n",
           USERIF_PURGE_CONTROL);
    wr(asic, USERIF_PURGE_CONTROL, 0x00000013);
    printf("phaseP: writes issued; reading them back is now supposed to be safe\n");
    printf("phaseP:   0x%06x = 0x%08x\n", USERIF_TIMEOUT, rd(asic, USERIF_TIMEOUT));
    printf("phaseP:   0x%06x = 0x%08x\n", USERIF_PURGE_CONTROL,
           rd(asic, USERIF_PURGE_CONTROL));
    printf("phaseP:   0x%06x = 0x%08x (status)\n", USERIF_STATUS,
           rd(asic, USERIF_STATUS));
    printf("phaseP:   0x%06x = 0x%08x (purge status)\n", USERIF_PURGE_STATUS,
           rd(asic, USERIF_PURGE_STATUS));
    printf("phaseP: done\n");
    return 0;
}

/* Phase 0 -- soc_reset() up to the chip_reset dispatch (drv.c:18640..19135).
 * On this part (BCM56860 -> BCM56850 features: cmicm+mcs, NOT iproc, NOT
 * reset_delay) it reduces to two writes and two delays:
 *
 *   soc_endian_config     0x0101ec = 0   (big_pio=0 on little-endian x86)
 *   soc_pci_ep_config     no-op          (requires soc_feature_iproc)
 *   soc_pci_burst_enable  no-op          (returns immediately for CMICm)
 *   CPS reset             0x010220 = 1
 *   sal_usleep            1 ms           (NOT 1 s -- no soc_feature_reset_delay)
 *   sal_usleep            10 ms          (ARL table self-init)
 *   dummy CMIC_CONFIG rd  SKIPPED        (guarded !cmicm && !cmicx)
 *   soc_endian_config     0x0101ec = 0   again -- "reset cleared it"
 *
 * Not implemented, deliberately: the MSI-enable and intr0/1/2 disable that
 * follow. We bind no driver and register no ISR, so there is nothing to take a
 * spurious interrupt. Noted rather than silently skipped.
 */
static int phase0(void)
{
    printf("phase0: endian config -- write 0x%06x = 0\n", PIO_ENDIANESS);
    wr(asic, PIO_ENDIANESS, 0);
    printf("phase0:   readback 0x%08x\n", rd(asic, PIO_ENDIANESS));

    printf("phase0: CPS RESET -- write 0x%06x = 1\n", CPS_RESET);
    wr(asic, CPS_RESET, 1);
    printf("phase0:   write returned\n");

    msleep(1);
    msleep(10);

    printf("phase0: CPS_RESET reads back 0x%08x\n", rd(asic, CPS_RESET));
    printf("phase0: DEV_REV_ID reads back 0x%08x (expect 0x0002b860)\n",
           rd(asic, 0x010224));

    printf("phase0: restore endian -- write 0x%06x = 0\n", PIO_ENDIANESS);
    wr(asic, PIO_ENDIANESS, 0);
    printf("phase0: done\n");
    return 0;
}

/* Phase A -- the first hardware actions of soc_trident2_chip_reset.
 * SBUS timeout first, then the ring map (source order is ring map then
 * timeout, but the timeout is what bounds a stalled SBUS access, so setting
 * it first is strictly safer for us). */
static int phaseA(void)
{
    int i;

    printf("phaseA: SBUS timeout -- write 0x%06x = 0x7d0\n", SBUS_TIMEOUT);
    wr(asic, SBUS_TIMEOUT, 0x7d0);
    printf("phaseA:   readback 0x%08x\n", rd(asic, SBUS_TIMEOUT));

    printf("phaseA: ring map, one register at a time\n");
    for (i = 0; i < RING_MAP_N; i++) {
        printf("phaseA:   w 0x%06x = 0x%08x ... ", RING_MAP_BASE + i * 4,
               RING_MAP[i]);
        wr(asic, RING_MAP_BASE + i * 4, RING_MAP[i]);
        printf("ok\n");
    }

    printf("phaseA: verify\n");
    for (i = 0; i < RING_MAP_N; i++) {
        uint32_t got = rd(asic, RING_MAP_BASE + i * 4);
        printf("  0x%06x = 0x%08x  %s\n", RING_MAP_BASE + i * 4, got,
               got == RING_MAP[i] ? "ok" : "** MISMATCH **");
    }
    return 0;
}

/* ---- uC core halt (the step we have never performed) ----------------------
 *
 * soc_trident2_chip_reset does this between the SBUS timeout and its FIRST
 * S-Channel transaction (trident2.c:9423-9438), guarded by soc_feature_mcs --
 * which IS true for us: BCM56860 falls through to soc_features_bcm56850_a0
 * (feature.c:6816) and 56850 lists soc_feature_mcs (feature.c:2833).
 *
 *   READ_UC_0_RST_CONTROLr; BYPASS_RSTFSM_CTRL=1; CPUHALT_N=0; WRITE
 *   READ_UC_1_RST_CONTROLr;                       CPUHALT_N=0; WRITE
 *
 * These are `soc_mcsreg` registers (allregs_u.i:39829/40177), NOT S-Channel.
 * They are reached by CMIC PIO through a paging window (reg.c:7546-7570):
 *
 *   page = addr & 0xffff8000;  off = addr & 0x00007fff
 *   soc_pci_write(CMIC_PIO_MCS_ACCESS_PAGE, page)
 *   soc_pci_read (CMIC_PIO_MCS_ACCESS_PAGE)        <- read back to flush
 *   soc_pci_{read,write}(0x38000 + off, ...)
 *
 * UC_0_RST_CONTROL = 0x7006000 -> page 0x07000000, BAR0 0x3e000
 * UC_1_RST_CONTROL = 0x7006008 -> page 0x07000000, BAR0 0x3e008
 *
 * Field bits from UC_x_RST_CONTROL: BYPASS_RSTFSM_CTRL [31], CPUHALT_N [2],
 * DEBUG_RESET_N [3], SYS_PORESET_N [1], CORE_RESET_N [0].
 *
 * Why this is worth a boot: it is the only remaining step inside chip_reset
 * before the first S-Channel op that we skip, and if the embedded uC cores are
 * live on the SBUS they would contend with the CMIC for the ring -- which is
 * exactly the symptom (every block silent, SCHAN_ERR clear).
 */
#define MCS_ACCESS_PAGE 0x010204
#define MCS_WINDOW      0x038000
#define UC_0_RST_CONTROL 0x7006000
#define UC_1_RST_CONTROL 0x7006008
#define UC_BYPASS_RSTFSM (1u << 31)
#define UC_CPUHALT_N     (1u << 2)

static uint32_t mcs_rd(uint32_t addr)
{
    uint32_t off = addr & 0x00007fff;
    wr(asic, MCS_ACCESS_PAGE, addr & 0xffff8000);
    (void)rd(asic, MCS_ACCESS_PAGE);     /* SDK reads back to flush the page */
    return rd(asic, MCS_WINDOW + off);
}

static void mcs_wr(uint32_t addr, uint32_t val)
{
    uint32_t off = addr & 0x00007fff;
    wr(asic, MCS_ACCESS_PAGE, addr & 0xffff8000);
    (void)rd(asic, MCS_ACCESS_PAGE);
    wr(asic, MCS_WINDOW + off, val);
}

static int do_uchalt(void)
{
    uint32_t v;

    printf("uchalt: MCS page window at 0x%06x, page reg 0x%06x\n",
           MCS_WINDOW, MCS_ACCESS_PAGE);
    fflush(stdout);
    msleep(300);

    printf("uchalt: READ UC_0_RST_CONTROL (0x%07x -> BAR0 0x%05x) ... ",
           UC_0_RST_CONTROL, MCS_WINDOW + (UC_0_RST_CONTROL & 0x7fff));
    fflush(stdout);
    msleep(300);
    v = mcs_rd(UC_0_RST_CONTROL);
    printf("0x%08x\n", v);
    fflush(stdout);
    msleep(200);

    v |= UC_BYPASS_RSTFSM;
    v &= ~UC_CPUHALT_N;
    printf("uchalt: WRITE UC_0_RST_CONTROL = 0x%08x (BYPASS_RSTFSM=1 CPUHALT_N=0) ... ",
           v);
    fflush(stdout);
    msleep(300);
    mcs_wr(UC_0_RST_CONTROL, v);
    printf("issued\n");
    fflush(stdout);
    msleep(200);

    printf("uchalt: READ UC_1_RST_CONTROL (0x%07x -> BAR0 0x%05x) ... ",
           UC_1_RST_CONTROL, MCS_WINDOW + (UC_1_RST_CONTROL & 0x7fff));
    fflush(stdout);
    msleep(300);
    v = mcs_rd(UC_1_RST_CONTROL);
    printf("0x%08x\n", v);
    fflush(stdout);
    msleep(200);

    v &= ~UC_CPUHALT_N;
    printf("uchalt: WRITE UC_1_RST_CONTROL = 0x%08x (CPUHALT_N=0) ... ", v);
    fflush(stdout);
    msleep(300);
    mcs_wr(UC_1_RST_CONTROL, v);
    printf("issued\n");
    fflush(stdout);
    msleep(300);

    printf("uchalt: verify UC_0 = 0x%08x  UC_1 = 0x%08x\n",
           mcs_rd(UC_0_RST_CONTROL), mcs_rd(UC_1_RST_CONTROL));
    printf("uchalt: done\n");
    return 0;
}

static int schan_read(int cmc, uint32_t hdr, uint32_t addr, int nwords);

/* Phase A3 -- Phase A in the SDK's ACTUAL order, fully instrumented.
 *
 * phaseA above writes CMIC_SBUS_TIMEOUT first, on the reasoning that a bounded
 * timeout is "strictly safer" before touching the ring. That inverted the SDK,
 * which programs the ring map first (trident2.c:9404-9419) and only then the
 * timeout (9421) -- and it is very likely the whole bug. Evidence:
 *
 *   - coldprobe READS 0x010094 on a virgin chip: fine, 0x00002700 (reset value)
 *   - phaseA WRITES 0x010094 first, then reads: wedges the host
 *   - phaseA2 writes ring map first, then timeout: never wedged
 *
 * A posted write cannot stall the CPU by itself, so the write does not "hang" --
 * it leaves the SBUS in a state where the next access to that block never
 * completes. Programming the timeout before any ring exists points the CMIC at
 * an unmapped ring, which is exactly the kind of thing that would do that.
 *
 * So: SDK order, and announce/drain around every single access so that if this
 * still wedges we learn precisely where, instead of losing the line to the UART.
 */
static int phaseA3(int cmc, uint32_t hdr, uint32_t addr, int nwords)
{
    int i, bad = 0;

    printf("phaseA3: SDK order -- ring map FIRST, then SBUS timeout\n");
    msleep(300);

    for (i = 0; i < RING_MAP_N; i++) {
        printf("phaseA3: WRITE ring[%2d] 0x%06x = 0x%08x ... ", i,
               RING_MAP_BASE + i * 4, RING_MAP[i]);
        fflush(stdout);
        msleep(200);
        wr(asic, RING_MAP_BASE + i * 4, RING_MAP[i]);
        printf("issued\n");
        fflush(stdout);
    }

    /* SDK order: ring map, then ring_map_check, and only then the timeout. */
    printf("phaseA3: ring_map_check (trident2.c:9207-9294)\n");
    fflush(stdout);
    msleep(300);

    for (i = 0; i < RING_MAP_N; i++) {
        uint32_t got;
        printf("phaseA3: READ  ring[%2d] 0x%06x ... ", i, RING_MAP_BASE + i * 4);
        fflush(stdout);
        msleep(150);
        got = rd(asic, RING_MAP_BASE + i * 4);
        printf("0x%08x  %s\n", got, got == RING_MAP[i] ? "ok" : "** MISMATCH **");
        fflush(stdout);
        if (got != RING_MAP[i]) bad++;
    }

    printf("phaseA3: ring map verify: %d/%d mismatched\n", bad, RING_MAP_N);
    if (bad) printf("phaseA3: ring map did NOT stick -- S-Channel will time out\n");
    fflush(stdout);
    msleep(300);

    printf("phaseA3: WRITE SBUS timeout 0x%06x = 0x7d0 ... ", SBUS_TIMEOUT);
    fflush(stdout);
    msleep(300);
    wr(asic, SBUS_TIMEOUT, 0x7d0);
    printf("issued\n");
    fflush(stdout);
    msleep(200);

    printf("phaseA3: READ  SBUS timeout 0x%06x ... ", SBUS_TIMEOUT);
    fflush(stdout);
    msleep(300);
    printf("0x%08x\n", rd(asic, SBUS_TIMEOUT));
    fflush(stdout);
    msleep(300);

    printf("phaseA3: S-Channel read via CMC%d\n", cmc);
    return schan_read(cmc, hdr, addr, nwords);
}

static void schan_regs(int cmc)
{
    uint32_t b = cmc_base(cmc);
    uint32_t ctrl = rd(asic, b + OFF_CTRL);
    printf("CMC%d @0x%06x\n", cmc, b);
    printf("  SCHAN_CTRL           = 0x%08x  ", ctrl); decode_ctrl(ctrl); printf("\n");
    printf("  SCHAN_ACK_BEAT_COUNT = 0x%08x\n", rd(asic, b + OFF_ACK));
    printf("  SCHAN_ERR            = 0x%08x\n", rd(asic, b + OFF_ERR));
    for (int i = 0; i < 6; i++)
        printf("  MESSAGE%-2d            = 0x%08x\n", i,
               rd(asic, b + OFF_MSG0 + 4 * i));
}

static int schan_read(int cmc, uint32_t hdr, uint32_t addr, int nwords)
{
    uint32_t b = cmc_base(cmc);
    uint32_t opc = hdr >> 26, ctrl, pre;
    int i;

    if (opc != 7 && opc != 11) {
        printf("REFUSING opcode %u: read commands only (7=READ_MEM, 11=READ_REG)\n",
               opc);
        return 2;
    }
    printf("header 0x%08x  OPC=%u DPORT=%u ACC=%u DLEN=%u\n", hdr, opc,
           (hdr >> 20) & 0x3f, (hdr >> 14) & 0x7, (hdr >> 7) & 0x7f);
    printf("addr   0x%08x -> msg[1]\n", addr);

    pre = rd(asic, b + OFF_CTRL);
    if (pre & MSG_START) {
        printf("REFUSING: MSG_START already set (0x%08x) -- busy\n", pre);
        return 2;
    }
    wr(asic, b + OFF_MSG0 + 0, hdr);
    wr(asic, b + OFF_MSG0 + 4, addr);
    wr(asic, b + OFF_CTRL, MSG_START);

    for (i = 0; i < 100000; i++) {
        ctrl = rd(asic, b + OFF_CTRL);
        if (ctrl & MSG_DONE) break;
    }
    ctrl = rd(asic, b + OFF_CTRL);
    printf("--- result (polled %d) ---\n", i);
    printf("  SCHAN_CTRL = 0x%08x  ", ctrl); decode_ctrl(ctrl); printf("\n");
    printf("  SCHAN_ERR  = 0x%08x\n", rd(asic, b + OFF_ERR));
    if (!(ctrl & MSG_DONE)) { printf("  ** never completed **\n"); return 3; }
    if (ctrl & (SCHAN_NACK | SCHAN_TIMEOUT | SCHAN_ERROR | SER_CHECK_FAIL))
        printf("  ** error flags set **\n");
    for (i = 0; i < nwords; i++)
        printf("  RSP[%2d] = 0x%08x\n", i, rd(asic, b + OFF_MSG0 + 4 * i));
    return 0;
}

/* ---- S-Channel WRITE ------------------------------------------------------
 *
 * The first genuinely destructive capability in this tool. Everything up to now
 * either read the chip or wrote CMIC PIO registers we had ground truth for; an
 * S-Channel write reaches inside a block and can change how the chip behaves.
 *
 * Opcodes (artifacts/bcm56860-registers.json, schan_opcodes):
 *   0x0d WRITE_REG_CMD -> 0x0e WRITE_REG_ACK
 *   0x09 WRITE_MEM_CMD -> 0x0a WRITE_MEM_ACK
 *
 * Layout per RE-METHOD-AND-SCHAN-FACTS.md 20.3: msg[0]=hdr, msg[1]=addr,
 * msg[2]=data, dwc_write=3.
 *
 * Guard rail: a small denylist of registers that reconfigure the chip wholesale.
 * Writing TOP_SOFT_RESET_REG is a legitimate and necessary step -- it is how
 * pipeline blocks come out of reset -- but it should be a deliberate act, not a
 * typo. Set SCHAN_FORCE=1 to override.
 */
static const struct { uint32_t addr; const char *name; } SCHAN_WRITE_DENY[] = {
    { 0x2030100, "TOP_SOFT_RESET_REG"   },
    { 0x2030200, "TOP_SOFT_RESET_REG_2" },
};

static int schan_write_op(int cmc, uint32_t hdr, uint32_t addr, uint32_t data)
{
    uint32_t b = cmc_base(cmc);
    uint32_t opc = hdr >> 26, ctrl, pre;
    unsigned k;
    int i;

    if (opc != 0x0d && opc != 0x09) {
        printf("REFUSING opcode %u: write commands only "
               "(13=WRITE_REG, 9=WRITE_MEM)\n", opc);
        return 2;
    }
    for (k = 0; k < sizeof(SCHAN_WRITE_DENY) / sizeof(SCHAN_WRITE_DENY[0]); k++) {
        if (addr == SCHAN_WRITE_DENY[k].addr && !getenv("SCHAN_FORCE")) {
            printf("REFUSING write to 0x%08x (%s)\n", addr,
                   SCHAN_WRITE_DENY[k].name);
            printf("  this reconfigures the chip; set SCHAN_FORCE=1 to mean it\n");
            return 2;
        }
    }

    printf("header 0x%08x  OPC=%u DPORT=%u ACC=%u DLEN=%u\n", hdr, opc,
           (hdr >> 20) & 0x3f, (hdr >> 14) & 0x7, (hdr >> 7) & 0x7f);
    printf("addr   0x%08x -> msg[1]\n", addr);
    printf("data   0x%08x -> msg[2]\n", data);

    pre = rd(asic, b + OFF_CTRL);
    if (pre & MSG_START) {
        printf("REFUSING: MSG_START already set (0x%08x) -- busy\n", pre);
        return 2;
    }
    wr(asic, b + OFF_MSG0 + 0, hdr);
    wr(asic, b + OFF_MSG0 + 4, addr);
    wr(asic, b + OFF_MSG0 + 8, data);
    wr(asic, b + OFF_CTRL, MSG_START);

    for (i = 0; i < 100000; i++) {
        ctrl = rd(asic, b + OFF_CTRL);
        if (ctrl & MSG_DONE) break;
    }
    ctrl = rd(asic, b + OFF_CTRL);
    printf("--- result (polled %d) ---\n", i);
    printf("  SCHAN_CTRL = 0x%08x  ", ctrl); decode_ctrl(ctrl); printf("\n");
    printf("  SCHAN_ERR  = 0x%08x\n", rd(asic, b + OFF_ERR));
    printf("  ACK[0]     = 0x%08x (opcode %u)\n", rd(asic, b + OFF_MSG0),
           rd(asic, b + OFF_MSG0) >> 26);
    if (!(ctrl & MSG_DONE)) { printf("  ** never completed **\n"); return 3; }
    if (ctrl & (SCHAN_NACK | SCHAN_TIMEOUT | SCHAN_ERROR | SER_CHECK_FAIL)) {
        printf("  ** error flags set **\n");
        return 3;
    }
    return 0;
}

/* Read one 32-bit register over S-Channel, quietly. */
static int schan_get(int cmc, int blk, uint32_t addr, uint32_t *out)
{
    uint32_t b = cmc_base(cmc);
    uint32_t hdr = (0x0bu << 26) | ((blk & 0x3f) << 20) | (4u << 7);
    uint32_t ctrl;
    int i;

    if (rd(asic, b + OFF_CTRL) & MSG_START) return -1;
    wr(asic, b + OFF_MSG0 + 0, hdr);
    wr(asic, b + OFF_MSG0 + 4, addr);
    wr(asic, b + OFF_CTRL, MSG_START);
    for (i = 0; i < 100000; i++) {
        ctrl = rd(asic, b + OFF_CTRL);
        if (ctrl & MSG_DONE) break;
    }
    ctrl = rd(asic, b + OFF_CTRL);
    if (!(ctrl & MSG_DONE) ||
        (ctrl & (SCHAN_NACK | SCHAN_TIMEOUT | SCHAN_ERROR | SER_CHECK_FAIL)))
        return -1;
    *out = rd(asic, b + OFF_MSG0 + 4);
    return 0;
}

/* Write one 32-bit register over S-Channel, quietly. No denylist: callers are
 * the deliberate in-tool sequences, not the user-facing schan-write command. */
static int schan_put(int cmc, int blk, uint32_t addr, uint32_t data)
{
    uint32_t b = cmc_base(cmc);
    uint32_t hdr = (0x0du << 26) | ((blk & 0x3f) << 20) | (4u << 7);
    uint32_t ctrl;
    int i;

    if (rd(asic, b + OFF_CTRL) & MSG_START) return -1;
    wr(asic, b + OFF_MSG0 + 0, hdr);
    wr(asic, b + OFF_MSG0 + 4, addr);
    wr(asic, b + OFF_MSG0 + 8, data);
    wr(asic, b + OFF_CTRL, MSG_START);
    for (i = 0; i < 100000; i++) {
        ctrl = rd(asic, b + OFF_CTRL);
        if (ctrl & MSG_DONE) break;
    }
    ctrl = rd(asic, b + OFF_CTRL);
    if (!(ctrl & MSG_DONE) ||
        (ctrl & (SCHAN_NACK | SCHAN_TIMEOUT | SCHAN_ERROR | SER_CHECK_FAIL)))
        return -1;
    return 0;
}

/* Phase B -- bring the pipeline blocks out of reset (trident2.c:9792-9812).
 *
 * TOP_SOFT_RESET_REG (block 57 TOP, 0x2030100, allregs_t.i:102966) reads
 * 0x00000000 on a cold chip and every field is an active-low *_RST_L, so every
 * block is held in reset. That is why block 1 answers warm (EOS released it)
 * and times out cold.
 *
 * Field bits for BCM56860 (fields_t.i:25194):
 *   IP 0, EP 1, MMU 2, TS 3, CLP0..CLP7 4..11
 *
 * The SDK does it in two steps with a delay between:
 *   rval = 0; CLP0..7 = 1, TS = 1            -> 0xff8   (port blocks)
 *   read-modify: IP = 1, EP = 1, MMU = 1     -> 0xfff   (pipeline + MMU)
 *
 * This is the first write that changes how the chip behaves, so every access is
 * announced and drained, and each write is read back before moving on.
 */
#define TOP_BLK              57
#define TOP_SOFT_RESET_REG   0x2030100
#define TOP_SOFT_RESET_REG_2 0x2030200

static int phaseB(int cmc)
{
    uint32_t v = 0, got = 0;

    printf("phaseB: releasing pipeline block resets via TOP_SOFT_RESET_REG\n");
    fflush(stdout);
    msleep(300);

    printf("phaseB: READ  TOP_SOFT_RESET_REG (0x%07x) ... ", TOP_SOFT_RESET_REG);
    fflush(stdout);
    msleep(250);
    if (schan_get(cmc, TOP_BLK, TOP_SOFT_RESET_REG, &v)) {
        printf("FAILED -- aborting before any write\n");
        return 3;
    }
    printf("0x%08x\n", v);
    fflush(stdout);
    msleep(250);

    /* Staged deliberately: TS + CLP0-7 first, then IP/EP/MMU below. Do NOT
     * collapse these into one 0xfff write -- that was tried on 2026-08-11 and
     * is a no-op, because the second write already ORs in 0x7. Field map:
     * artifacts/bcm56860-registers.json, TOP_SOFT_RESET_REG, bit 0 IP,
     * 1 EP, 2 MMU, 3 TS, 4-11 CLP0-7. */
    printf("phaseB: WRITE 0x00000ff8 (CLP0-7 + TS out of reset) ... ");
    fflush(stdout);
    msleep(250);
    if (schan_put(cmc, TOP_BLK, TOP_SOFT_RESET_REG, 0xff8)) {
        printf("FAILED\n"); return 3;
    }
    printf("ok\n");
    fflush(stdout);
    msleep(300);

    if (schan_get(cmc, TOP_BLK, TOP_SOFT_RESET_REG, &got) == 0)
        printf("phaseB:   read-back 0x%08x %s\n", got,
               got == 0xff8 ? "ok" : "** unexpected **");
    fflush(stdout);
    msleep(300);

    printf("phaseB: WRITE 0x00000fff (add IP + EP + MMU) ... ");
    fflush(stdout);
    msleep(250);
    if (schan_put(cmc, TOP_BLK, TOP_SOFT_RESET_REG, got | 0x7)) {
        printf("FAILED\n"); return 3;
    }
    printf("ok\n");
    fflush(stdout);
    msleep(300);

    if (schan_get(cmc, TOP_BLK, TOP_SOFT_RESET_REG, &got) == 0)
        printf("phaseB:   read-back 0x%08x %s\n", got,
               got == 0xfff ? "ok" : "** unexpected **");
    fflush(stdout);
    msleep(500);

    printf("phaseB: blocks released. Re-testing block 1 (epic), which times out"
           " cold today.\n");
    fflush(stdout);
    msleep(300);
    return schan_read(cmc, 0x1c10c200, 0x38400000, 4);
}

/* ---- S-Channel memory access ---------------------------------------------
 *
 * READ_MEM_CMD  0x07 -> READ_MEM_ACK  0x08
 * WRITE_MEM_CMD 0x09 -> WRITE_MEM_ACK 0x0a
 *
 * msg[0] = hdr, msg[1] = address, msg[2..] = data.
 * dwc_write = 2 + words for a write; dwc_read = 1 + words for a read.
 *
 * DLEN differs between the two directions, which cost some confusion:
 *   - the captured EOS WRITE_MEM header 0x25200800 decodes to DLEN = 16 for a
 *     4-word entry, i.e. DLEN is the entry size in BYTES
 *   - the captured EOS READ_MEM header used DLEN = 4 for a 48-byte L3_DEFIP
 *     entry, while the returned ACK reported 48
 * So outbound READ DLEN is not the entry size. For a single-word table the two
 * conventions coincide at 4, which is part of why EDB_1DBG_B is a good first
 * write target.
 *
 * Geometry, block id and access type all come from
 * artifacts/bcm56860-memories.json (docs/MEMORY-MAP.md).
 */
static uint32_t mem_hdr(uint32_t opc, int blk, int acc, uint32_t dlen)
{
    return ((opc & 0x3f) << 26) | ((blk & 0x3f) << 20) |
           ((acc & 0x7) << 14) | ((dlen & 0x7f) << 7);
}

/* Status word from the last mem_get failure, so callers can say WHY. */
uint32_t mem_get_last_ctrl;

/*
 * Read a memory.
 *
 * The status bits in SCHAN_CTRL are LATCHED. Until 2026-08-11 this function
 * never cleared them, so a single legitimate NACK -- an uninitialised
 * ECC-protected MMU memory is the obvious source -- made every subsequent
 * read in the same run report failure, whether or not it had actually failed.
 *
 * That is not a theory. In one boot, with identical arguments,
 * `memr 3 0 0x68001000 1` returned 0x01a7bc08 (the exact value EOS has) while
 * `dumpset` reported FAIL for the same address, because the dumpset had
 * already tripped an earlier NACK. It produced 666 phantom "unreadable"
 * registers and invalidated the register-diff metric this repo had been
 * using. See docs/DUMPSET-FAIL-IS-NOT-CHIP-STATE-20260811.md.
 *
 * So: clear the status before starting, and retry once after a settle before
 * declaring failure.
 */
static int mem_get_once(int cmc, int blk, int acc, uint32_t addr,
                        uint32_t *out, int words)
{
    uint32_t b = cmc_base(cmc), ctrl;
    int i;

    /* Clear any latched status from a previous op, then confirm the channel
     * is idle. Order matters -- checking MSG_START first would reject a
     * channel that is merely holding stale status. */
    wr(asic, b + OFF_CTRL, 0);
    if (rd(asic, b + OFF_CTRL) & MSG_START) {
        mem_get_last_ctrl = rd(asic, b + OFF_CTRL);
        return -1;
    }
    wr(asic, b + OFF_MSG0 + 0, mem_hdr(0x07, blk, acc, 4));
    wr(asic, b + OFF_MSG0 + 4, addr);
    wr(asic, b + OFF_CTRL, MSG_START);
    for (i = 0; i < 100000; i++) {
        ctrl = rd(asic, b + OFF_CTRL);
        if (ctrl & MSG_DONE) break;
    }
    ctrl = rd(asic, b + OFF_CTRL);
    mem_get_last_ctrl = ctrl;
    if (!(ctrl & MSG_DONE) ||
        (ctrl & (SCHAN_NACK | SCHAN_TIMEOUT | SCHAN_ERROR | SER_CHECK_FAIL)))
        return -1;
    for (i = 0; i < words; i++)
        out[i] = rd(asic, b + OFF_MSG0 + 4 * (i + 1));
    return 0;
}

/*
 * Recover a wedged S-Channel exactly as the SDK does: toggle the abort bit.
 *
 * Measured 2026-08-11: a 4,074-entry dump produced 1,450 TIMEOUT and 149 NACK
 * failures, and the MMU stopped answering for the rest of the boot. Clearing
 * the latched status was not enough -- a timeout leaves the channel itself
 * stuck, and only the abort toggle frees it.
 */
static void schan_abort(int cmc)
{
    uint32_t b = cmc_base(cmc);
    uint32_t v = rd(asic, b + OFF_CTRL);

    wr(asic, b + OFF_CTRL, v | SCHAN_ABORT);
    wr(asic, b + OFF_CTRL, v);
    wr(asic, b + OFF_CTRL, 0);
}

static int mem_get(int cmc, int blk, int acc, uint32_t addr,
                   uint32_t *out, int words)
{
    if (mem_get_once(cmc, blk, acc, addr, out, words) == 0) return 0;
    /* Recover the channel, then retry once. A read that fails twice is a real
     * failure; a read that fails once and then succeeds was collateral. */
    schan_abort(cmc);
    if (mem_get_once(cmc, blk, acc, addr, out, words) == 0) return 0;
    /* Leave the channel usable for whatever runs next, even on real failure --
     * this is what stopped one bad read from poisoning the whole dump. */
    schan_abort(cmc);
    return -1;
}

/* Decode the latched status for a human. */
const char *mem_get_fail_reason(void)
{
    static char buf[64];
    uint32_t c = mem_get_last_ctrl;

    snprintf(buf, sizeof(buf), "%s%s%s%s%s",
             (c & SCHAN_NACK)     ? "NACK "     : "",
             (c & SCHAN_TIMEOUT)  ? "TIMEOUT "  : "",
             (c & SCHAN_ERROR)    ? "ERROR "    : "",
             (c & SER_CHECK_FAIL) ? "SERFAIL "  : "",
             (c & MSG_DONE)       ? ""          : "NOTDONE ");
    return buf[0] ? buf : "?";
}

static int reg_get(int cmc, int blk, int acc, uint32_t addr,
                   uint32_t *out, int words, uint32_t dlen);
static int reg_put(int cmc, int blk, int acc, uint32_t addr,
                   const uint32_t *data, int words, uint32_t dlen);

/*
 * TSC (SerDes core) reset, ported from the SDK's _soc_xgxs_reset_single_tsc.
 *
 * This is the step our own bring-up never performed, and it is why the TSC
 * lane window does not read: the ablation runs of 2026-08-11 all reported
 *
 *     blk18 lane 8..b SC_X4_RSLVD0 = memr: READ_MEM FAILED
 *
 * so the front-panel ports were dark and no MMU work could have moved a
 * frame. Six earlier hypotheses about that window were refuted by guessing;
 * this sequence is taken from the SDK running on this board, attributed to
 * drv.c:4371-4450 (artifacts/sdk-chipreset-20260811/).
 *
 * One register, 0x02003200, per PGW_CL block 6..13, written with a fixed
 * value sequence. The SDK reads it first each time (drv.c:4371) -- this is a
 * read-modify-write in the original -- so the values below are what the
 * sequence SETTLED on for this chip, not a blind copy of arbitrary data.
 */
struct tsc_w { int blk; uint32_t val; };

static const struct tsc_w tsc_seq[] = {
    {  6, 0x00000030 },
    {  6, 0x00000010 },
    {  6, 0x00000010 },
    {  6, 0x00000014 },
    {  6, 0x00000014 },
    {  6, 0x00000014 },
    {  6, 0x00000010 },
    {  6, 0x00000014 },
    {  7, 0x00000030 },
    {  7, 0x00000010 },
    {  7, 0x00000010 },
    {  7, 0x00000014 },
    {  7, 0x00000014 },
    {  7, 0x00000014 },
    {  7, 0x00000010 },
    {  7, 0x00000014 },
    {  7, 0x00000014 },
    {  7, 0x00000014 },
    {  7, 0x00000010 },
    {  7, 0x00000014 },
    {  7, 0x00000014 },
    {  7, 0x00000014 },
    {  7, 0x00000010 },
    {  7, 0x00000014 },
    {  8, 0x00000030 },
    {  8, 0x00000010 },
    {  8, 0x00000010 },
    {  8, 0x00000014 },
    {  8, 0x00000014 },
    {  8, 0x00000014 },
    {  8, 0x00000010 },
    {  8, 0x00000014 },
    {  9, 0x00000030 },
    {  9, 0x00000010 },
    {  9, 0x00000010 },
    {  9, 0x00000014 },
    {  9, 0x00000014 },
    {  9, 0x00000014 },
    {  9, 0x00000010 },
    {  9, 0x00000014 },
    {  9, 0x00000014 },
    {  9, 0x00000014 },
    {  9, 0x00000010 },
    {  9, 0x00000014 },
    {  9, 0x00000014 },
    {  9, 0x00000014 },
    {  9, 0x00000010 },
    {  9, 0x00000014 },
    { 10, 0x00000030 },
    { 10, 0x00000010 },
    { 10, 0x00000010 },
    { 10, 0x00000014 },
    { 10, 0x00000014 },
    { 10, 0x00000014 },
    { 10, 0x00000010 },
    { 10, 0x00000014 },
    { 10, 0x00000014 },
    { 10, 0x00000014 },
    { 10, 0x00000010 },
    { 10, 0x00000014 },
    { 10, 0x00000014 },
    { 10, 0x00000014 },
    { 10, 0x00000010 },
    { 10, 0x00000014 },
    { 11, 0x00000030 },
    { 11, 0x00000010 },
    { 11, 0x00000010 },
    { 11, 0x00000014 },
    { 11, 0x00000014 },
    { 11, 0x00000014 },
    { 11, 0x00000010 },
    { 11, 0x00000014 },
    { 12, 0x00000030 },
    { 12, 0x00000010 },
    { 12, 0x00000010 },
    { 12, 0x00000014 },
    { 12, 0x00000014 },
    { 12, 0x00000014 },
    { 12, 0x00000010 },
    { 12, 0x00000014 },
    { 12, 0x00000014 },
    { 12, 0x00000014 },
    { 12, 0x00000010 },
    { 12, 0x00000014 },
    { 12, 0x00000014 },
    { 12, 0x00000014 },
    { 12, 0x00000010 },
    { 12, 0x00000014 },
    { 13, 0x00000030 },
    { 13, 0x00000010 },
    { 13, 0x00000010 },
    { 13, 0x00000014 },
    { 13, 0x00000014 },
    { 13, 0x00000014 },
    { 13, 0x00000010 },
    { 13, 0x00000014 },
};

/*
 * tsc_init -- the SDK's _soc_xgxs_reset_single_tsc (drv.c:4361) PORTED, not
 * replayed.
 *
 * tsc_reset() below writes the same values this does. It was verified
 * op-for-op against the SDK and it is still not the same thing, because it
 * writes them back-to-back with NO DELAYS. The SDK's sequence is:
 *
 *     rval = read(reg)                      <- read-modify-write, not a table
 *     REFIN_EN = lcpll(1);  write
 *     PWRDWN   = 0;         write; usleep(1100)
 *     RSTB_HW  = 0;         write; usleep(1100 + 10000)   <-- 11.1 ms
 *     RSTB_HW  = 1;         write; usleep(1100)
 *
 * The 11.1 ms hold while the TSC sits in reset is the step no replay carries:
 * a trace records ops, never the waits between them. REPLAY_DELAY_US applied a
 * uniform 1,100 us and was refuted (SETTLE-REFUTED-20260813.md) -- but a
 * uniform 1.1 ms is 10x too short for THIS step, so that test never actually
 * reproduced this sequence.
 *
 * Fields for PGW_TSCn_CTRL_REG on BCM56860, from fields_p.i:52289 (guarded
 * BCM_56850_A0 || BCM_56860_A0), not inferred:
 *     RSTB_DVT 0, DVT_EN 1, RSTB_HW 2, REFOUT_EN 3, REFIN_EN 4,
 *     PWRDWN 5, IDDQ 6
 * There is no RSTB_REFCLK and no RSTB_PLL field on this chip, so the SDK's
 * two soc_reg_field_valid branches for them do not apply.
 *
 * Registers are PGW_TSC0..3_CTRL_REG at 0x02003200 + idx*0x100, in PGW_CL
 * blocks 6..13 (SERDES-RESET-FOUND.md), which is where the SDK's
 * ctrl_regs_td2plus[] lands for a non-100G TD2+ port (drv.c:4876).
 */
#define TSC_RSTB_HW  (1u << 2)
#define TSC_REFIN_EN (1u << 4)
#define TSC_PWRDWN   (1u << 5)

static int tsc_init(int cmc)
{
    int blk, idx;
    long ok = 0, bad = 0, skipped = 0;
    const char *e = getenv("TSC_HOLD_US");
    unsigned long hold = e ? strtoul(e, NULL, 0) : 11100;
    const char *e2 = getenv("TSC_SETTLE_US");
    unsigned long settle = e2 ? strtoul(e2, NULL, 0) : 1100;

    printf("tscinit: PGW blocks 6-13, TSC0-3, RMW with %lu us settle / "
           "%lu us reset hold\n", settle, hold);
    for (blk = 6; blk <= 13; blk++) {
        for (idx = 0; idx < 4; idx++) {
            uint32_t addr = 0x02003200 + (uint32_t)idx * 0x100;
            uint32_t v;

            if (reg_get(cmc, blk, 0, addr, &v, 1, 4)) { skipped++; continue; }

            v |= TSC_REFIN_EN;                       /* reference clock in */
            if (reg_put(cmc, blk, 0, addr, &v, 1, 4)) { bad++; continue; }

            v &= ~TSC_PWRDWN;                        /* deassert power down */
            if (reg_put(cmc, blk, 0, addr, &v, 1, 4)) { bad++; continue; }
            usleep(settle);

            v &= ~TSC_RSTB_HW;                       /* hold XGXS in reset */
            if (reg_put(cmc, blk, 0, addr, &v, 1, 4)) { bad++; continue; }
            usleep(hold);                            /* THE 11.1 ms */

            v |= TSC_RSTB_HW;                        /* bring it out */
            if (reg_put(cmc, blk, 0, addr, &v, 1, 4)) { bad++; continue; }
            usleep(settle);
            ok++;
        }
    }
    printf("tscinit: DONE ok %ld bad %ld unreadable %ld\n", ok, bad, skipped);
    return bad ? 1 : 0;
}

static int tsc_reset(int cmc)
{
    unsigned i;
    long ok = 0, bad = 0;

    printf("tscreset: %d writes to 0x02003200 across PGW_CL blocks 6-13\n",
           (int)(sizeof(tsc_seq) / sizeof(tsc_seq[0])));
    for (i = 0; i < sizeof(tsc_seq) / sizeof(tsc_seq[0]); i++) {
        uint32_t v = tsc_seq[i].val, got;
        (void)reg_get(cmc, tsc_seq[i].blk, 0, 0x02003200, &got, 1, 4);
        if (reg_put(cmc, tsc_seq[i].blk, 0, 0x02003200, &v, 1, 4)) bad++; else ok++;
    }
    printf("tscreset: DONE ok %ld bad %ld\n", ok, bad);
    return bad ? 1 : 0;
}

/*
 * MMU per-port buffer and threshold configuration.
 *
 * _soc_td2_mmu_config_buf_set_hw_port is the largest MMU block the SDK runs
 * that we do not -- 11,078 operations. In the C it is a computed function over
 * a _soc_mmu_cfg_buf_t of pool sizes and guarantees (trident2.c:14981-15490),
 * but on THIS chip with THIS config it reduces to constant fills: each memory
 * receives one value across a contiguous range of queue entries.
 *
 * The counts decode against the port map: XPIPE is 45 ports x 12 queues = 540,
 * YPIPE is 33 x 12 = 396, matching the per-pipe port counts derived for the
 * LLS tree.
 *
 * Runs extracted from artifacts/sdk-mmu-lls-20260811/attributed-trace.txt.
 * Runs shorter than 8 entries are omitted -- they are one-off register writes,
 * not table fills, and would be guesses.
 *
 * IMPORTANT: this is config for one port map. Regenerate if config.bcm
 * changes. It is NOT a replay of EOS; it is the SDK's own settled state for
 * this configuration, and every value is a single constant per memory rather
 * than an opaque stream.
 */
static int mem_put(int cmc, int blk, int acc, uint32_t addr,
                   const uint32_t *data, int words, uint32_t bytes);

struct thd_run { uint32_t base; int count; uint32_t val; };

static const struct thd_run thd_runs[] = {
    { 0x08000003,    42, 0x0003ffff },
    { 0x08000040,    33, 0x0003ffff },
    { 0x08007000,    45, 0x00ffffff },
    { 0x08007040,    33, 0x00ffffff },
    { 0x08007108,    37, 0x00ffffff },
    { 0x08007140,    33, 0x00ffffff },
    { 0x08007300,    45, 0x00000000 },
    { 0x08007340,    33, 0x00000000 },
    { 0xa8000000,    58, 0x00008000 },
    { 0xa8000046,   470, 0x00008000 },
    { 0xa8001000,    18, 0x00013936 },
    { 0xa8001013,    10, 0x00013936 },
    { 0xa8001025,    20, 0x00013936 },
    { 0xa800103a,    20, 0x00013936 },
    { 0xa800104f,   461, 0x00013936 },
    { 0xa8002000,    22, 0x20008002 },
    { 0xa8002019,    27, 0x20008002 },
    { 0xa8002035,    15, 0x20008002 },
    { 0xa8002045,   471, 0x20008002 },
    { 0xa8100000,   396, 0x00008000 },
    { 0xa8101000,   396, 0x00013936 },
    { 0xa8102000,   396, 0x20008002 },
    { 0xac000000,    10, 0x00013936 },
    { 0xac000011,    28, 0x00013936 },
    { 0xac00002f,    13, 0x00013936 },
    { 0xac00003f,   387, 0x00013936 },
    { 0xac000208,    10, 0x00013936 },
    { 0xac000219,     9, 0x00013936 },
    { 0xac000223,    13, 0x00013936 },
    { 0xac001000,    31, 0x00000002 },
    { 0xac001023,    39, 0x00000002 },
    { 0xac00104b,   375, 0x00000002 },
    { 0xac00120b,     8, 0x00000002 },
    { 0xac001214,    15, 0x00000002 },
    { 0xac001225,    19, 0x00000002 },
    { 0xac006005,    40, 0x4e47392c },
    { 0xac080000,   330, 0x00013936 },
    { 0xac080208,     9, 0x00013936 },
    { 0xac081000,   330, 0x00000002 },
    { 0xac081208,     9, 0x00000002 },
    { 0xac086000,    33, 0x4e47392c },
    { 0xb0000000,    16, 0x0000a5a9 },
    { 0xb0000011,    45, 0x0000a5a9 },
    { 0xb000003f,   387, 0x0000a5a9 },
    { 0xb0000208,    35, 0x0000a5a9 },
    { 0xb0001000,    18, 0x00000001 },
    { 0xb0001015,    12, 0x00000001 },
    { 0xb0001022,    11, 0x00000001 },
    { 0xb000102e,     9, 0x00000001 },
    { 0xb0001045,   381, 0x00000001 },
    { 0xb000120e,    13, 0x00000001 },
    { 0xb000121c,    25, 0x00000001 },
    { 0xb0006000,    45, 0x0969a5a9 },
    { 0xb0080000,   330, 0x0000a5a9 },
    { 0xb0080208,     8, 0x0000a5a9 },
    { 0xb0081000,   330, 0x00000001 },
    { 0xb0081208,     9, 0x00000001 },
    { 0xb0086000,    33, 0x0969a5a9 },
};

static int thd_init(int cmc)
{
    unsigned r;
    long ok = 0, bad = 0;

    printf("thdinit: %d runs of per-port MMU threshold config\n",
           (int)(sizeof(thd_runs) / sizeof(thd_runs[0])));
    for (r = 0; r < sizeof(thd_runs) / sizeof(thd_runs[0]); r++) {
        const struct thd_run *t = &thd_runs[r];
        long rok = 0, rbad = 0;
        int i;

        for (i = 0; i < t->count; i++) {
            uint32_t v = t->val;
            if (mem_put(cmc, 3, 0, t->base + i, &v, 1, 4)) rbad++; else rok++;
        }
        ok += rok; bad += rbad;
        if (rbad) {
            printf("  0x%08x..0x%08x val 0x%08x : ok %ld BAD %ld\n",
                   t->base, t->base + t->count - 1, t->val, rok, rbad);
        }
    }
    printf("thdinit: DONE ok %ld bad %ld\n", ok, bad);
    return bad ? 1 : 0;
}

/*
 * Fan control -- and the fans are NOT on the SCD.
 *
 * Every SCD-side search for fan registers failed because the premise was
 * wrong. EOS's own libInvScd.so is MDIO/transceiver code with no fan content.
 * The fans hang off the AMD southbridge, and Arista's raven-fan-driver.c
 * (GPL, aristanetworks/sonic) gives the exact map:
 *
 *   SB800_BASE       0xfed80000
 *   GPIO   base      +0x0100   presence, LEDs
 *   PM2    base      +0x0400   fan control
 *
 *   PWM   [fan]  = PM2 + 3    + fan * 0x10     byte, 0..255
 *   TACH  [fan]  = PM2 + 0x69 + fan * 0x05     byte lo, then +1 hi
 *                  RPM = 22700 * 60 / (raw * 2)
 *   PRESENT[fan] = GPIO + {206, 212, 220, 224}[fan], present = (~b >> 7) & 1
 *
 * This is our board and not an assumption: the CPU is an AMD GX-424CC SOC
 * (Family 16h) whose FCH puts PM2/GPIO at 0xfed80000, and /sys/class/hwmon/
 * hwmon0 points at 0000:00:18.3 (K10Temp) -- exactly what Arista's own
 * Cloverdale platform file declares alongside RavenFanComplex. The 7151
 * already runs this driver on the same kernel lineage.
 *
 * Done in userspace over /dev/mem rather than as a kernel module because
 * EdgeNOS kexecs EOS's own linux-i386 kernel, so a .ko would have to match
 * that kernel's version and config exactly. This does not.
 *
 * `fanset` clamps to a floor: the whole point of fan control is not to cook
 * the ASIC, and a typo that commands 0 should not be able to stop the air.
 */
#define SB800_PHYS      0xfed80000u
#define SB800_LEN       0x1000u
#define SB800_GPIO_OFF  0x0100u
#define SB800_PM2_OFF   0x0400u
#define FAN_PWM_OFF     3u
#define FAN_PWM_STEP    0x10u
#define FAN_TACH_OFF    0x69u
#define FAN_TACH_STEP   0x05u
#define FAN_COUNT       4
#define FAN_PWM_FLOOR   102     /* 40% -- never command below this */

static const unsigned fan_present_off[FAN_COUNT] = { 206, 212, 220, 224 };

static volatile unsigned char *sb800_map(int writable)
{
    static volatile unsigned char *p;
    static int mapped_rw;
    int fd;

    if (p && (!writable || mapped_rw)) return p;
    if ((fd = open("/dev/mem", (writable ? O_RDWR : O_RDONLY) | O_SYNC)) < 0) {
        perror("open /dev/mem");
        return NULL;
    }
    p = mmap(NULL, SB800_LEN, writable ? (PROT_READ | PROT_WRITE) : PROT_READ,
             MAP_SHARED, fd, SB800_PHYS);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap 0xfed80000"); p = NULL; return NULL; }
    mapped_rw = writable;
    return p;
}

/*
 * Read-only census of an arbitrary physical range, 16 bytes per line, with
 * all-0xff and all-0x00 lines collapsed.
 *
 * Written for one question: which parts of the AMD FCH window at 0xfed80000
 * actually decode? Presence reads at +0x100 are correct while every byte of
 * the PM2 fan block at +0x400 returns 0xff, through the same mmap. Either the
 * fan block is somewhere else on this Family 16h part, or the AcpiMmio range
 * is gated. A dump answers that without writing anything.
 *
 * READ ONLY. Nothing here writes, and blind writes near chipset windows are
 * how this box got reset twice.
 */
static int phys_dump(unsigned long phys, unsigned long len)
{
    unsigned long pagebase = phys & ~0xfffUL, off = phys - (phys & ~0xfffUL);
    unsigned long maplen = ((off + len + 0xfff) & ~0xfffUL);
    volatile unsigned char *m;
    unsigned long i;
    int fd, live = 0, ff = 0, zero = 0;

    if ((fd = open("/dev/mem", O_RDONLY | O_SYNC)) < 0) {
        perror("open /dev/mem"); return 1;
    }
    m = mmap(NULL, maplen, PROT_READ, MAP_SHARED, fd, (off_t)pagebase);
    close(fd);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }

    printf("physdump 0x%08lx..0x%08lx (READ ONLY)\n", phys, phys + len - 1);
    for (i = 0; i < len; i += 16) {
        const volatile unsigned char *r = m + off + i;
        int j, allff = 1, all00 = 1;
        for (j = 0; j < 16; j++) {
            if (r[j] != 0xff) allff = 0;
            if (r[j] != 0x00) all00 = 0;
        }
        if (allff) { ff++; continue; }
        if (all00) { zero++; continue; }
        live++;
        printf("  +0x%03lx ", i);
        for (j = 0; j < 16; j++) printf("%02x ", r[j]);
        printf("\n");
    }
    printf("physdump: %d live line(s), %d all-ff, %d all-00\n", live, ff, zero);
    munmap((void *)m, maplen);
    return 0;
}

static unsigned fan_rpm_from_raw(unsigned raw)
{
    return (22700u * 60u) / ((raw ? raw : 1u) * 2u);
}

static int fan_read(void)
{
    volatile unsigned char *b = sb800_map(0);
    const volatile unsigned char *pm2, *gpio;
    int i;

    if (!b) return 1;
    pm2  = b + SB800_PM2_OFF;
    gpio = b + SB800_GPIO_OFF;

    printf("fans at SB800 0x%08x (PM2 +0x%03x, GPIO +0x%03x)\n",
           SB800_PHYS, SB800_PM2_OFF, SB800_GPIO_OFF);
    for (i = 0; i < FAN_COUNT; i++) {
        unsigned lo, hi, lo2, hi2, raw, pwm, present;
        const volatile unsigned char *t = pm2 + FAN_TACH_OFF + i * FAN_TACH_STEP;

        /* Re-read and prefer the consistent pair; the counter can tick
         * between the two byte reads. Same guard the vendor driver uses. */
        lo = *t; hi = *(t + 1);
        lo2 = *t; hi2 = *(t + 1);
        raw = (lo2 == lo) ? ((hi << 8) | lo) : ((hi2 << 8) | lo2);

        pwm = *(pm2 + FAN_PWM_OFF + i * FAN_PWM_STEP);
        present = ((~(*(gpio + fan_present_off[i]))) >> 7) & 1;

        printf("  fan%d  present=%u  pwm=%3u (%3u%%)  tach=0x%04x  %5u rpm\n",
               i + 1, present, pwm, (pwm * 100) / 255, raw,
               fan_rpm_from_raw(raw));
    }
    return 0;
}

static int fan_set(const char *which, const char *val)
{
    volatile unsigned char *b;
    volatile unsigned char *pm2;
    long pwm = strtol(val, NULL, 0);
    int i, one = -1;

    if (strcmp(which, "all") != 0) {
        one = atoi(which);
        if (one < 1 || one > FAN_COUNT) {
            printf("fanset: fan must be 1..%d or 'all'\n", FAN_COUNT);
            return 1;
        }
    }
    if (pwm < 0 || pwm > 255) { printf("fanset: pwm must be 0..255\n"); return 1; }
    if (pwm < FAN_PWM_FLOOR) {
        printf("fanset: %ld is below the floor, clamping to %d (%d%%) -- "
               "airflow must never stop\n",
               pwm, FAN_PWM_FLOOR, (FAN_PWM_FLOOR * 100) / 255);
        pwm = FAN_PWM_FLOOR;
    }
    if (!(b = sb800_map(1))) return 1;
    pm2 = b + SB800_PM2_OFF;

    for (i = 0; i < FAN_COUNT; i++) {
        if (one > 0 && i != one - 1) continue;
        *(pm2 + FAN_PWM_OFF + i * FAN_PWM_STEP) = (unsigned char)pwm;
        printf("  fan%d pwm <- %ld (%ld%%)\n", i + 1, pwm, (pwm * 100) / 255);
    }
    return 0;
}

/*
 * XLPORT bring-up -- the step that opens the TSC lane window.
 *
 * Eight hypotheses were refuted chasing "blk18 lane 8..b = READ_MEM FAILED".
 * The answer, from the bcm_init capture (docs/TSC-WINDOW-OPENED-20260812.md),
 * is _pm4x10_pm_xlport_init at pm4x10.c:3873, and the first of its three
 * writes is the one that matters:
 *
 *   0x02020a00 <- 0   XLPORT_POWER_SAVE   XPORT_CORE0f = 0   pm4x10.c:3885
 *   0x02020600 <- 0   XLPORT_MODE_REG                        pm4x10.c:3919
 *   <_soc_xgxs_reset_single_tsc -- SerDes out of reset>
 *   0x02020d00 <- 0   XLPORT_MAC_CONTROL  XMAC0_RESET = 0    pm4x10.c:3941
 *
 * The XLPORT core was in POWER SAVE. The block was not mis-reset and its PLLs
 * were not unlocked -- it was asleep, which is why every lane read failed and
 * why no amount of SOC-layer experimentation could find it. The SOC layer
 * touches block 18 exactly once, to write an LED register.
 *
 * Two things this run corrects about tscreset:
 *
 *   - tscreset is RIGHT for its layer. Verified op-for-op against the SDK's
 *     SOC-layer _soc_xgxs_reset_single_tsc, per block, value for value.
 *   - It is INCOMPLETE for the chip. It writes only 0x02003200; the port layer
 *     resets all four TSC instances per PGW_CL block -- 0x02003200, 0x300,
 *     0x400 and 0x500. Those 120 extra writes are included here.
 *
 * So the order is: tscreset (96 SOC-layer writes), then this (210 port-layer
 * writes). They do not overlap; the generator excludes anything tscreset does.
 *
 * NOT included: the 3,380 portmod_common_phy_sbus_reg_read/write lane accesses.
 * Those are read-modify-write against live lane state and a blind replay would
 * be meaningless. This command answers the narrower, checkable question --
 * does the window open?
 *
 * The test is NOT the write count. It is whether
 *     scdreset memw 18 0 0x0 16 0x0068c072 0 0 0 ; scdreset memr 18 0 0x0 4
 * returns 0x00000e05 with no SDK present. We know the right answer now.
 */
#include "xlport-init-data.h"

static int xlport_init(int cmc)
{
    unsigned i;
    long ok = 0, bad = 0;
    int cur_fn = -1, cur_src = -1;
    long s_ok = 0, s_bad = 0;

    printf("xlportinit: %d port-layer writes "
           "(run tscreset first -- its 96 are not repeated here)\n",
           (int)(sizeof(xlport_ops) / sizeof(xlport_ops[0])));

    for (i = 0; i < sizeof(xlport_ops) / sizeof(xlport_ops[0]); i++) {
        const struct xlport_op *x = &xlport_ops[i];
        uint32_t v = x->data;

        if (x->fn != cur_fn || x->src != cur_src) {
            if (cur_fn >= 0)
                printf("  %-28s :%-5d ok %ld bad %ld\n",
                       xlport_fn[cur_fn], cur_src, s_ok, s_bad);
            cur_fn = x->fn; cur_src = x->src;
            s_ok = s_bad = 0;
        }
        if (reg_put(cmc, x->blk, 0, x->addr, &v, 1, x->dlen)) {
            s_bad++; bad++;
        } else {
            s_ok++; ok++;
        }
    }
    if (cur_fn >= 0)
        printf("  %-28s :%-5d ok %ld bad %ld\n",
               xlport_fn[cur_fn], cur_src, s_ok, s_bad);

    printf("xlportinit: DONE ok %ld bad %ld\n", ok, bad);
    return bad ? 1 : 0;
}

/*
 * TDM -- the arbitration calendar. This is the step our bring-up never did.
 *
 * _soc_trident2_tdm_init (trident2.c:10835) is called from
 * _soc_trident2_misc_init (trident2.c:13142), right after port mapping init. It
 * programs, per pipe, which port owns which slot in two calendars:
 *
 *   IARB_MAIN_TDM_X/Y     ingress arbiter, 200 slots   0x74200000..0x742000c7
 *   ES_PIPEn_TDM_TABLE_0  MMU egress,      356 entries 0xbc000000..0xbc000163
 *
 * plus PGW_TDM_CONTROL and the oversubscription spacing/weight registers on
 * every PGW_CL instance. A port with no slot in these tables can link and can
 * even latch a frame at the MAC, but the pipeline never schedules it -- which
 * is exactly the symptom we have been chasing.
 *
 * The data is not hand-derived. It is 1,218 writes lifted in issue order from
 * artifacts/sdk-mmu-lls-20260811/attributed-trace.txt, the SDK running on this
 * board with every op attributed to a trident2.c line. See tools/gen-tdm-init.py
 * for the derivation and the cross-checks, the load-bearing one being that EOS's
 * own inventory records IARB_MAIN_TDM as 200 distinct targets -- the same
 * calendar depth the SDK writes, from a completely separate capture.
 *
 * Caveat worth stating: the calendar the SDK computed depends on the port map it
 * was given, and 168 of our 404 config.bcm lines are still inert (they name
 * ports by index where the SDK wants names). If that port map was wrong, these
 * values are wrong in the same way. Reading the tables back after a run and
 * comparing against EOS's is the check that settles it.
 */
#include "tdm-init-data.h"

static int tdm_init(int cmc)
{
    unsigned i;
    long ok = 0, bad = 0;
    int cur_src = -1;
    long src_ok = 0, src_bad = 0;

    printf("tdminit: %d writes, _soc_trident2_tdm_init in issue order\n",
           (int)(sizeof(tdm_ops) / sizeof(tdm_ops[0])));

    for (i = 0; i < sizeof(tdm_ops) / sizeof(tdm_ops[0]); i++) {
        const struct tdm_op *t = &tdm_ops[i];
        int rv;

        if (t->src != cur_src) {
            if (cur_src >= 0)
                printf("  trident2.c:%-5d ok %ld bad %ld\n",
                       cur_src, src_ok, src_bad);
            cur_src = t->src;
            src_ok = src_bad = 0;
        }

        if (t->op == 9)
            rv = mem_put(cmc, t->blk, t->acc, t->addr, t->data,
                         t->words, t->dlen);
        else
            rv = reg_put(cmc, t->blk, t->acc, t->addr, t->data,
                         t->words, t->dlen);

        if (rv) { src_bad++; bad++; } else { src_ok++; ok++; }
    }
    if (cur_src >= 0)
        printf("  trident2.c:%-5d ok %ld bad %ld\n", cur_src, src_ok, src_bad);

    printf("tdminit: DONE ok %ld bad %ld\n", ok, bad);
    return bad ? 1 : 0;
}

/*
 * Read every address tdminit wrote and compare against what we meant to write.
 *
 * This exists because "ok 1218 bad 0" is not evidence. An S-Channel write that
 * completes without NACK says the bus worked, not that the calendar took, and
 * this project has twice mistaken a green write count for a working chip.
 * Worse, the calendar the SDK computed depends on the port map it was given,
 * and 168 of our 404 config.bcm lines are still inert -- a wrong port map would
 * produce a perfectly clean tdminit run and a useless calendar.
 *
 * Three outcomes per address, reported per trident2.c source line:
 *   match     read back exactly what we wrote
 *   DIFFER    read back something else -- the interesting case, values shown
 *   unread    the read itself failed (write-only or gated register)
 *
 * "unread" is not a failure. Plenty of these are write-only. What matters is
 * that DIFFER is zero and that the two calendars -- IARB_MAIN_TDM at
 * 0x742000xx and the MMU table at 0xbc0000xx -- read back clean, because those
 * are the ones a frame's scheduling actually depends on.
 */
static int tdm_verify(int cmc)
{
    unsigned i;
    long match = 0, differ = 0, unread = 0;
    int cur_src = -1;
    long s_m = 0, s_d = 0, s_u = 0;
    int shown = 0;

    printf("tdmverify: reading back %d addresses\n",
           (int)(sizeof(tdm_ops) / sizeof(tdm_ops[0])));

    for (i = 0; i < sizeof(tdm_ops) / sizeof(tdm_ops[0]); i++) {
        const struct tdm_op *t = &tdm_ops[i];
        uint32_t got[2] = { 0, 0 };
        int rv, j, same = 1;

        if (t->src != cur_src) {
            if (cur_src >= 0)
                printf("  trident2.c:%-5d match %ld DIFFER %ld unread %ld\n",
                       cur_src, s_m, s_d, s_u);
            cur_src = t->src;
            s_m = s_d = s_u = 0;
        }

        if (t->op == 9)
            rv = mem_get(cmc, t->blk, t->acc, t->addr, got, t->words);
        else
            rv = reg_get(cmc, t->blk, t->acc, t->addr, got, t->words, t->dlen);

        if (rv) { s_u++; unread++; continue; }

        for (j = 0; j < t->words; j++)
            if (got[j] != t->data[j]) same = 0;

        if (same) { s_m++; match++; continue; }

        s_d++; differ++;
        if (shown < 20) {
            printf("    DIFFER blk %2d 0x%08x  want %08x %08x  got %08x %08x"
                   "  (trident2.c:%d)\n",
                   t->blk, t->addr, t->data[0], t->data[1], got[0], got[1],
                   t->src);
            shown++;
            if (shown == 20) printf("    ... further differences not shown\n");
        }
    }
    if (cur_src >= 0)
        printf("  trident2.c:%-5d match %ld DIFFER %ld unread %ld\n",
               cur_src, s_m, s_d, s_u);

    printf("tdmverify: DONE match %ld DIFFER %ld unread %ld\n",
           match, differ, unread);
    return differ ? 1 : 0;
}

/*
 * LLS (scheduler) node reset -- our own implementation of soc_td2_lls_reset.
 *
 * The SDK walks every LLS node memory writing an entry whose C_PARENT field is
 * INVALID_PARENT for that level (cosq.c:2242). Captured from the SDK running
 * on this board (artifacts/lls-sequence-20260811/), the traffic is three
 * memories, each two contiguous runs -- X pipe and Y pipe -- with one constant
 * value per memory:
 *
 *   0x68000000..0x6800010f  and  0x6800e000..0x6800e10f   272/pipe  0x0000002d
 *   0x6c000000..0x6c0003ff  and  0x6c00e000..0x6c00e3ff  1024/pipe  0x0000010f
 *   0x70000000..0x700007ff  and  0x70009000..0x700097ff  2048/pipe  0x000003ff
 *
 * 2 * (272 + 1024 + 2048) = 6688, which is exactly the op count captured.
 *
 * This is NOT a replay of EOS's blob. Three attempts at that failed -- name
 * filter, block-3 slice, and raw final state -- because the capture carried no
 * structure. This is a loop with the values read off the vendor driver's own
 * behaviour, and it can be checked against cosq.c.
 */
struct lls_run { uint32_t base; int count; uint32_t val; };

static const struct lls_run lls_runs[] = {
    { 0x68000000, 272,  0x0000002d },
    { 0x6800e000, 272,  0x0000002d },
    { 0x6c000000, 1024, 0x0000010f },
    { 0x6c00e000, 1024, 0x0000010f },
    { 0x70000000, 2048, 0x000003ff },
    { 0x70009000, 2048, 0x000003ff },
};

/*
 * The whole of soc_td2_lls_init (cosq.c:2355-2404), in issue order.
 *
 * Captured from the SDK on this board: seven operations around the node reset.
 *
 *   1. dummy read ES_PIPE0_LLS_L0_PARENT[0]  0x68000000   errata TD2-3313
 *   2. dummy read ES_PIPE1_LLS_L0_PARENT[0]  0x6800e000
 *   3. HSP_SCHED_GLOBAL_CONFIG  0x96080000 = 0
 *   4. soc_td2_lls_reset -- 6,688 node writes
 *   5. ES_PIPE0_LLS_FC_CONFIG   0x62000400 = 0
 *   6. ES_PIPE1_LLS_FC_CONFIG   0x63000300 = 8
 *
 * The dummy reads are not decoration. cosq.c:2357 calls them out as a
 * workaround for TD2-3313, so they run before anything else touches the
 * scheduler.
 */
static int lls_reset(int cmc);

static int lls_init(int cmc)
{
    uint32_t v, got;
    int rv = 0;

    printf("llsinit: 1-2. dummy reads of L0_PARENT (errata TD2-3313)\n");
    printf("  ES_PIPE0 0x68000000: %s\n",
           mem_get(cmc, 3, 0, 0x68000000, &got, 1) ? "FAILED" : "ok");
    printf("  ES_PIPE1 0x6800e000: %s\n",
           mem_get(cmc, 3, 0, 0x6800e000, &got, 1) ? "FAILED" : "ok");

    printf("llsinit: 3. HSP_SCHED_GLOBAL_CONFIG = 0\n");
    v = 0;
    if (reg_put(cmc, 3, 0, 0x96080000, &v, 1, 4)) { printf("  FAILED\n"); rv = 1; }

    printf("llsinit: 4. node reset\n");
    if (lls_reset(cmc)) rv = 1;

    printf("llsinit: 5-6. LLS_FC_CONFIG per pipe\n");
    v = 0;
    if (reg_put(cmc, 3, 0, 0x62000400, &v, 1, 4)) { printf("  PIPE0 FAILED\n"); rv = 1; }
    v = 8;
    if (reg_put(cmc, 3, 0, 0x63000300, &v, 1, 4)) { printf("  PIPE1 FAILED\n"); rv = 1; }

    printf("llsinit: %s\n", rv ? "COMPLETED WITH FAILURES" : "ok");
    return rv;
}

static int lls_reset(int cmc)
{
    unsigned r;
    long ok = 0, bad = 0;

    printf("llsreset: %d runs, MMU block 3, one constant per memory\n",
           (int)(sizeof(lls_runs) / sizeof(lls_runs[0])));
    for (r = 0; r < sizeof(lls_runs) / sizeof(lls_runs[0]); r++) {
        const struct lls_run *lr = &lls_runs[r];
        long rok = 0, rbad = 0;
        int i;

        for (i = 0; i < lr->count; i++) {
            uint32_t v = lr->val;
            if (mem_put(cmc, 3, 0, lr->base + i, &v, 1, 4)) rbad++; else rok++;
        }
        printf("  0x%08x..0x%08x val 0x%08x : ok %ld bad %ld\n",
               lr->base, lr->base + lr->count - 1, lr->val, rok, rbad);
        fflush(stdout);
        ok += rok; bad += rbad;
    }
    printf("llsreset: DONE ok %ld bad %ld (expect 6688 total)\n", ok, bad);
    return bad ? 1 : 0;
}

/*
 * READ a register, as opposed to a memory.
 *
 * Block 3 (MMU) carries both: EOS's own capture shows 89,349 writes at
 * opcode 9 (WRITE_MEM) and 22,063 at opcode 13 (WRITE_REG) on that block
 * alone. They are different S-Channel operations and the chip does not
 * accept a memory read against a register -- it answers READ_MEM FAILED.
 *
 * That cost two wrong conclusions before it was noticed. `memr` has no dlen
 * argument and always issues READ_MEM_CMD (0x07), so every "the register
 * reads back zero" observation taken with it is worthless: XLPORT_SOFT_RESET
 * was declared write-only or self-clearing on exactly that basis, and
 * MMU_GCFG_MISCCONFIG was declared unreachable at an address EOS demonstrably
 * writes.
 *
 * Register writes were never affected -- memw takes an explicit dlen and
 * WRITE_MEM against a register is accepted by the chip, which is why the CL72
 * and gap replays landed.
 *
 * Opcodes are from the chip's own table in artifacts/bcm-registers-all.json:
 * READ_REG_CMD 0x0b, READ_REG_ACK 0x0c.
 */
static int reg_get(int cmc, int blk, int acc, uint32_t addr,
                   uint32_t *out, int words, uint32_t dlen)
{
    uint32_t b = cmc_base(cmc), ctrl;
    int i;

    if (rd(asic, b + OFF_CTRL) & MSG_START) return -1;
    wr(asic, b + OFF_MSG0 + 0, mem_hdr(0x0b, blk, acc, dlen));
    wr(asic, b + OFF_MSG0 + 4, addr);
    wr(asic, b + OFF_CTRL, MSG_START);
    for (i = 0; i < 100000; i++) {
        ctrl = rd(asic, b + OFF_CTRL);
        if (ctrl & MSG_DONE) break;
    }
    ctrl = rd(asic, b + OFF_CTRL);
    if (!(ctrl & MSG_DONE) ||
        (ctrl & (SCHAN_NACK | SCHAN_TIMEOUT | SCHAN_ERROR | SER_CHECK_FAIL)))
        return -1;
    for (i = 0; i < words; i++)
        out[i] = rd(asic, b + OFF_MSG0 + 4 * (i + 1));
    return 0;
}

static int mem_put(int cmc, int blk, int acc, uint32_t addr,
                   const uint32_t *data, int words, uint32_t bytes)
{
    uint32_t b = cmc_base(cmc), ctrl;
    int i;

    if (rd(asic, b + OFF_CTRL) & MSG_START) return -1;
    wr(asic, b + OFF_MSG0 + 0, mem_hdr(0x09, blk, acc, bytes));
    wr(asic, b + OFF_MSG0 + 4, addr);
    for (i = 0; i < words; i++)
        wr(asic, b + OFF_MSG0 + 8 + 4 * i, data[i]);
    wr(asic, b + OFF_CTRL, MSG_START);
    for (i = 0; i < 100000; i++) {
        ctrl = rd(asic, b + OFF_CTRL);
        if (ctrl & MSG_DONE) break;
    }
    ctrl = rd(asic, b + OFF_CTRL);
    if (!(ctrl & MSG_DONE) ||
        (ctrl & (SCHAN_NACK | SCHAN_TIMEOUT | SCHAN_ERROR | SER_CHECK_FAIL)))
        return -1;
    return 0;
}

/* Non-destructive memory round trip, same shape as schan-wtest:
 * read original -> write pattern -> verify -> restore -> confirm.
 * Aborts before writing anything if the initial read fails.
 *
 * Multi-word capable, which is the case that actually matters: a single-word
 * write leaves both dwc_write and DLEN untested, because the read and write
 * DLEN conventions coincide at one word. */
#define MEM_MAX_WORDS 8

static void mem_show(const char *tag, const uint32_t *v, int words)
{
    int i;
    printf("memwtest: %-10s", tag);
    for (i = 0; i < words; i++) printf(" 0x%08x", v[i]);
    printf("\n");
    fflush(stdout);
}

static int mem_wtest(int cmc, int blk, int acc, uint32_t addr,
                     const uint32_t *pattern, int words, uint32_t bytes)
{
    uint32_t orig[MEM_MAX_WORDS], got[MEM_MAX_WORDS], back[MEM_MAX_WORDS];
    int i, match = 1;

    printf("memwtest: blk %d acc %d addr 0x%08x words %d dlen %u\n",
           blk, acc, addr, words, bytes);
    fflush(stdout);
    msleep(250);

    if (mem_get(cmc, blk, acc, addr, orig, words)) {
        printf("memwtest: READ_MEM failed -- aborting before any write\n");
        return 3;
    }
    mem_show("original", orig, words);
    mem_show("pattern", pattern, words);
    msleep(250);

    printf("memwtest: WRITE_MEM ... ");
    fflush(stdout);
    msleep(250);
    if (mem_put(cmc, blk, acc, addr, pattern, words, bytes)) {
        printf("FAILED -- nothing to restore\n");
        return 3;
    }
    printf("ok\n");
    fflush(stdout);
    msleep(250);

    if (mem_get(cmc, blk, acc, addr, got, words)) {
        printf("memwtest: read-back FAILED after write\n");
        return 3;
    }
    mem_show("read-back", got, words);
    for (i = 0; i < words; i++) if (got[i] != pattern[i]) match = 0;
    printf("memwtest: %s\n", match
           ? "** ALL WORDS MATCH -- WRITE_MEM WORKS **"
           : "mismatch (masked field, RO bits, or DLEN not rounded up to a "
             "whole word)");
    fflush(stdout);
    msleep(250);

    printf("memwtest: restoring ... ");
    fflush(stdout);
    msleep(250);
    if (mem_put(cmc, blk, acc, addr, orig, words, bytes)) {
        printf("** RESTORE FAILED **\n");
        mem_show("left at", got, words);
        return 3;
    }
    printf("ok\n");
    if (mem_get(cmc, blk, acc, addr, back, words) == 0) {
        int ok = 1;
        for (i = 0; i < words; i++) if (back[i] != orig[i]) ok = 0;
        mem_show("restored", back, words);
        printf("memwtest: %s\n", ok ? "restored cleanly" : "** NOT RESTORED **");
    }
    return match ? 0 : 1;
}

/* ---- replay -------------------------------------------------------------
 *
 * Re-issue a captured write sequence. The sequence comes from EOS's own
 * S-Channel trace (docs/BSL-TRACE.md): one CLI command makes EOS log every
 * transaction it performs, and bouncing the Strata agent gets a complete chip
 * initialisation -- 409,516 writes, in order, with block, access type, address
 * and data. `tools/make-replay.py` packs that into the blob this reads.
 *
 * Why replay rather than port the SDK: `_soc_trident2_misc_init`,
 * `_soc_trident2_tdm_init` and `_soc_trident2_mmu_init` are thousands of lines
 * and 232,572 of the writes go to the MMU alone. Replaying what the working
 * driver actually did is the same method that cracked the ring map, the DCB
 * format and the CPU-TX module header.
 *
 * It is a blind replay: reads are dropped, so anything EOS decided from a
 * read-back is frozen at whatever it saw on that boot. That is a real limit,
 * not a detail -- but chip init is overwhelmingly static configuration, and
 * the failure mode is visible (a write NACKs, or the loop still drops frames).
 *
 * Header shape is identical for both opcodes: msg[0] = header,
 * msg[1] = address, msg[2..] = data. WRITE_REG simply always has one word.
 */
#define REPLAY_MAX_WORDS 24
#define REPLAY_MAGIC "TD2RPL01"

static int schan_write_raw(int cmc, uint32_t opc, int blk, int acc,
                           uint32_t addr, const uint32_t *data, int words,
                           uint32_t dlen)
{
    uint32_t b = cmc_base(cmc), ctrl;
    int i;

    if (rd(asic, b + OFF_CTRL) & MSG_START) return -1;
    wr(asic, b + OFF_MSG0 + 0, mem_hdr(opc, blk, acc, dlen));
    wr(asic, b + OFF_MSG0 + 4, addr);
    for (i = 0; i < words; i++)
        wr(asic, b + OFF_MSG0 + 8 + 4 * i, data[i]);
    wr(asic, b + OFF_CTRL, MSG_START);
    for (i = 0; i < 100000; i++) {
        ctrl = rd(asic, b + OFF_CTRL);
        if (ctrl & MSG_DONE) break;
    }
    ctrl = rd(asic, b + OFF_CTRL);
    if (!(ctrl & MSG_DONE) ||
        (ctrl & (SCHAN_NACK | SCHAN_TIMEOUT | SCHAN_ERROR | SER_CHECK_FAIL)))
        return -1;
    return 0;
}

static unsigned long replay_delay_us, replay_delay_blk;

static int do_replay(const char *path, int cmc)
{
    unsigned long from = 0, limit = 0, shown = 0, ok = 0, bad = 0, n = 0;
    unsigned long progress = 20000, skipblk = 64;
    /* A failing write spins the 100k-iteration poll to its end, so a wedged
     * chip would turn 409k records into hours rather than seconds. Bail out
     * after a run of consecutive failures -- by then the answer is in. */
    unsigned long maxfail = 200, consec = 0;
    const unsigned char *p, *end;
    unsigned char *buf;
    long size;
    uint32_t count;
    time_t t0, t1;
    FILE *f;
    const char *e;

    if ((e = getenv("REPLAY_FROM"))) from = strtoul(e, NULL, 0);
    if ((e = getenv("REPLAY_MAX"))) limit = strtoul(e, NULL, 0);
    if ((e = getenv("REPLAY_PROGRESS"))) progress = strtoul(e, NULL, 0);
    if ((e = getenv("REPLAY_SKIP_BLK"))) skipblk = strtoul(e, NULL, 0);
    if ((e = getenv("REPLAY_MAX_FAIL"))) maxfail = strtoul(e, NULL, 0);

    /*
     * REPLAY_DELAY_US -- the thing a replay cannot inherit from a trace.
     *
     * A trace records the ops the SDK issued, never the time it waited
     * between them. `_soc_xgxs_reset_single_tsc` (drv.c:4362) declares
     *
     *     int sleep_usec = SAL_BOOT_QUICKTURN ? 500000 : 1100;
     *
     * and calls sal_usleep(sleep_usec) immediately after clearing PWRDWN and
     * IDDQ on the macro -- an 1,100 us settle at exactly the point the lane
     * window opens (the attributed trace puts _soc_xgxs_reset_single_tsc and
     * _pm4x10_pm_xlport_init at op ~70,550). Our replays land every write and
     * wait for none of them: 555,552 ops in 2 seconds.
     *
     * So this is not "add delays and see". It is one documented sleep, with a
     * value taken from the SDK rather than guessed.
     *
     * REPLAY_DELAY_BLK restricts the wait to one destination block, which is
     * what makes the run affordable: delaying every one of 70,795 ops by
     * 1.1 ms costs 78 s, but the ops that matter are the port-block ones.
     */
    {
        unsigned long dus = 0, dblk = 0xffffffff;
        if ((e = getenv("REPLAY_DELAY_US"))) dus = strtoul(e, NULL, 0);
        if ((e = getenv("REPLAY_DELAY_BLK"))) dblk = strtoul(e, NULL, 0);
        replay_delay_us = dus;
        replay_delay_blk = dblk;
        if (dus) {
            printf("replay: settle %lu us before each op (== after the "
                   "previous one, which is where the SDK's sleep sits)", dus);
            if (dblk != 0xffffffff) printf(" on block %lu only", dblk);
            printf("\n");
        }
    }

    if (!(f = fopen(path, "rb"))) { perror(path); return 1; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size < 16) { printf("replay: file too small\n"); fclose(f); return 1; }
    if (!(buf = malloc((size_t)size))) { printf("replay: OOM\n"); fclose(f); return 1; }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        printf("replay: short read\n"); fclose(f); free(buf); return 1;
    }
    fclose(f);
    if (memcmp(buf, REPLAY_MAGIC, 8)) {
        printf("replay: bad magic\n"); free(buf); return 1;
    }
    memcpy(&count, buf + 8, 4);
    printf("replay: %s, %u records, %ld bytes\n", path, count, size);
    printf("replay: from %lu, limit %lu, skip block %lu\n", from, limit, skipblk);
    fflush(stdout);

    p = buf + 16;
    end = buf + size;
    t0 = time(NULL);
    while (p + 12 <= end) {
        uint32_t data[REPLAY_MAX_WORDS], dlen, addr;
        unsigned opc = p[0], blk = p[1], acc = p[2], words = p[3];

        memcpy(&dlen, p + 4, 4);
        memcpy(&addr, p + 8, 4);
        p += 12;
        if (words > REPLAY_MAX_WORDS) { printf("replay: bad record at %lu\n", n); break; }
        if (p + 4 * words > end) break;
        memcpy(data, p, 4 * words);
        p += 4 * words;

        if (n++ < from) continue;
        if (limit && (n - from) > limit) break;
        if (blk == skipblk) continue;

        /* Reads are replayed too when the blob carries them (opcodes 7 and
         * 11). The values are discarded -- the point is that some sequences
         * are handshakes, where the driver's read is what lets the hardware
         * advance, and a write-only replay silently skips those beats. */
        if (opc == 0x07 || opc == 0x0b) {
            uint32_t got[REPLAY_MAX_WORDS];
            int rw = words ? (int)words : 1;
            if (rw > REPLAY_MAX_WORDS) rw = REPLAY_MAX_WORDS;
            if (mem_get(cmc, blk, acc, addr, got, rw)) {
                bad++;
                if (shown++ < 12)
                    printf("replay: READ FAIL rec %lu blk %u acc %u addr 0x%08x\n",
                           n - 1, blk, acc, addr);
                if (maxfail && ++consec >= maxfail) {
                    printf("replay: %lu consecutive failures at record %lu -- "
                           "aborting\n", consec, n - 1);
                    break;
                }
            } else {
                ok++;
                consec = 0;
            }
            if (progress && ((n - from) % progress) == 0) {
                printf("replay: %lu records, ok %lu bad %lu, %lds\n",
                       n - from, ok, bad, (long)(time(NULL) - t0));
                fflush(stdout);
            }
            continue;
        }
        if (replay_delay_us &&
            (replay_delay_blk == 0xffffffff || blk == replay_delay_blk)) {
            usleep(replay_delay_us);
        }
        if (schan_write_raw(cmc, opc, blk, acc, addr, data, (int)words, dlen)) {
            bad++;
            /* The first few failures are the ones that explain the rest;
             * printing all of them on a 400k-record run would bury them. */
            if (shown++ < 12)
                printf("replay: FAIL rec %lu  opc %u blk %u acc %u dlen %u "
                       "addr 0x%08x words %u\n",
                       n - 1, opc, blk, acc, dlen, addr, words);
            if (getenv("REPLAY_STOP_ON_ERR")) {
                printf("replay: stopping at record %lu\n", n - 1);
                break;
            }
            if (maxfail && ++consec >= maxfail) {
                printf("replay: %lu consecutive failures at record %lu -- "
                       "aborting\n", consec, n - 1);
                break;
            }
        } else {
            ok++;
            consec = 0;
        }
        if (progress && ((n - from) % progress) == 0) {
            printf("replay: %lu records, ok %lu bad %lu, %lds\n",
                   n - from, ok, bad, (long)(time(NULL) - t0));
            fflush(stdout);
        }
    }
    t1 = time(NULL);
    printf("replay: DONE %lu attempted, ok %lu, failed %lu, %ld seconds\n",
           ok + bad, ok, bad, (long)(t1 - t0));
    free(buf);
    return bad ? 1 : 0;
}

/* Dump a list of table entries in one process.
 *
 * The question this exists for: the replay wrote everything EOS wrote, and the
 * loop still drops frames, so the remaining difference is a difference in chip
 * STATE, not in the operation log. Comparing state means reading a few hundred
 * entries from a running EOS and the same few hundred from our chip, which is
 * one transaction each and hopeless one process at a time.
 *
 * Input lines: <label> <blk> <acc> <addr> <words>   (# and blank lines ignored)
 * Output:      <label> <blk> <acc> <addr> : <w0> <w1> ...   or  FAIL
 */
static int do_dumpset(const char *path, int cmc)
{
    char line[512];
    unsigned long n = 0, bad = 0;
    FILE *f = fopen(path, "r");

    if (!f) { perror(path); return 1; }
    while (fgets(line, sizeof line, f)) {
        char label[128];
        uint32_t v[REPLAY_MAX_WORDS], addr;
        int blk, acc, words, i;

        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%127s %d %d %x %d", label, &blk, &acc, &addr,
                   &words) != 5)
            continue;
        if (words < 1 || words > REPLAY_MAX_WORDS) continue;
        n++;
        if (mem_get(cmc, blk, acc, addr, v, words)) {
            /* Say WHY. A bare "FAIL" was read as "the chip is dead" for
             * weeks; it usually means the previous read NACKed. */
            printf("%s %d %d 0x%08x : FAIL %s(ctrl 0x%08x)\n",
                   label, blk, acc, addr, mem_get_fail_reason(),
                   mem_get_last_ctrl);
            bad++;
            continue;
        }
        printf("%s %d %d 0x%08x :", label, blk, acc, addr);
        for (i = 0; i < words; i++) printf(" %08x", v[i]);
        printf("\n");
    }
    fclose(f);
    fprintf(stderr, "dumpset: %lu entries, %lu failed\n", n, bad);
    return 0;
}

/* Plain table read and write, no save/restore.
 *
 * memwtest exists to prove the transport and always puts the entry back, which
 * is what you want when the question is "does WRITE_MEM work". It is the wrong
 * tool for bringing a chip up, where the whole point is that the value STAYS.
 * These two are that tool: memr to look, memw to set and confirm.
 *
 * dlen for a write is the entry size rounded UP to a whole number of words
 * (docs/TABLE-ACCESS-COMPLETE.md) -- the memory map emits it as dlen_write.
 * Passing the descriptor's byte count instead silently drops the last word
 * while reporting success. */
/*
 * WRITE a register. Counterpart to reg_get, and needed for the same reason.
 *
 * memw issues WRITE_MEM_CMD (0x09). Some registers accept that -- the CL72 and
 * gap replays wrote plenty of them successfully that way -- but MMU block 3
 * registers do not: MMU_GCFG_MISCCONFIG answers WRITE_MEM FAILED, while
 * reg_get reads it back cleanly at the same address. WRITE_REG_CMD is 0x0d,
 * from the chip's own opcode table.
 *
 * No read-back verification here, deliberately. mem_write_cmd compares the
 * read-back against the word it was handed and calls a mismatch a failure,
 * which is wrong for masked read-modify-write registers and produced 311 bogus
 * "failed" lines in the CL72 replay. Read separately with regr if you want to
 * check.
 */
static int reg_put(int cmc, int blk, int acc, uint32_t addr,
                   const uint32_t *data, int words, uint32_t dlen)
{
    uint32_t b = cmc_base(cmc), ctrl;
    int i;

    if (rd(asic, b + OFF_CTRL) & MSG_START) return -1;
    wr(asic, b + OFF_MSG0 + 0, mem_hdr(0x0d, blk, acc, dlen));
    wr(asic, b + OFF_MSG0 + 4, addr);
    for (i = 0; i < words; i++)
        wr(asic, b + OFF_MSG0 + 8 + 4 * i, data[i]);
    wr(asic, b + OFF_CTRL, MSG_START);
    for (i = 0; i < 100000; i++) {
        ctrl = rd(asic, b + OFF_CTRL);
        if (ctrl & MSG_DONE) break;
    }
    ctrl = rd(asic, b + OFF_CTRL);
    if (!(ctrl & MSG_DONE) ||
        (ctrl & (SCHAN_NACK | SCHAN_TIMEOUT | SCHAN_ERROR | SER_CHECK_FAIL)))
        return -1;
    return 0;
}

static int reg_write_cmd(int cmc, int blk, int acc, uint32_t addr,
                         const uint32_t *val, int words, uint32_t dlen)
{
    int i;
    printf("regw: blk %d acc %d addr 0x%08x dlen %u words %d <-",
           blk, acc, addr, dlen, words);
    for (i = 0; i < words; i++) printf(" 0x%08x", val[i]);
    printf("\n");
    if (reg_put(cmc, blk, acc, addr, val, words, dlen)) {
        printf("regw: WRITE_REG FAILED\n");
        return 1;
    }
    printf("regw: issued\n");
    return 0;
}

/*
 * Issue an arbitrary S-Channel command and report what comes back.
 *
 * Everything else in this file speaks four opcodes -- READ_MEM 0x07,
 * WRITE_MEM 0x09, READ_REG 0x0b, WRITE_REG 0x0d -- but the chip's own opcode
 * table (artifacts/bcm-registers-all.json) lists commands that are not
 * reads or writes at all. The one that matters here is 21 (0x15) INIT_CFAP:
 * a dedicated command to build the cell free address pool.
 *
 * That fits the evidence better than anything else tried. Nothing in the SDK
 * writes MMU_CFAP_BANK*, toggling MMU_GCFG_MISCCONFIG.INIT_MEM leaves the banks
 * empty, and a COMMAND could never appear in our capture because
 * schanboot-writes.csv.gz is a writes-only extract -- its opcode census is
 * exclusively 9 and 13.
 *
 * Generic rather than an initcfap wrapper, because the hypothesis may need
 * variants (with/without a payload, different block) and each is one argument
 * change rather than a rebuild.
 */
static int schan_cmd(int cmc, uint32_t opc, int blk, int acc, uint32_t dlen,
                     const uint32_t *data, int words, int rwords)
{
    uint32_t b = cmc_base(cmc), ctrl;
    int i;

    printf("schancmd: opc 0x%02x blk %d acc %d dlen %u words %d\n",
           opc, blk, acc, dlen, words);
    if (rd(asic, b + OFF_CTRL) & MSG_START) {
        printf("schancmd: channel busy\n");
        return 1;
    }
    wr(asic, b + OFF_MSG0 + 0, mem_hdr(opc, blk, acc, dlen));
    for (i = 0; i < words; i++)
        wr(asic, b + OFF_MSG0 + 4 * (i + 1), data[i]);
    wr(asic, b + OFF_CTRL, MSG_START);
    for (i = 0; i < 100000; i++) {
        ctrl = rd(asic, b + OFF_CTRL);
        if (ctrl & MSG_DONE) break;
    }
    ctrl = rd(asic, b + OFF_CTRL);
    printf("schancmd: CTRL 0x%08x%s%s%s%s%s\n", ctrl,
           (ctrl & MSG_DONE)        ? " DONE"    : " NO-DONE",
           (ctrl & SCHAN_NACK)      ? " NACK"    : "",
           (ctrl & SCHAN_TIMEOUT)   ? " TIMEOUT" : "",
           (ctrl & SCHAN_ERROR)     ? " ERROR"   : "",
           (ctrl & SER_CHECK_FAIL)  ? " SERFAIL" : "");
    if (rwords > 0) {
        printf("schancmd: reply");
        for (i = 0; i < rwords && i < MEM_MAX_WORDS; i++)
            printf(" 0x%08x", rd(asic, b + OFF_MSG0 + 4 * i));
        printf("\n");
    }
    if (!(ctrl & MSG_DONE) ||
        (ctrl & (SCHAN_NACK | SCHAN_TIMEOUT | SCHAN_ERROR | SER_CHECK_FAIL)))
        return 1;
    return 0;
}

static int reg_read_cmd(int cmc, int blk, int acc, uint32_t addr,
                        uint32_t dlen, int words)
{
    uint32_t v[MEM_MAX_WORDS];
    int i;

    if (words > MEM_MAX_WORDS) words = MEM_MAX_WORDS;
    printf("regr: blk %d acc %d addr 0x%08x dlen %u words %d\n",
           blk, acc, addr, dlen, words);
    if (reg_get(cmc, blk, acc, addr, v, words, dlen)) {
        printf("regr: READ_REG FAILED\n");
        return 1;
    }
    printf("regr: ");
    for (i = 0; i < words; i++) printf(" 0x%08x", v[i]);
    printf("\n");
    /*
     * A read that the chip never answered leaves our own address word sitting
     * in MSG1, and we hand it back as if it were data -- with MSG_DONE set and
     * no error bit. It looks exactly like a successful read of a plausible
     * value.
     *
     * That is not hypothetical: CFAPCONFIG and CFAPINIT, addressed with the
     * absolute addresses from bcm-registers-all.json, both "read" as their own
     * address. So did 0x0a000000 on blocks 1, 2 and 5 during an earlier block
     * sweep. The genuine read of MMU_GCFG_MISCCONFIG on block 3 returned
     * 0x00000000, i.e. NOT the address, which is how we know that one was real.
     *
     * Flag it rather than silently returning it.
     */
    if (words > 0 && v[0] == addr)
        printf("regr: ** SUSPECT: value == address. The chip likely did not "
               "answer and this is our own address word echoed back, not data. "
               "Check the block and address encoding.\n");
    return 0;
}

static int mem_read_cmd(int cmc, int blk, int acc, uint32_t addr, int words)
{
    uint32_t v[MEM_MAX_WORDS];
    int i;

    if (words > MEM_MAX_WORDS) words = MEM_MAX_WORDS;
    printf("memr: blk %d acc %d addr 0x%08x words %d\n", blk, acc, addr, words);
    if (mem_get(cmc, blk, acc, addr, v, words)) {
        printf("memr: READ_MEM FAILED\n");
        return 1;
    }
    printf("memr: ");
    for (i = 0; i < words; i++) printf(" 0x%08x", v[i]);
    printf("\n");
    return 0;
}

static int mem_write_cmd(int cmc, int blk, int acc, uint32_t addr,
                         const uint32_t *val, int words, uint32_t bytes)
{
    uint32_t back[MEM_MAX_WORDS];
    int i, ok = 1;

    printf("memw: blk %d acc %d addr 0x%08x words %d dlen %u\n",
           blk, acc, addr, words, bytes);
    mem_show("writing", val, words);
    if (mem_put(cmc, blk, acc, addr, val, words, bytes)) {
        printf("memw: WRITE_MEM FAILED\n");
        return 1;
    }
    if (mem_get(cmc, blk, acc, addr, back, words)) {
        printf("memw: written, but read-back FAILED\n");
        return 1;
    }
    mem_show("read-back", back, words);
    for (i = 0; i < words; i++) if (back[i] != val[i]) ok = 0;
    printf("memw: %s\n", ok ? "matches" :
           "differs (masked field, RO bits, or dlen not rounded up)");
    return ok ? 0 : 1;
}

/* ---- port register capture ------------------------------------------------
 *
 * Et1 is port 1 / SerDes lane 0, 10G SFI (docs/TRANSCEIVER-Et1.md), and under
 * EOS it is a live routed link holding an OSPF adjacency. That makes it the
 * ideal subject for the method that has worked all day: capture register state
 * on a chip where the link is UP (warm inherit from EOS), capture the same
 * registers on our cold-initialised chip, and diff. The difference IS the port
 * bring-up specification -- recovered from working silicon rather than
 * reconstructed from SDK source.
 *
 * Addresses from allregs_x.i for BCM56860; there are only 35 XLPORT/XLMAC
 * registers on this part. XLPORT instance 0 is S-Channel block 15.
 */
static const struct { const char *name; uint32_t addr; } PORTREGS[] = {
    /* XLPORT-level: these answer on a cold chip. */
    { "XLPORT_MODE_REG",    0x2020600 },
    { "XLPORT_MAC_CONTROL", 0x2020d00 },
    { "XLPORT_POWER_SAVE",  0x2020a00 },  /* bit 0 XPORT_CORE0 -- powers the core */
    { "XLPORT_EEE_CLOCK_GATE", 0x2020b00 },
    { "XLPORT_TSC_PLL_LOCK", 0x2020f00 },  /* bit 0 CURRENT (RO), bit 1 LOST */
    /* XLMAC-level: all of these time out cold. VERSION_ID is the cleanest
     * liveness probe -- read-only constant when the block is clocked. */
    { "XLMAC_VERSION_ID",   0x0063500 },
    { "XLMAC_CTRL",         0x0060000 },
    { "XLMAC_TX_CTRL",      0x0060400 },
    { "XLMAC_RX_CTRL",      0x0060600 },
    { "XLMAC_RX_LSS_CTRL",  0x0060a00 },
    { "XLMAC_PAUSE_CTRL",   0x0060d00 },
};

/* schan_get builds its header with ACC = 0, which is what these registers use. */
static int port_dump(int cmc, int blk)
{
    unsigned i;
    uint32_t v;

    printf("portdump: block %d (ACC 0)\n", blk);
    fflush(stdout);
    msleep(250);
    for (i = 0; i < sizeof(PORTREGS) / sizeof(PORTREGS[0]); i++) {
        printf("portdump: %-20s 0x%07x ... ", PORTREGS[i].name, PORTREGS[i].addr);
        fflush(stdout);
        msleep(200);
        if (schan_get(cmc, blk, PORTREGS[i].addr, &v) == 0)
            printf("0x%08x\n", v);
        else
            printf("TIMEOUT\n");
        fflush(stdout);
    }
    printf("portdump: done\n");
    return 0;
}

/* Phase C -- LCPLL programming and PLL reset release (trident2.c:9483-9740).
 *
 * The step phaseB deliberately skipped. Blocks come out of reset on default
 * clocking without it, but nothing timing-sensitive is trustworthy until the
 * LCPLLs are configured and locked.
 *
 * All addresses looked up individually in allregs_t.i -- the strides look
 * regular (0x600 per PLL) but assuming that is exactly the mistake that cost us
 * two wrong-address failures, so each one is confirmed:
 *
 *   PLL      CTRL_1      CTRL_2      STATUS
 *   0        0x2031200   0x2031300   0x2031600
 *   1        0x2031800   0x2031900   0x2031c00
 *   2        0x2031e00   0x2031f00   0x2032200
 *   3        0x2032400   0x2032500   0x2032800
 *
 * CTRL_1 PDIV     = bits [21:18], set to 7
 * CTRL_2 NDIV_INT = bits [9:0],   set to 140  (TD2P/TT2P branch, 9512)
 * STATUS TOP_XGPLL_LOCK = bit 31 (fields_t.i:29634)
 *
 * TOP_SOFT_RESET_REG_2 (0x2030200) field bits (fields_t.i:24567):
 *   XG_PLL0..3_RST_L 0,2,4,6   TS_PLL_RST_L 8   BS_PLL_RST_L 10
 *   AVS/ARS bits 18..21 are already set on a cold chip (reads 0x003c0000),
 *   which is why block 59 (AVS) answers before any of this runs.
 */
static const struct { uint32_t ctrl1, ctrl2, status; } LCPLL[4] = {
    { 0x2031200, 0x2031300, 0x2031600 },
    { 0x2031800, 0x2031900, 0x2031c00 },
    { 0x2031e00, 0x2031f00, 0x2032200 },
    { 0x2032400, 0x2032500, 0x2032800 },
};
#define PLL_LOCK_BIT (1u << 31)

static int phaseC(int cmc)
{
    uint32_t v = 0;
    int i, locked = 0;

    printf("phaseC: LCPLL config + PLL reset release\n");
    fflush(stdout);
    msleep(300);

    for (i = 0; i < 4; i++) {
        if (schan_get(cmc, TOP_BLK, LCPLL[i].status, &v) == 0)
            printf("phaseC: PLL%d status before = 0x%08x  lock=%d\n", i, v,
                   !!(v & PLL_LOCK_BIT));
        else
            printf("phaseC: PLL%d status read FAILED\n", i);
        fflush(stdout);
    }
    msleep(300);

    for (i = 0; i < 4; i++) {
        printf("phaseC: PLL%d CTRL_1 PDIV=7 ... ", i);
        fflush(stdout);
        msleep(150);
        if (schan_get(cmc, TOP_BLK, LCPLL[i].ctrl1, &v)) { printf("read FAILED\n"); return 3; }
        v = (v & ~(0xfu << 18)) | (7u << 18);
        if (schan_put(cmc, TOP_BLK, LCPLL[i].ctrl1, v)) { printf("write FAILED\n"); return 3; }
        printf("0x%08x\n", v);
        fflush(stdout);

        printf("phaseC: PLL%d CTRL_2 NDIV_INT=140 ... ", i);
        fflush(stdout);
        msleep(150);
        if (schan_get(cmc, TOP_BLK, LCPLL[i].ctrl2, &v)) { printf("read FAILED\n"); return 3; }
        v = (v & ~0x3ffu) | 140u;
        if (schan_put(cmc, TOP_BLK, LCPLL[i].ctrl2, v)) { printf("write FAILED\n"); return 3; }
        printf("0x%08x\n", v);
        fflush(stdout);
    }
    msleep(300);

    printf("phaseC: READ  TOP_SOFT_RESET_REG_2 (0x%07x) ... ", TOP_SOFT_RESET_REG_2);
    fflush(stdout);
    msleep(250);
    if (schan_get(cmc, TOP_BLK, TOP_SOFT_RESET_REG_2, &v)) {
        printf("FAILED\n"); return 3;
    }
    printf("0x%08x\n", v);
    fflush(stdout);
    msleep(250);

    /* XG_PLL0..3, TS, BS out of reset. AVS/ARS bits are preserved as read. */
    v |= (1u << 0) | (1u << 2) | (1u << 4) | (1u << 6) | (1u << 8) | (1u << 10);
    printf("phaseC: WRITE TOP_SOFT_RESET_REG_2 = 0x%08x (PLLs out of reset) ... ", v);
    fflush(stdout);
    msleep(250);
    if (schan_put(cmc, TOP_BLK, TOP_SOFT_RESET_REG_2, v)) {
        printf("FAILED\n"); return 3;
    }
    printf("ok\n");
    fflush(stdout);
    msleep(300);

    if (schan_get(cmc, TOP_BLK, TOP_SOFT_RESET_REG_2, &v) == 0)
        printf("phaseC:   read-back 0x%08x\n", v);
    fflush(stdout);

    printf("phaseC: waiting for PLL lock\n");
    fflush(stdout);
    msleep(500);

    for (i = 0; i < 4; i++) {
        if (schan_get(cmc, TOP_BLK, LCPLL[i].status, &v) == 0) {
            printf("phaseC: PLL%d status after  = 0x%08x  lock=%s\n", i, v,
                   (v & PLL_LOCK_BIT) ? "** LOCKED **" : "no");
            if (v & PLL_LOCK_BIT) locked++;
        } else {
            printf("phaseC: PLL%d status read FAILED\n", i);
        }
        fflush(stdout);
    }
    printf("phaseC: %d/4 LCPLLs locked\n", locked);
    fflush(stdout);
    msleep(300);

    /* De-assert the LCPLL POST resets -- trident2.c:9772-9788.
     *
     * A SECOND write to TOP_SOFT_RESET_REG_2, after the lock check. We had
     * missed it entirely: phaseC set only the *_RST_L bits (0,2,4,6,8,10) and
     * stopped once the PLLs reported lock.
     *
     * A locked PLL with its post-divider still in reset produces no output
     * clock. That fits every symptom of the port blocker exactly: the LCPLLs
     * report LOCKED, yet XLPORT_TSC_PLL_LOCK_STATUS reads 0 and every XLMAC
     * register times out because the block is unclocked.
     *
     * Field bits (fields_t.i:24567): XG_PLL0..3_POST_RST_L 1,3,5,7;
     * TS_PLL_POST_RST_L 9; BS_PLL_POST_RST_L 11. The SDK also sets
     * top_soft_rst_field, which for TD2P/TT2P is TOP_PVT_MON_MIN_RST_L
     * (bit 16, trident2.c:9352) rather than TOP_TEMP_MON_PEAK_RST_L.
     *
     * The AS5610 (BCM56846/Trident+) reverse-engineering reached the same
     * conclusion independently: its BMD sequence is PLLs out of reset -> wait
     * for lock -> POST dividers -> port groups, as a distinct ordered step.
     */
    printf("phaseC: READ  TOP_SOFT_RESET_REG_2 before POST de-assert ... ");
    fflush(stdout);
    msleep(250);
    if (schan_get(cmc, TOP_BLK, TOP_SOFT_RESET_REG_2, &v)) {
        printf("FAILED\n"); return 3;
    }
    printf("0x%08x\n", v);
    fflush(stdout);

    v |= (1u << 1) | (1u << 3) | (1u << 5) | (1u << 7)   /* XG_PLL0..3 POST */
       | (1u << 9) | (1u << 11)                          /* TS, BS POST     */
       | (1u << 16);                                     /* PVT_MON_MIN     */
    printf("phaseC: WRITE TOP_SOFT_RESET_REG_2 = 0x%08x (POST de-assert) ... ", v);
    fflush(stdout);
    msleep(250);
    if (schan_put(cmc, TOP_BLK, TOP_SOFT_RESET_REG_2, v)) {
        printf("FAILED\n"); return 3;
    }
    printf("ok\n");
    fflush(stdout);
    msleep(300);

    if (schan_get(cmc, TOP_BLK, TOP_SOFT_RESET_REG_2, &v) == 0)
        printf("phaseC:   read-back 0x%08x\n", v);
    fflush(stdout);
    msleep(300);

    for (i = 0; i < 4; i++) {
        if (schan_get(cmc, TOP_BLK, LCPLL[i].status, &v) == 0)
            printf("phaseC: PLL%d after POST      = 0x%08x  lock=%s\n", i, v,
                   (v & PLL_LOCK_BIT) ? "LOCKED" : "no");
        fflush(stdout);
    }
    return locked == 4 ? 0 : 1;
}

/* Non-destructive round trip: save, write pattern, verify, restore, confirm.
 *
 * This is how the write path gets its first exercise. It only makes sense on a
 * register with no side effects -- AVS_REG_TOP_CTRL_SPARE_LOW (block 59,
 * 0x20af800, allregs_a.i:53780) is a spare with reset value 0 and a full
 * 0xffffffff writable mask, so a pattern written there means nothing to the
 * chip and is restored regardless of outcome.
 */
static int schan_wtest(int cmc, int blk, uint32_t addr, uint32_t pattern)
{
    uint32_t orig = 0, got = 0, back = 0;
    uint32_t whdr = (0x0du << 26) | ((blk & 0x3f) << 20) | (4u << 7);
    int rv;

    printf("wtest: block %d addr 0x%08x pattern 0x%08x\n", blk, addr, pattern);
    if (schan_get(cmc, blk, addr, &orig)) {
        printf("wtest: cannot READ the register -- aborting before any write\n");
        return 3;
    }
    printf("wtest: original = 0x%08x\n", orig);

    printf("wtest: WRITE pattern\n");
    rv = schan_write_op(cmc, whdr, addr, pattern);
    if (rv) { printf("wtest: write failed (%d) -- nothing to restore\n", rv); return rv; }

    if (schan_get(cmc, blk, addr, &got)) {
        printf("wtest: read-back FAILED after write\n");
        return 3;
    }
    printf("wtest: read-back = 0x%08x  %s\n", got,
           got == pattern ? "** MATCHES PATTERN -- WRITE PATH WORKS **"
                          : "does not match (register may be RO or masked)");

    printf("wtest: restoring original 0x%08x\n", orig);
    if (schan_write_op(cmc, whdr, addr, orig)) {
        printf("wtest: ** RESTORE FAILED -- register left at 0x%08x **\n", got);
        return 3;
    }
    if (!schan_get(cmc, blk, addr, &back))
        printf("wtest: after restore = 0x%08x  %s\n", back,
               back == orig ? "restored" : "** NOT RESTORED **");
    return got == pattern ? 0 : 1;
}

/* Phase A, no-verify variant.
 *
 * MMIO writes are posted and cannot stall the CPU; reads can. Both previous
 * hangs had a read-back immediately after the write, so the read is the more
 * likely culprit. This variant writes the ring map and SBUS timeout with NO
 * read of 0x0100xx at all, then goes straight to an S-Channel transaction --
 * which only touches the CMC block at 0x031000+, proven safe.
 *
 * If S-Channel comes back without TIMEOUT, the writes landed all along and our
 * verification step was the blocker.
 *
 * Register order follows the SDK exactly: ring map first, then timeout.
 */
static int phaseA_noverify(int cmc, uint32_t hdr, uint32_t addr, int nwords)
{
    int i;

    printf("phaseA2: writing ring map, NO read-back\n");
    for (i = 0; i < RING_MAP_N; i++) {
        printf("phaseA2:   w 0x%06x = 0x%08x\n", RING_MAP_BASE + i * 4,
               RING_MAP[i]);
        wr(asic, RING_MAP_BASE + i * 4, RING_MAP[i]);
    }
    printf("phaseA2:   w 0x%06x = 0x000007d0 (SBUS timeout)\n", SBUS_TIMEOUT);
    wr(asic, SBUS_TIMEOUT, 0x7d0);

    printf("phaseA2: all writes issued, no 0x0100xx reads performed\n");
    msleep(10);

    printf("phaseA2: now the real test -- S-Channel read via CMC%d\n", cmc);
    return schan_read(cmc, hdr, addr, nwords);
}

/* ---- PCI config space ------------------------------------------------------
 *
 * `echo 1 > .../enable` runs pci_enable_device(), which sets Memory Space but
 * nothing else. Arista's own kernel BDE (arista-bde.ko, _pci_probe) does more,
 * and since PCI setup is the ONLY thing between reset-release and the first
 * ring-map write, any difference here is a live suspect. Decoded from the
 * module -- it ships unstripped -- and cross-checked against live EOS:
 *
 *   - walk the capability list for PCI_CAP_ID_EXP (0x10); on this board it
 *     lands at 0xac, NOT the 0x48 the cap pointer first points at (that is
 *     Power Management, then VPD at 0x50, then MSI at 0x58).
 *   - DEVCTL (cap+8): clear bit 4, Enable Relaxed Ordering.  Unconditional.
 *   - MPS/MRRS (DEVCTL bits 7:5 and 14:12) only when the maxpayload module
 *     param is set; it defaults to 0 and live EOS reads 128/128, the reset
 *     default -- so we deliberately do NOT touch them.
 *   - PCI_COMMAND (0x04) |= 0x6, Memory Space + Bus Master.
 *   - the 0xb4/0xb5 vendor-config quirk applies only to chips whose table
 *     entry has the flag at +0x3e set (Arad, fe1600, fe3200). Trident2Plus
 *     (devid 0xb860) has it clear, so it does not apply here.
 */
#define PCI_CFG        ASIC_DEV "/config"
#define PCI_COMMAND    0x04
#define PCI_CAP_PTR    0x34
#define PCI_CAP_ID_EXP 0x10
#define PCI_EXP_DEVCTL 0x08
#define PCI_EXP_DEVCTL_RELAX_EN 0x0010

static int cfg_rd(unsigned off, unsigned len, uint32_t *out)
{
    unsigned char b[4] = { 0, 0, 0, 0 };
    int fd = open(PCI_CFG, O_RDONLY), n;
    if (fd < 0) { perror(PCI_CFG); return -1; }
    n = pread(fd, b, len, off);
    close(fd);
    if (n != (int)len) { perror("pread config"); return -1; }
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

static int cfg_wr(unsigned off, unsigned len, uint32_t val)
{
    unsigned char b[4];
    int fd = open(PCI_CFG, O_WRONLY), n;
    if (fd < 0) { perror(PCI_CFG); return -1; }
    b[0] = val & 0xff;         b[1] = (val >> 8) & 0xff;
    b[2] = (val >> 16) & 0xff; b[3] = (val >> 24) & 0xff;
    n = pwrite(fd, b, len, off);
    close(fd);
    if (n != (int)len) { perror("pwrite config"); return -1; }
    return 0;
}

/* Config offset of the PCI Express capability, or 0 if absent. */
static unsigned find_pcie_cap(void)
{
    uint32_t v;
    unsigned off, guard;
    if (cfg_rd(PCI_CAP_PTR, 1, &v)) return 0;
    off = v & 0xfc;
    for (guard = 0; off && guard < 48; guard++) {
        uint32_t id, next;
        if (cfg_rd(off, 1, &id)) return 0;
        if ((id & 0xff) == 0xff) return 0;
        if ((id & 0xff) == PCI_CAP_ID_EXP) return off;
        if (cfg_rd(off + 1, 1, &next)) return 0;
        off = next & 0xfc;
    }
    return 0;
}

static void show_cfg_key(unsigned cap)
{
    uint32_t devctl = 0, cmd = 0;
    cfg_rd(PCI_COMMAND, 2, &cmd);
    printf("  COMMAND = 0x%04x  Mem=%d BusMaster=%d\n", cmd,
           !!(cmd & 2), !!(cmd & 4));
    if (!cap) return;
    cfg_rd(cap + PCI_EXP_DEVCTL, 2, &devctl);
    printf("  DEVCTL  = 0x%04x  RlxdOrd=%d MPS=%u MRRS=%u\n", devctl,
           !!(devctl & PCI_EXP_DEVCTL_RELAX_EN),
           (devctl >> 5) & 7, (devctl >> 12) & 7);
}

static int do_cfgdump(void)
{
    unsigned off, cap;
    printf("ASIC PCI config space (256 bytes):\n");
    for (off = 0; off < 0x100; off += 16) {
        unsigned i;
        printf("%02x:", off);
        for (i = 0; i < RING_MAP_N; i++) {
            uint32_t b;
            if (cfg_rd(off + i, 1, &b)) return 1;
            printf(" %02x", b & 0xff);
        }
        printf("\n");
    }
    cap = find_pcie_cap();
    printf("PCIe capability at 0x%02x%s\n", cap, cap ? "" : "  ** NOT FOUND **");
    show_cfg_key(cap);
    return 0;
}

/* Apply exactly what arista-bde's _pci_probe applies to a Trident2Plus. */
static int do_pcicfg(void)
{
    unsigned cap = find_pcie_cap();
    uint32_t devctl = 0, cmd = 0;

    if (!cap) { printf("pcicfg: no PCIe capability found\n"); return 1; }
    printf("pcicfg: PCIe capability at 0x%02x\n", cap);
    printf("pcicfg: before\n");
    show_cfg_key(cap);

    if (cfg_rd(cap + PCI_EXP_DEVCTL, 2, &devctl)) return 1;
    devctl &= ~(uint32_t)PCI_EXP_DEVCTL_RELAX_EN;
    if (cfg_wr(cap + PCI_EXP_DEVCTL, 2, devctl)) return 1;

    if (cfg_rd(PCI_COMMAND, 2, &cmd)) return 1;
    if ((cmd & 0x6) != 0x6 && cfg_wr(PCI_COMMAND, 2, cmd | 0x6)) return 1;

    printf("pcicfg: after\n");
    show_cfg_key(cap);
    printf("pcicfg: done\n");
    return 0;
}

/* ---- cold probe ------------------------------------------------------------
 *
 * The console runs at 9600 baud, ~1 ms per character. Every earlier hang
 * truncated its own last line, which is exactly how two hangs got
 * misattributed to the write preceding the read. So: announce the access, let
 * the UART drain, and only then touch the register. Whatever line ends the
 * console log is then unambiguously the access that wedged the CPU.
 *
 * Reads only, with no writes anywhere before them. This asks the one question
 * that splits the hypothesis space in half. If a bare cold read of
 * CMIC_SBUS_TIMEOUT works, then something *we* do (phaseP/phase0) breaks the
 * chip and the fix is ours. If it hangs with nothing written at all, the SBUS
 * clock domain is not alive on a cold part and reordering our writes can never
 * help.
 */
static void announce(const char *what, uint32_t off)
{
    printf("coldprobe: READ %-22s 0x%06x ... ", what, off);
    fflush(stdout);
    msleep(300);            /* let 9600 baud drain before we risk the CPU */
}

static int do_coldprobe(void)
{
    static const struct { const char *name; uint32_t off; } probes[] = {
        { "CMIC_DEV_REV_ID",      0x010224 },   /* known good */
        { "CMIC_CPS_RESET",       0x010220 },
        { "USERIF_TIMEOUT",       0x010250 },
        { "USERIF_PURGE_CONTROL", 0x010260 },
        { "CMC0_SCHAN_CTRL",      0x031000 },
        /* --- everything above is proven; below is the actual question --- */
        { "CMIC_SBUS_TIMEOUT",    0x010094 },
        { "SBUS_RING_MAP_0_7",    0x010098 },
        { "SBUS_RING_MAP_56_63",  0x0100b4 },
    };
    unsigned i;

    printf("coldprobe: READ-ONLY -- no register has been written this boot.\n");
    printf("coldprobe: if the console stops, the last line names the culprit.\n");
    msleep(300);
    for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        uint32_t v;
        announce(probes[i].name, probes[i].off);
        v = rd(asic, probes[i].off);
        printf("0x%08x\n", v);
        fflush(stdout);
        msleep(120);
    }
    printf("coldprobe: ALL READS SURVIVED\n");
    return 0;
}

/* ---- SCD SMBus master -----------------------------------------------------
 *
 * Transcribed from Arista's GPL driver (edgecore/sonic/src/scd-smbus.{c,h}) --
 * the same driver that gave us the reset register and the watchdog, so this is
 * transcription rather than reverse engineering.
 *
 * Masters live at BAR0 + 0x8000, stride 0x80. Confirmed on this board by
 * reading the live SCD: masters 0..8 populated, 9 absent, i.e.
 * addSmbusMasterRange(0x8000, 9, 0x80). Each master fronts up to 8 buses.
 *
 *   +0x10 REQUEST   d[7:0] ss[13:8] ed[14] br[15] dat[17:16] t[19:18]
 *                   sp[20] da[21] dod[22] st[23] bs[27:24] ti[31:28]
 *   +0x20 CONTROL   nrs[9:0] fsz[12:10] foe[13] sp[15:14] nrq[25:16]
 *                   brb[26] ver[29:28] fe[30] rst[31]
 *   +0x30 RESPONSE  d[7:0] bus_conflict[8] timeout[9] ack_err[10] flushed[11]
 *                   ti[15:12] ss[21:16] foe[30] fe[31]
 *   +0x40 SPEED
 *
 * The slave address goes in the request's DATA byte as (addr << 1) | rd
 * (scd-smbus.c:357). `ss` is the transfer SIZE -- the total number of request
 * words in the transaction -- and is written only on the FIRST request
 * (scd-smbus.c:338, cleared at :363).
 *
 * Note the two different stop-flag rules, which are easy to get wrong:
 *   scd-smbus.c:351  header    req.sp = req.ti == ss     (pre-increment ti)
 *   scd-smbus.c:383  data byte req.sp = ti  == ss        (post-increment ti)
 */
#define SMB_BASE      0x8000
#define SMB_STRIDE    0x80
#define SMB_REQ       0x10
#define SMB_CS        0x20
#define SMB_RESP      0x30
#define SMB_SP        0x40

#define CS_FE     (1u << 30)
#define CS_RST    (1u << 31)
#define RSP_BCE   (1u << 8)
#define RSP_TO    (1u << 9)
#define RSP_ACKE  (1u << 10)
#define RSP_FLUSH (1u << 11)
#define RSP_FOE   (1u << 30)
#define RSP_FE    (1u << 31)

static uint32_t smb_base(int master) { return SMB_BASE + master * SMB_STRIDE; }

static uint32_t smb_req_word(unsigned d, unsigned ss, unsigned ed, unsigned br,
                             unsigned t, unsigned sp, unsigned da, unsigned dod,
                             unsigned st, unsigned bs, unsigned ti)
{
    return (d & 0xff) | ((ss & 0x3f) << 8) | ((ed & 1) << 14) |
           ((br & 1) << 15) | ((t & 3) << 18) | ((sp & 1) << 20) |
           ((da & 1) << 21) | ((dod & 1) << 22) | ((st & 1) << 23) |
           ((bs & 0xf) << 24) | ((ti & 0xf) << 28);
}

/* Poll CONTROL until the transaction completes (fe), then clear fe by writing
 * the value back -- scd_smbus_master_wait(), scd-smbus.c:251. */
static int smb_wait(int master, uint32_t *cs_out)
{
    uint32_t b = smb_base(master), cs = 0;
    int i;

    for (i = 0; i < 2000; i++) {
        cs = rd(scd, b + SMB_CS);
        if (cs & CS_FE) break;
        usleep(200);
    }
    if (cs_out) *cs_out = cs;
    if (!(cs & CS_FE)) return -1;
    wr(scd, b + SMB_CS, cs);          /* clear fe */
    return 0;
}

/* Drain any stale responses so a fresh transaction starts clean. */
static void smb_drain(int master)
{
    uint32_t b = smb_base(master);
    int i;
    for (i = 0; i < 16; i++) {
        uint32_t cs = rd(scd, b + SMB_CS);
        if (!(cs & 0x03ff)) break;    /* nrs == 0 */
        (void)rd(scd, b + SMB_RESP);
    }
}

/* One-byte read: [S addr|R  data  P]. ss = 2 (header + 1 data). */
static int smb_read_byte(int master, int bus, int addr, uint32_t *out)
{
    uint32_t b = smb_base(master), rsp;
    int ti = 0, ss = 2;

    smb_drain(master);
    /* header: st=1 dod=1 da=0, d = (addr<<1)|1, carries ss */
    wr(scd, b + SMB_REQ,
       smb_req_word((addr << 1) | 1, ss, 0, 0, 1, (ti == ss), 0, 1, 1, bus, ti));
    ti++;
    /* data: last word so sp=1, da = rd && !sp = 0 */
    wr(scd, b + SMB_REQ,
       smb_req_word(0, 0, 0, 0, 1, ((ti + 1) == ss), 0, 0, 0, bus, ti));

    if (smb_wait(master, NULL)) return -1;
    (void)rd(scd, b + SMB_RESP);      /* header response */
    rsp = rd(scd, b + SMB_RESP);      /* data response */
    if (rsp & (RSP_BCE | RSP_TO | RSP_ACKE | RSP_FLUSH | RSP_FOE | RSP_FE))
        return -2;
    if (out) *out = rsp & 0xff;
    return 0;
}

/* Probe: a one-byte read that we only check for ACK. This is what i2cdetect
 * does; it is non-destructive on these parts (no register is written). */
static int smb_probe(int master, int bus, int addr)
{
    uint32_t v;
    return smb_read_byte(master, bus, addr, &v);
}

/* Single-byte write: [S addr|W  data  P]. Same shape as the read, but dod stays
 * 1 on the data word and the byte is ours. */
static int smb_write_byte(int master, int bus, int addr, unsigned val)
{
    uint32_t b = smb_base(master), rsp;
    int ti = 0, ss = 2;

    smb_drain(master);
    wr(scd, b + SMB_REQ,
       smb_req_word((addr << 1) | 0, ss, 0, 0, 1, (ti == ss), 0, 1, 1, bus, ti));
    ti++;
    wr(scd, b + SMB_REQ,
       smb_req_word(val, 0, 0, 0, 1, ((ti + 1) == ss), 0, 1, 0, bus, ti));

    if (smb_wait(master, NULL)) return -1;
    (void)rd(scd, b + SMB_RESP);
    rsp = rd(scd, b + SMB_RESP);
    if (rsp & (RSP_BCE | RSP_TO | RSP_ACKE | RSP_FLUSH | RSP_FOE | RSP_FE))
        return -2;
    return 0;
}

/* PCA954x mux: the control register is a bare byte at the mux's own address,
 * bit N enabling channel N (Pca9548KernelDriver, NUM_CHANNELS = 8). Select a
 * channel, scan behind it, then close the mux again.
 *
 * Closing matters: leaving a channel open would change what every later scan on
 * this bus sees, making results depend on history. */
static int do_smbmux(int master, int bus, int muxaddr, int lo, int hi)
{
    int ch, a, total = 0;

    printf("smbmux: master %d bus %d mux 0x%02x, channels 0..7\n",
           master, bus, muxaddr);
    fflush(stdout);
    msleep(200);

    for (ch = 0; ch < 8; ch++) {
        int found = 0;
        if (smb_write_byte(master, bus, muxaddr, 1u << ch)) {
            printf("smbmux: ch%d  select FAILED\n", ch);
            fflush(stdout);
            continue;
        }
        for (a = lo; a <= hi; a++) {
            if (a == muxaddr) continue;      /* never probe the mux itself */
            if (smb_probe(master, bus, a) == 0) {
                printf("smbmux: ch%d  0x%02x  ACK  *** device ***\n", ch, a);
                found++; total++;
                fflush(stdout);
            }
        }
        if (!found) { printf("smbmux: ch%d  (none)\n", ch); fflush(stdout); }
    }

    smb_write_byte(master, bus, muxaddr, 0x00);   /* close all channels */
    printf("smbmux: mux closed; %d device(s) behind it\n", total);
    return 0;
}

static int do_smbscan(int master, int bus, int lo, int hi)
{
    int a, found = 0;

    printf("smbscan: master %d bus %d, addresses 0x%02x..0x%02x\n",
           master, bus, lo, hi);
    printf("smbscan: read-byte probe only -- nothing is written\n");
    fflush(stdout);
    msleep(200);

    for (a = lo; a <= hi; a++) {
        int rv = smb_probe(master, bus, a);
        if (rv == 0) {
            printf("smbscan:   0x%02x  ACK  *** device ***\n", a);
            found++;
        } else if (rv == -1) {
            printf("smbscan:   0x%02x  (no completion)\n", a);
        }
        fflush(stdout);
    }
    printf("smbscan: %d device(s) on master %d bus %d\n", found, master, bus);
    return 0;
}

/* Print only non-zero / non-ff SCD registers in a range. The SCD aliases each
 * register across 16 bytes, so stepping 0x10 gives one line per real register.
 * Bounded and read-only: the SCD range 0x0-0xc000 is the part proven safe on
 * this board, and this never leaves it. */
static int do_scdscan(uint32_t lo, uint32_t hi, uint32_t step)
{
    uint32_t off;
    int n = 0;

    if (hi > SCD_MAP_SIZE) hi = SCD_MAP_SIZE;
    if (step < 4) step = 4;
    step &= ~3u;                       /* registers are 32-bit */
    /* The default 0x10 stride is what the block layout suggests, but it makes
     * any register at +0x4/+0x8/+0xc within a group invisible -- which is
     * exactly the blind spot that left the fan PWM unfound. */
    printf("scdscan: 0x%04x-0x%04x, non-zero/non-ff, step 0x%x\n", lo, hi, step);
    for (off = lo; off < hi; off += step) {
        uint32_t v = rd(scd, off);
        if (v == 0 || v == 0xffffffff) continue;
        printf("  0x%04x = 0x%08x\n", off, v);
        n++;
        if (n > 20000) { printf("  ... truncated\n"); break; }
    }
    printf("scdscan: %d register(s)\n", n);
    return 0;
}

static int do_smbdump(void)
{
    int i;
    printf("SCD SMBus masters at 0x%04x stride 0x%02x:\n", SMB_BASE, SMB_STRIDE);
    printf("  m   base     REQ        CS         RESP       SPEED\n");
    for (i = 0; i < 10; i++) {
        uint32_t b = smb_base(i);
        printf("  %d   0x%04x   %08x   %08x   %08x   %08x\n", i, b,
               rd(scd, b + SMB_REQ), rd(scd, b + SMB_CS),
               rd(scd, b + SMB_RESP), rd(scd, b + SMB_SP));
    }
    return 0;
}

/* ---- SBUS-MDIO: the real SerDes access path on TD2+ ------------------------
 *
 * On Trident2+ the internal TSC SerDes is NOT reached through the CMIC MIIM
 * block -- measured: EOS never writes MIIM_PARAM at all. `td2_sbus_mdio`
 * defaults true for TD2P/TT2P (trident2.c:13758), routing SerDes MDIO over SBUS
 * through a parallel-bus window in the XLPORT block.
 *
 * Protocol, from soc_sbus_mdio_reg_read (drv.c:23475-23530). Everything is
 * READ_MEM/WRITE_MEM on XLPORT_WC_UCMEM_DATA index 0, block 15, 4 words:
 *
 *   entry[0] = sbus_mdio_addr(phy_addr, 0xffde)   ; AER register
 *   entry[1] = lane << 16
 *   entry[2] = 1                                  ; write
 *   write entry
 *
 *   entry[0] = sbus_mdio_addr(phy_addr, phy_reg) | entry[1]
 *   entry[2] = 0                                  ; read
 *   write entry
 *
 *   read entry ; phy_data = entry[0]
 *
 * Address encoding (soc_sbus_mdio_addr, drv.c:23456):
 *   addr = (phy_reg & 0xffff) | ((phy_addr & 0x1f) << 19) | (devad << 27)
 *   devad = (phy_reg >> 27) & 0x1f      lane = (phy_reg >> 16) & 0x7
 */
#define WC_UCMEM_CTRL 0x2021400
#define WC_UCMEM_DATA 0x0
#define XLPORT_BLK    15

static uint32_t sbus_mdio_addr(uint32_t phy_addr, uint32_t phy_reg)
{
    uint32_t devad = (phy_reg >> 27) & 0x1f;
    return (phy_reg & 0xffff) | ((phy_addr & 0x1f) << 19) | (devad << 27);
}

/* One SBUS-MDIO read. blk is the XLPORT block (15 for instance 0). */
static int sbus_mdio_read(int cmc, int blk, uint32_t phy_addr,
                          uint32_t phy_reg, uint32_t *out)
{
    uint32_t e[4];
    uint32_t lane = (phy_reg >> 16) & 0x7;

    /* AER: select the lane */
    e[0] = sbus_mdio_addr(phy_addr, 0xffde);
    e[1] = lane << 16;
    e[2] = 1;               /* write */
    e[3] = 0;
    if (mem_put(cmc, blk, 0, WC_UCMEM_DATA, e, 4, 16)) return -1;

    /* Point at the target register, mark as read */
    e[0] = sbus_mdio_addr(phy_addr, phy_reg) | e[1];
    e[2] = 0;               /* read */
    if (mem_put(cmc, blk, 0, WC_UCMEM_DATA, e, 4, 16)) return -2;

    /* The MDIO result's landing word is not something to guess at. Earlier
     * sbmdio always returned 0 because only e[0] was inspected; dump the whole
     * entry so the data can be located rather than assumed. */
    usleep(1000);
    if (mem_get(cmc, blk, 0, WC_UCMEM_DATA, e, 4)) return -3;
    if (getenv("SBMDIO_VERBOSE"))
        printf("[entry %08x %08x %08x %08x] ", e[0], e[1], e[2], e[3]);
    if (out) *out = e[0];
    return 0;
}

static int do_sbmdio(int cmc, int blk, uint32_t phy, uint32_t reg)
{
    uint32_t v = 0;
    int rv;

    printf("sbmdio: blk %d phy 0x%02x reg 0x%08x (devad %u lane %u) ... ",
           blk, phy, reg, (reg >> 27) & 0x1f, (reg >> 16) & 0x7);
    fflush(stdout);
    msleep(250);
    rv = sbus_mdio_read(cmc, blk, phy, reg, &v);
    if (rv) { printf("FAILED (%d)\n", rv); return 1; }
    printf("0x%08x\n", v);
    return 0;
}

/* Sweep PHY addresses reading a TSC identity register. */
static int do_sbmdioscan(int cmc, int blk, uint32_t reg)
{
    uint32_t phy, v;
    int found = 0;

    printf("sbmdioscan: blk %d reg 0x%08x, phy 0x00..0x1f\n", blk, reg);
    fflush(stdout);
    msleep(250);
    for (phy = 0; phy < 32; phy++) {
        v = 0xdeadbeef;
        if (sbus_mdio_read(cmc, blk, phy, reg, &v)) {
            printf("sbmdioscan:   phy 0x%02x  FAILED\n", phy);
        } else {
            printf("sbmdioscan:   phy 0x%02x  0x%08x%s\n", phy, v,
                   (v && v != 0xffff && v != 0xffffffff) ? "  *** responds ***" : "");
            if (v && v != 0xffff && v != 0xffffffff) found++;
        }
        fflush(stdout);
    }
    printf("sbmdioscan: %d responder(s)\n", found);
    return 0;
}

/* ---- XLPORT bring-up, in the SDK's own order -------------------------------
 *
 * `tscinit` and `xlportinit` each do a part of this and neither works alone.
 * The trace shows why (attributed-trace.txt.gz, ops 70,539-70,550): the SDK
 * interleaves the port layer and the SOC layer, and the TSC reset sits IN THE
 * MIDDLE of _pm4x10_pm_xlport_init, not before it.
 *
 *   XLPORT_POWER_SAVE   0x02020a00 <- 0        pm4x10.c:3885
 *   XLPORT_MODE_REG     0x02020600 <- 0        pm4x10.c:3919   (read back 0x40)
 *   _soc_xgxs_reset_single_tsc on this block's PGW TSC instance, WITH the
 *   11.1 ms hold                               drv.c:4381-4450
 *   XLPORT_MAC_CONTROL  0x02020d00 <- 0        pm4x10.c:3941   (XMAC0_RESET)
 *   -- the register mailbox answers from here on --
 *
 * Neither existing command produces that state:
 *
 *   - `tscinit` resets every TSC correctly but never clears POWER_SAVE, so the
 *     XLPORT block stays asleep and every lane read returns zero.
 *   - `xlportinit` clears POWER_SAVE but replays 120 captured TSC writes with
 *     NO delays, which re-runs the reset badly and undoes what tscinit did.
 *
 * Which XLPORT block is served by which PGW TSC instance is not guessable --
 * XLPORT 15 uses PGW 6 TSC0 while XLPORT 18 uses PGW 6 TSC3. The map is read
 * out of the trace by tools/gen-xlport-tsc-map.py.
 */
#include "xlport-tsc-map.h"

#define XLPORT_POWER_SAVE    0x02020a00
#define XLPORT_MODE_REG      0x02020600
#define XLPORT_MAC_CONTROL   0x02020d00
#define XLPORT_WC_UCMEM_CTRL 0x02021400
#define XLPORT_UCMEM_DATA    0x00000000

#define XLPORT_MAP_N ((int)(sizeof(xlport_tsc_map) / sizeof(xlport_tsc_map[0])))

static const struct xlport_tsc *xlport_lookup(int blk)
{
    int i;
    for (i = 0; i < XLPORT_MAP_N; i++)
        if (xlport_tsc_map[i].blk == blk) return &xlport_tsc_map[i];
    return NULL;
}

static int xlport_reg_zero(int cmc, int blk, uint32_t addr)
{
    uint32_t z = 0;
    return reg_put(cmc, blk, 0, addr, &z, 1, 4);
}

/* _soc_xgxs_reset_single_tsc for one TSC instance, with the real waits. */
static int xlport_tsc_reset(int cmc, const struct xlport_tsc *e,
                            unsigned long settle, unsigned long hold)
{
    uint32_t v;

    if (reg_get(cmc, e->pgw, 0, e->tsc_reg, &v, 1, 4)) return -1;
    v |= TSC_REFIN_EN;
    if (reg_put(cmc, e->pgw, 0, e->tsc_reg, &v, 1, 4)) return -1;
    v &= ~TSC_PWRDWN;
    if (reg_put(cmc, e->pgw, 0, e->tsc_reg, &v, 1, 4)) return -1;
    usleep(settle);
    v &= ~TSC_RSTB_HW;
    if (reg_put(cmc, e->pgw, 0, e->tsc_reg, &v, 1, 4)) return -1;
    usleep(hold);                                   /* THE 11.1 ms */
    v |= TSC_RSTB_HW;
    if (reg_put(cmc, e->pgw, 0, e->tsc_reg, &v, 1, 4)) return -1;
    usleep(settle);
    return 0;
}

static int xlport_bring(int cmc)
{
    const char *e1 = getenv("TSC_SETTLE_US"), *e2 = getenv("TSC_HOLD_US");
    unsigned long settle = e1 ? strtoul(e1, NULL, 0) : 1100;
    unsigned long hold = e2 ? strtoul(e2, NULL, 0) : 11100;
    int i, ok = 0, bad = 0;

    printf("xlportbring: %d XLPORT blocks, %lu us settle / %lu us reset hold\n",
           XLPORT_MAP_N, settle, hold);

    for (i = 0; i < XLPORT_MAP_N; i++) {
        const struct xlport_tsc *e = &xlport_tsc_map[i];
        int rv = 0;

        if (xlport_reg_zero(cmc, e->blk, XLPORT_POWER_SAVE)) rv = 1;
        else if (xlport_reg_zero(cmc, e->blk, XLPORT_MODE_REG)) rv = 2;
        else if (xlport_tsc_reset(cmc, e, settle, hold)) rv = 3;
        else if (xlport_reg_zero(cmc, e->blk, XLPORT_MAC_CONTROL)) rv = 4;

        if (rv) {
            printf("  blk %2d (pgw %2d %08x): FAILED at step %d\n",
                   e->blk, e->pgw, e->tsc_reg, rv);
            bad++;
        } else {
            ok++;
        }
        fflush(stdout);
    }
    printf("xlportbring: DONE ok %d bad %d\n", ok, bad);
    return bad ? 1 : 0;
}

/* ---- the XLPORT register mailbox -------------------------------------------
 *
 * TSC lane registers are NOT plain S-Channel reads. portmod_common_phy_sbus_
 * reg_read (portmod_common.c:393) writes a 4-word command into index 0 of
 * XLPORT_UCMEM_DATA and reads the same index back:
 *
 *   word0 = reg | (core_addr << 19) | (lane << 16)
 *   word1 = data (write) / 0 (read)
 *   word2 = 1 for write, 0 for read
 *   word3 = 0
 *
 * So a raw `memr 18 0 <n>` reads the MAILBOX, not lane storage -- reading zero
 * there says nothing about whether the block stores anything, which is what
 * TSC-ACKS-BUT-DOES-NOT-STORE-20260813.md got wrong.
 *
 * The result lands in word0. Positive control, straight off the wire: on a
 * working macro reg 0x0002 reads 0x0000600d, 0x0003 reads 0x00008770 and
 * 0x900e reads 0x000002d2. Those are fixed identity values -- unlike
 * SC_X4_RSLVD0 (0x0e05) they need no port configuration to be correct, so they
 * separate "the SerDes is alive" from "the SerDes is configured".
 */
static int sbus_reg_read(int cmc, int blk, uint32_t addr, uint32_t out[4])
{
    uint32_t e[4];

    e[0] = addr; e[1] = 0; e[2] = 0; e[3] = 0;      /* word2 = 0 -> read */
    if (mem_put(cmc, blk, 0, XLPORT_UCMEM_DATA, e, 4, 16)) return -1;
    if (mem_get(cmc, blk, 0, XLPORT_UCMEM_DATA, out, 4)) return -2;
    return 0;
}

/* portmod_common_phy_sbus_reg_write, portmod_common.c:330-345. The value word
 * is NOT the value: it is the low half in the top 16 bits and an INVERTED mask
 * of the high half in the bottom 16, so one word carries data and write-enable
 * together.
 *
 *   word1 = ((val & 0xffff) << 16) | ((~val & 0xffff0000) >> 16)
 *
 * Checked against the trace: writing 0x80008000 to the TSC uC control register
 * appears as data 0x80007fff, which is what this produces.
 */
static int sbus_reg_write(int cmc, int blk, uint32_t addr, uint32_t val)
{
    uint32_t e[4];

    e[0] = addr;
    e[1] = ((val & 0xffff) << 16) | ((~val & 0xffff0000) >> 16);
    e[2] = 1;                                       /* word2 = 1 -> write */
    e[3] = 0;
    return mem_put(cmc, blk, 0, XLPORT_UCMEM_DATA, e, 4, 16);
}

/* The lane field is THREE bits, not two -- soc_sbus_mdio_addr uses
 * (phy_reg >> 16) & 0x7, and the SDK's own PMD sequence addresses lane 6
 * (trace op 71,689, `006ea000`). A 2-bit mask silently aliases those onto
 * lane 2. The identity sweep only uses lanes 0-3 and was unaffected. */
static uint32_t sbus_reg_addr(const struct xlport_tsc *e, int lane, uint32_t reg)
{
    return e->mbox | ((uint32_t)(lane & 7) << 16)
                   | (reg & 0xffff) | (reg & 0xf8000000);
}

static int do_sbusrd(int cmc, int blk, int lane, uint32_t reg)
{
    const struct xlport_tsc *e = xlport_lookup(blk);
    uint32_t out[4], addr;
    int rv;

    if (!e) {
        printf("sbusrd: XLPORT block %d is not in the map\n", blk);
        return 1;
    }
    addr = sbus_reg_addr(e, lane, reg);
    printf("sbusrd: blk %d lane %d reg 0x%04x (mailbox 0x%08x) ... ",
           blk, lane, reg & 0xffff, addr);
    fflush(stdout);
    rv = sbus_reg_read(cmc, blk, addr, out);
    if (rv) { printf("FAILED (%d)\n", rv); return 1; }
    printf("0x%08x  [%08x %08x %08x %08x]\n", out[0],
           out[0], out[1], out[2], out[3]);
    return 0;
}

static int do_sbuswr(int cmc, int blk, int lane, uint32_t reg, uint32_t val)
{
    const struct xlport_tsc *e = xlport_lookup(blk);
    uint32_t out[4], addr;

    if (!e) {
        printf("sbuswr: XLPORT block %d is not in the map\n", blk);
        return 1;
    }
    addr = sbus_reg_addr(e, lane, reg);
    printf("sbuswr: blk %d lane %d reg 0x%04x <- 0x%08x (mailbox 0x%08x) ... ",
           blk, lane, reg & 0xffff, val, addr);
    fflush(stdout);
    if (sbus_reg_write(cmc, blk, addr, val)) { printf("FAILED\n"); return 1; }
    if (sbus_reg_read(cmc, blk, addr, out)) { printf("written, read-back FAILED\n"); return 1; }
    printf("read-back 0x%08x%s\n", out[0],
           (out[0] & 0xffff) == (val & 0xffff) ? "  (low half matches)" : "");
    return 0;
}

/* Sweep the identity registers on every mapped block and lane. */
static int do_sbusid(int cmc)
{
    static const uint32_t regs[3] = { 0x0002, 0x0003, 0x900e };
    static const uint32_t want[3] = { 0x0000600d, 0x00008770, 0x000002d2 };
    int i, lane, r, alive = 0, total = 0;

    printf("sbusid: identity sweep, expecting 600d / 8770 / 02d2\n");
    for (i = 0; i < XLPORT_MAP_N; i++) {
        const struct xlport_tsc *e = &xlport_tsc_map[i];
        for (lane = 0; lane < 4; lane++) {
            uint32_t got[3] = { 0, 0, 0 };
            int fail = 0, match = 0;
            for (r = 0; r < 3; r++) {
                uint32_t out[4];
                if (sbus_reg_read(cmc, e->blk, sbus_reg_addr(e, lane, regs[r]),
                                  out)) { fail = 1; break; }
                got[r] = out[0];
                if (out[0] == want[r]) match++;
            }
            total++;
            if (fail) {
                printf("  blk %2d lane %d  MAILBOX FAILED\n", e->blk, lane);
                continue;
            }
            if (match) alive++;
            printf("  blk %2d lane %d  %08x %08x %08x  %s\n", e->blk, lane,
                   got[0], got[1], got[2],
                   match == 3 ? "*** ALIVE ***" : match ? "partial" : "");
            fflush(stdout);
        }
    }
    printf("sbusid: %d/%d lanes returned identity data\n", alive, total);
    return alive ? 0 : 1;
}

/* ---- TSC microcontroller bring-up ------------------------------------------
 *
 * The 08-13 microcode run failed on more than ACCESS_MODE. The SDK wraps the
 * load in a lane-register sequence that puts the DW8051 in reset and points
 * its RAM at the parallel bus first, and switches the core on afterwards.
 * Transcribed from the attributed trace, block 36 (= real 18), ops
 * 70,949-70,976 and 71,667-71,670. Every macro gets the identical sequence.
 *
 *   pre    9010 <- 0000/ffff   9010 <- 0003/ffff   9000 <- 6000/e000
 *          d0f4 <- 0271/03ff   d20c <- 0000/0002
 *          d20d <- 0001/0001, 0002/0002, 0000/0002, 0002/0002
 *          d202 <- 0000/0180   d201 <- 0000/ffff
 *          d202 <- 0000/8000, 8000/8000, 0000/8000    <- 8051 held in reset
 *          read d205 (the SDK sees 0x8000)
 *          9010 <- 0100/0100   d20c <- 0005/0007      <- RAM to parallel bus
 *   load   ACCESS_MODE=1, write the ucode, ACCESS_MODE=0
 *   post   d20c <- 0002/0007   9010 <- 0000/0100
 *   start  d083 <- 0001/ffff
 *          d0f4 <- 8000/8000                          <- UC_ACTIVE
 *          d202 <- 0010/0010
 *          d083 <- 0000/ffff
 *
 * `d0f4` is DIG_TOP_USER_CTL0 and bit 15 is UC_ACTIVE -- eagle_uc_active_set
 * (eagle_cfg_seq.c:31) is what the SDK calls, and this is the write it makes.
 * Reading it back is therefore a direct check of whether the core is running.
 *
 * All of this is core-level, addressed through lane 0. `val` packs the write
 * mask in the high half and the data in the low half, as sbus_reg_write wants.
 *
 * eagle_tsc_ucode_init (eagle_cfg_seq.c:258) names what these bits are and,
 * crucially, that there is a **500 us wait** between asserting and clearing
 * micro_init_cmd. The trace cannot show a delay; the source can. Mapping:
 *
 *   d20d bit 0  micro_system_clk_en        set once
 *   d20d bit 1  micro_system_reset_n       1, 0, 1 -- toggled
 *   d201        micro_ram_address          0
 *   d202 bit 15 micro_init_cmd             0, 1, [500 us], 0
 *   d205 bit 15 micro_init_done            READ -- the SDK checks this and
 *                                          errors ERR_CODE_MICRO_INIT_NOT_DONE
 *
 * So the d205 read is not decoration: 0x8000 means the RAM initialised. That
 * makes this sequence self-checking part-way through, before the ucode is
 * written and long before UC_ACTIVE is set.
 */
struct lane_op {
    unsigned char devad;        /* 0 or 1 */
    unsigned char rd;           /* 1 = read and report, data/mask ignored */
    unsigned short reg;
    unsigned short data;
    unsigned short mask;
    unsigned int us;            /* wait after this op */
};

static const struct lane_op tsc_micro_pre[] = {
    { 0, 0, 0x9010, 0x0000, 0xffff, 0 },
    { 0, 0, 0x9010, 0x0003, 0xffff, 0 },
    { 0, 0, 0x9000, 0x6000, 0xe000, 0 },
    { 1, 0, 0xd0f4, 0x0271, 0x03ff, 0 },
    { 1, 0, 0xd20c, 0x0000, 0x0002, 0 },
    { 1, 0, 0xd20d, 0x0001, 0x0001, 0 },   /* micro_system_clk_en */
    { 1, 0, 0xd20d, 0x0002, 0x0002, 0 },   /* reset_n 1 */
    { 1, 0, 0xd20d, 0x0000, 0x0002, 0 },   /* reset_n 0 */
    { 1, 0, 0xd20d, 0x0002, 0x0002, 0 },   /* reset_n 1 */
    { 1, 0, 0xd202, 0x0000, 0x0180, 0 },
    { 1, 0, 0xd201, 0x0000, 0xffff, 0 },   /* micro_ram_address = 0 */
    { 1, 0, 0xd202, 0x0000, 0x8000, 0 },   /* micro_init_cmd = 0 */
    { 1, 0, 0xd202, 0x8000, 0x8000, 500 }, /* micro_init_cmd = 1, THE 500 us */
    { 1, 0, 0xd202, 0x0000, 0x8000, 0 },   /* micro_init_cmd = 0 */
    { 1, 1, 0xd205, 0x0000, 0x0000, 0 },   /* micro_init_done -- expect 0x8000 */
    { 0, 0, 0x9010, 0x0100, 0x0100, 0 },
    { 1, 0, 0xd20c, 0x0005, 0x0007, 0 },
};

static const struct lane_op tsc_micro_post[] = {
    { 1, 0, 0xd20c, 0x0002, 0x0007, 0 },
    { 0, 0, 0x9010, 0x0000, 0x0100, 0 },
    { 1, 0, 0xd083, 0x0001, 0xffff, 0 },
    { 1, 0, 0xd0f4, 0x8000, 0x8000, 0 },   /* UC_ACTIVE = 1 */
    { 1, 0, 0xd202, 0x0010, 0x0010, 0 },
    { 1, 0, 0xd083, 0x0000, 0xffff, 0 },
};

static int lane_ops_run(int cmc, const char *tag,
                        const struct lane_op *ops, int n)
{
    int i, j, ok = 0, bad = 0;

    printf("%s: %d ops on each of %d XLPORT macros\n", tag, n, XLPORT_MAP_N);
    for (i = 0; i < XLPORT_MAP_N; i++) {
        const struct xlport_tsc *e = &xlport_tsc_map[i];
        for (j = 0; j < n; j++) {
            const struct lane_op *o = &ops[j];
            uint32_t reg = o->reg | ((uint32_t)o->devad << 27);
            uint32_t addr = sbus_reg_addr(e, 0, reg);
            uint32_t out[4];

            if (o->rd) {
                if (sbus_reg_read(cmc, e->blk, addr, out)) { bad++; continue; }
                printf("  blk %2d read 0x%04x -> 0x%08x%s\n", e->blk, o->reg,
                       out[0],
                       o->reg == 0xd205
                           ? ((out[0] & 0x8000) ? "  micro_init_done"
                                                : "  ** INIT NOT DONE **")
                           : "");
                ok++;
                if (o->us) usleep(o->us);
                continue;
            }
            if (sbus_reg_write(cmc, e->blk, addr,
                               ((uint32_t)o->mask << 16) | o->data)) {
                printf("  blk %2d reg 0x%04x: WRITE FAILED\n", e->blk, o->reg);
                bad++;
            } else {
                ok++;
            }
            if (o->us) usleep(o->us);
        }
        fflush(stdout);
    }
    printf("%s: DONE ok %d bad %d\n", tag, ok, bad);
    return bad ? 1 : 0;
}

/* ---- PMD core init -- the SDK's post-load lane sequence --------------------
 *
 * The 55 mailbox ops each macro receives after its microcode is loaded and
 * before the port layer starts: refclk and PLL configuration, lane swap, the
 * TX FIR table, and the core/lane datapath soft-reset releases.
 *
 * Every previous replay excluded these on the grounds that they are
 * read-modify-write against live lane state, so a blind replay would be
 * meaningless. That was true while we could not read a lane. It is not true
 * now, which is what makes this worth doing.
 *
 * The table is per-block because the sequence is not uniform: blocks 15 and 43
 * differ from the other 22 in exactly five ops -- 0x9003 (lane swap, 0x1b vs
 * 0xe4) and 0xd0fb/0xd0fc/0xd0fd (PLL and refclk dividers). That is the
 * per-port variation the FDL records too.
 *
 * word1 is replayed verbatim rather than re-derived from data and mask; the
 * packing does not need decoding to be reproduced faithfully.
 *
 * NOTE: the first four ops are the same activate step `tscmicropost` ends
 * with (d083/d0f4/d202/d083). Running both repeats them with identical
 * values, which is harmless.
 */
#include "tsc-pmd-init.h"

static int sbus_raw(int cmc, int blk, uint32_t addr, uint32_t word1, int mode,
                    uint32_t out[4])
{
    uint32_t e[4];

    e[0] = addr; e[1] = word1; e[2] = (uint32_t)mode; e[3] = 0;
    if (mem_put(cmc, blk, 0, XLPORT_UCMEM_DATA, e, 4, 16)) return -1;
    if (mode == 0 && mem_get(cmc, blk, 0, XLPORT_UCMEM_DATA, out, 4)) return -2;
    return 0;
}

static int do_pmdinit(int cmc)
{
    int i, j, ok = 0, bad = 0;
    int n = (int)(sizeof(pmd_init_seq) / sizeof(pmd_init_seq[0]));

    printf("pmdinit: PMD core init on %d XLPORT macros\n", n);
    for (i = 0; i < n; i++) {
        const struct pmd_seq *s = &pmd_init_seq[i];
        const struct xlport_tsc *e = xlport_lookup(s->blk);
        if (!e) { bad++; continue; }

        for (j = 0; j < s->n; j++) {
            const struct pmd_op *o = &s->ops[j];
            uint32_t out[4];

            if (sbus_raw(cmc, s->blk, e->mbox + o->delta, o->word1,
                         o->mode, out)) {
                printf("  blk %2d op %2d (delta 0x%08x): FAILED\n",
                       s->blk, j, o->delta);
                bad++;
            } else {
                ok++;
            }
        }
        fflush(stdout);
    }
    printf("pmdinit: DONE ok %d bad %d\n", ok, bad);
    return bad ? 1 : 0;
}

/* ---- microcode load over the register interface ----------------------------
 *
 * The DMA replay cannot be checked. `ucmemacc 1` turns the UCMEM into a
 * write-only parallel-bus port, so nothing can be read back, and its byte
 * order comes from _portmod_dma_buf_alloc's arr_pos tables rather than from
 * anything we can verify -- we replay the captured DMA buffer verbatim and
 * simply hope the S-Channel delivers it the way the DMA engine would.
 *
 * eagle_tsc_ucode_mdio_load (eagle_tsc_dv_functions_c.h:826) is the SDK's
 * other path and it is fully specified in source: 16 bits at a time through
 * micro_ram_wrdata, little-endian, no DMA framing to guess. Better still,
 * eagle_tsc_ucode_load_verify reads it straight back out of the program RAM.
 * That turns "did the image land?" from an assumption into a measurement.
 *
 * XLPORT_WC_UCMEM_CTRL is not involved at all -- this route never leaves
 * mailbox mode.
 *
 * d202 bit map, from the wrc_ macros in eagle_tsc_fields.h (addr, mask, shift):
 *   0 run   1 stop   2 read   3 write   4 dw8051_reset_n
 *   6 ram_read_autoinc_en     7:8 ram_access_mode     9 byte_mode
 *   15 init_cmd
 * d200 ram_count   d201 ram_address   d203 wrdata   d204 rddata   d205 status
 * d20d: 0 system_clk_en   1 system_reset_n
 */
#define MU_CTRL      0xd202
#define MU_RUN       0x0001
#define MU_STOP      0x0002
#define MU_READ      0x0004
#define MU_WRITE     0x0008
#define MU_8051RSTN  0x0010
#define MU_AUTOINC   0x0040
#define MU_ACCMODE   0x0180
#define MU_BYTEMODE  0x0200
#define MU_INITCMD   0x8000

static int mu_wr(int cmc, const struct xlport_tsc *e, uint32_t reg,
                 uint32_t data, uint32_t mask)
{
    uint32_t addr = sbus_reg_addr(e, 0, reg | (1u << 27));
    return sbus_reg_write(cmc, e->blk, addr, (mask << 16) | (data & 0xffff));
}

/* Same, on devad 0 -- the 0x90xx PMD control registers live there. */
static int mu_wr0(int cmc, const struct xlport_tsc *e, uint32_t reg,
                  uint32_t data, uint32_t mask)
{
    uint32_t addr = sbus_reg_addr(e, 0, reg);
    return sbus_reg_write(cmc, e->blk, addr, (mask << 16) | (data & 0xffff));
}

static int mu_rd(int cmc, const struct xlport_tsc *e, uint32_t reg,
                 uint32_t *val)
{
    uint32_t addr = sbus_reg_addr(e, 0, reg | (1u << 27));
    uint32_t out[4];

    if (sbus_reg_read(cmc, e->blk, addr, out)) return -1;
    *val = out[0] & 0xffff;
    return 0;
}

static unsigned env_u(const char *name, unsigned dflt)
{
    const char *s = getenv(name);
    return s ? (unsigned)strtoul(s, NULL, 0) : dflt;
}

/* How much of the image to load and verify. UCODE_WORDS caps it so a pacing
 * sweep costs seconds instead of a boot; 0 means the whole image. Load and
 * verify must agree, so both go through here. */
static unsigned ucode_span(unsigned len)
{
    unsigned padded = (len + 7) & ~7u;
    unsigned words = env_u("UCODE_WORDS", 0);

    if (words && words * 2 < padded) padded = (words * 2 + 7) & ~7u;
    return padded;
}

/* The PMD core enable that must precede any micro RAM work: take the core out
 * of reset and program heartbeat_count_1us. These are the first four ops of
 * tscmicropre, and without them micro_init_cmd cannot complete -- the first
 * ucodemdio run reported "micro_init_done NOT SET" on every macro purely
 * because it was invoked without them. Doing it here makes the command
 * self-contained; repeating it after tscmicropre is harmless, same values. */
static int pmd_core_enable(int cmc, const struct xlport_tsc *e)
{
    if (mu_wr0(cmc, e, 0x9010, 0x0000, 0xffff)) return -1;
    if (mu_wr0(cmc, e, 0x9010, 0x0003, 0xffff)) return -1;   /* por/core_dp */
    if (mu_wr0(cmc, e, 0x9000, 0x6000, 0xe000)) return -1;
    if (mu_wr(cmc, e, 0xd0f4, 0x0271, 0x03ff)) return -1;    /* heartbeat 1us */
    return 0;
}

static int ucode_mdio_load(int cmc, const struct xlport_tsc *e,
                           const unsigned char *img, unsigned len)
{
    unsigned padded = ucode_span(len);
    unsigned wr_us = env_u("UCODE_WR_US", 0);
    unsigned i;
    uint32_t v;

    if (pmd_core_enable(cmc, e)) return -1;
    if (mu_wr(cmc, e, MU_CTRL, 0, MU_8051RSTN)) return -1;    /* 8051 in reset */
    if (mu_wr(cmc, e, 0xd20d, 1, 0x0001)) return -1;          /* clk_en = 1 */
    if (mu_wr(cmc, e, 0xd20d, 2, 0x0002)) return -1;          /* reset_n 1 */
    if (mu_wr(cmc, e, 0xd20d, 0, 0x0002)) return -1;          /* reset_n 0 */
    if (mu_wr(cmc, e, 0xd20d, 2, 0x0002)) return -1;          /* reset_n 1 */
    if (mu_wr(cmc, e, MU_CTRL, 0, MU_ACCMODE)) return -1;     /* program mem */
    if (mu_wr(cmc, e, MU_CTRL, 0, MU_BYTEMODE)) return -1;    /* word mode */
    if (mu_wr(cmc, e, 0xd201, 0, 0xffff)) return -1;          /* ram_address */

    if (mu_wr(cmc, e, MU_CTRL, 0, MU_INITCMD)) return -1;
    if (mu_wr(cmc, e, MU_CTRL, MU_INITCMD, MU_INITCMD)) return -1;
    if (mu_wr(cmc, e, MU_CTRL, 0, MU_INITCMD)) return -1;
    usleep(300);                                              /* THE 300 us */

    if (mu_rd(cmc, e, 0xd205, &v)) return -1;
    if (!(v & 0x8000)) {
        printf("  blk %2d: micro_init_done NOT SET (d205=0x%04x)\n", e->blk, v);
        return -2;
    }

    if (mu_wr(cmc, e, 0xd200, padded - 1, 0xffff)) return -1; /* ram_count */
    if (mu_wr(cmc, e, 0xd201, 0, 0xffff)) return -1;          /* ram_address */
    if (mu_wr(cmc, e, MU_CTRL, 0, MU_STOP)) return -1;
    if (mu_wr(cmc, e, MU_CTRL, MU_WRITE, MU_WRITE)) return -1;
    if (mu_wr(cmc, e, MU_CTRL, MU_RUN, MU_RUN)) return -1;

    for (i = 0; i < padded; i += 2) {
        uint32_t lo = (i < len) ? img[i] : 0;
        uint32_t hi = (i + 1 < len) ? img[i + 1] : 0;
        if (mu_wr(cmc, e, 0xd203, (hi << 8) | lo, 0xffff)) {
            printf("  blk %2d: wrdata failed at byte %u\n", e->blk, i);
            return -1;
        }
        if (wr_us) usleep(wr_us);
    }

    if (mu_wr(cmc, e, MU_CTRL, 0, MU_WRITE)) return -1;
    if (mu_wr(cmc, e, MU_CTRL, 0, MU_RUN)) return -1;
    if (mu_wr(cmc, e, MU_CTRL, MU_STOP, MU_STOP)) return -1;

    if (mu_rd(cmc, e, 0xd205, &v)) return -1;
    printf("  blk %2d: loaded %u bytes (%u us/word), d205=0x%04x\n",
           e->blk, padded, wr_us, v);
    if (mu_wr(cmc, e, MU_CTRL, 0, MU_STOP)) return -1;
    return 0;
}

/* eagle_tsc_ucode_load_verify -- read the program RAM back and compare. */
static int ucode_mdio_verify(int cmc, const struct xlport_tsc *e,
                             const unsigned char *img, unsigned len)
{
    unsigned padded = ucode_span(len);
    unsigned i, bad = 0;
    uint32_t v;

    if (mu_wr(cmc, e, MU_CTRL, 0, MU_BYTEMODE)) return -1;       /* word mode */
    if (mu_wr(cmc, e, MU_CTRL, 0x0080, MU_ACCMODE)) return -1;   /* mode 1 */
    if (mu_wr(cmc, e, MU_CTRL, MU_AUTOINC, MU_AUTOINC)) return -1;
    if (mu_wr(cmc, e, 0xd201, 0, 0xffff)) return -1;

    for (i = 0; i < padded; i += 2) {
        uint32_t lo = (i < len) ? img[i] : 0;
        uint32_t hi = (i + 1 < len) ? img[i + 1] : 0;
        uint32_t want = (hi << 8) | lo;

        if (mu_rd(cmc, e, 0xd204, &v)) return -1;
        if (v != want) {
            if (bad < 4)
                printf("  blk %2d: MISMATCH at 0x%04x: read 0x%04x want 0x%04x\n",
                       e->blk, i, v, want);
            bad++;
        }
    }
    if (mu_wr(cmc, e, MU_CTRL, 0, MU_AUTOINC)) return -1;
    /* the SDK leaves access mode at 2 (data RAM) when verify finishes */
    if (mu_wr(cmc, e, MU_CTRL, 0x0100, MU_ACCMODE)) return -1;
    printf("  blk %2d: verify %u words, %u mismatches%s\n", e->blk, padded / 2,
           bad, bad ? "" : "  *** IMAGE IS IN THE RAM ***");
    return bad ? 1 : 0;
}

static unsigned char *slurp(const char *path, unsigned *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long n;

    if (!f) { printf("cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        printf("cannot read %s\n", path); fclose(f); free(b); return NULL;
    }
    fclose(f);
    *len = (unsigned)n;
    return b;
}

static int do_ucodemdio(int cmc, const char *path, int only, int verify)
{
    unsigned len;
    unsigned char *img = slurp(path, &len);
    int i, ok = 0, bad = 0;

    if (!img) return 1;
    printf("ucodemdio: %s, %u bytes, %s\n", path, len,
           verify ? "verify only" : "load");
    for (i = 0; i < XLPORT_MAP_N; i++) {
        const struct xlport_tsc *e = &xlport_tsc_map[i];
        int rv;

        if (only >= 0 && e->blk != only) continue;
        rv = verify ? ucode_mdio_verify(cmc, e, img, len)
                    : ucode_mdio_load(cmc, e, img, len);
        if (rv) bad++; else ok++;
        fflush(stdout);
    }
    free(img);
    printf("ucodemdio: DONE ok %d bad %d\n", ok, bad);
    return bad ? 1 : 0;
}

/* ---- port layer: XLMAC + PCS speed control ---------------------------------
 *
 * Every port of every macro: XLMAC configuration (the 64-bit block at
 * 0x0006xx00) plus the PCS speed-control setup. 132 ops per port --
 * 18 macros x 4 ports for the 10G lanes, 6 macros x 1 port for the 40G ones.
 *
 * Ports are segmented on the `_pm4x10_pm_port_init` entry. Segmenting on the
 * XLPORT_SOFT_RESET marker looks right and is not: that register is written
 * by pm4x10_port_soft_reset_toggle too, which truncated each port to about a
 * third of its ops. The truncated version still resolved the speed on lane 0,
 * which is exactly why the mistake was easy to miss.
 *
 * This is the step after `lanerst`, and it is where SC_X4_RSLVD0 (0xc072)
 * becomes meaningful: that register is a *resolved* speed status produced by
 * the PCS speed-control FSM, which is why it reads 0 until this runs. Every
 * earlier reading of it as a success signal was reading it too early.
 *
 * Not uniform: 7 of 24 macros differ, which is per-port speed and lane
 * configuration showing through, so each block carries its own table.
 *
 * The sequence includes its own PMD_X4_CTL toggle (0xc010 low then high) --
 * the SDK does `lanerst` here, in the port layer. Running lanerst first is
 * harmless and is what makes the microcontroller come up before this point.
 */
#include "tsc-port-init.h"

static int do_portinit(int cmc, int only)
{
    int i, j, ok = 0, bad = 0;
    int n = (int)(sizeof(port_init_seq) / sizeof(port_init_seq[0]));

    printf("portinit: XLMAC + PCS speed control, all ports\n");
    for (i = 0; i < n; i++) {
        const struct port_seq *s = &port_init_seq[i];
        const struct xlport_tsc *e = xlport_lookup(s->blk);

        if (!e) { bad++; continue; }
        if (only >= 0 && s->blk != only) continue;

        for (j = 0; j < s->n; j++) {
            const struct port_op *o = &s->ops[j];
            uint32_t out[4], data[2];
            int rv;

            if (o->kind == 0) {
                data[0] = o->d0; data[1] = o->d1;
                rv = reg_put(cmc, s->blk, 0, o->addr, data, o->nwords,
                             (uint32_t)o->nwords * 4);
            } else {
                rv = sbus_raw(cmc, s->blk, e->mbox + o->addr, o->d0,
                              o->mode, out);
            }
            if (rv) {
                printf("  blk %2d op %3d (kind %d addr 0x%08x): FAILED\n",
                       s->blk, j, o->kind, o->addr);
                bad++;
            } else {
                ok++;
            }
        }
        fflush(stdout);
    }
    printf("portinit: DONE ok %d bad %d\n", ok, bad);
    return bad ? 1 : 0;
}

/* ---- portfull: every XLPORT write the SDK issues, in its own order --------
 *
 * `portinit` above is one bring-up pass per port -- 11,616 ops, 14% of what
 * the SDK does to these blocks after the PMD stage. The rest is ten further
 * phases per macro, including a 976-op reconfiguration and a 1,520-op one, and
 * it is where the difference has to be: TX-IS-THE-FAULT-20260814.md measured
 * our receiver locking to the AS5610 while its receiver never locks to us,
 * with our transmitter powered, clocked and out of reset.
 *
 * Order is global across blocks, exactly as traced. Reads are dropped -- they
 * carry no state -- so this is 80,216 writes. Run it INSTEAD of portinit; it
 * begins at the same op index.
 *
 * `only` restricts to one block, and PORTFULL_MAX caps the op count, both for
 * bisecting which phase matters once this either works or does not.
 */
#include "tsc-port-full.h"

static int do_portfull(int cmc, int only)
{
    int i, ok = 0, bad = 0, skipped = 0;
    int n = (int)(sizeof(port_full_ops) / sizeof(port_full_ops[0]));
    const char *lim = getenv("PORTFULL_MAX");
    int max = lim ? atoi(lim) : n;

    if (max > n) max = n;
    printf("portfull: %d of %d XLPORT writes, SDK order\n", max, n);
    for (i = 0; i < max; i++) {
        const struct port_full_op *o = &port_full_ops[i];
        const struct xlport_tsc *e;
        uint32_t out[4], data[2];
        int rv;

        if (only >= 0 && o->blk != only) { skipped++; continue; }
        e = xlport_lookup(o->blk);
        if (!e) { bad++; continue; }

        if (o->kind == 0) {
            data[0] = o->d0; data[1] = o->d1;
            rv = reg_put(cmc, o->blk, 0, o->addr, data, o->nwords,
                         (uint32_t)o->nwords * 4);
        } else {
            rv = sbus_raw(cmc, o->blk, e->mbox + o->addr, o->d0,
                          o->mode, out);
        }
        if (rv) {
            if (bad < 10)
                printf("  op %d blk %2d (kind %d addr 0x%08x): FAILED\n",
                       i, o->blk, o->kind, o->addr);
            bad++;
        } else {
            ok++;
        }
        if ((i & 0x3fff) == 0x3fff) { printf("  .. %d\n", i + 1); fflush(stdout); }
    }
    printf("portfull: DONE ok %d bad %d skipped %d\n", ok, bad, skipped);
    return bad ? 1 : 0;
}

/* ---- chipreplay: the pipeline blocks, the way portfull did the macros ------
 *
 * EPIPE (block 2, 110,014 writes), IPIPE (block 1, 67,309) and MMU (block 3,
 * 54,409) are what a frame has to cross, and nothing had ever replayed them.
 * The hand-ported substitutes -- tdminit, llsinit, thdinit, the CFAP poke --
 * come to a few thousand writes between them, and thdinit NACKs 301 of its own.
 *
 * Split in two around the port layer, because the microcode load in between
 * goes over SBUS DMA and cannot come from an S-Channel trace:
 *
 *   bring -> chipinit -> ucodemdio/tscmicropost/pmdinit/lanerst
 *         -> portfull -> chippost -> polfix/macen/sdclear
 *
 * CHIPREPLAY_MAX caps the op count for bisecting, and `only` restricts to one
 * block.
 */
#include "chip-replay.h"

static int do_chipreplay(int cmc, int from, int to, int only, const char *tag)
{
    int i, ok = 0, bad = 0, skipped = 0;
    const char *lim = getenv("CHIPREPLAY_MAX");
    int max = lim ? atoi(lim) : 0;

    if (max > 0 && to > from + max) to = from + max;
    printf("%s: ops %d..%d of %d\n", tag, from, to,
           (int)(sizeof(chip_ops) / sizeof(chip_ops[0])));
    for (i = from; i < to; i++) {
        const struct chip_op *o = &chip_ops[i];
        int rv;

        if (only >= 0 && o->blk != only) { skipped++; continue; }
        if (o->kind == 0)
            rv = reg_put(cmc, o->blk, o->acc, o->addr,
                         &chip_words[o->doff], o->nwords, o->dlen);
        else
            rv = mem_put(cmc, o->blk, o->acc, o->addr,
                         &chip_words[o->doff], o->nwords, o->dlen);
        if (rv) {
            if (bad < 8)
                printf("  op %d blk %d acc %d kind %d addr 0x%08x: FAILED\n",
                       i, o->blk, o->acc, o->kind, o->addr);
            bad++;
        } else {
            ok++;
        }
        if ((i & 0xffff) == 0xffff) { printf("  .. %d\n", i + 1); fflush(stdout); }
    }
    printf("%s: DONE ok %d bad %d skipped %d\n", tag, ok, bad, skipped);
    return bad ? 1 : 0;
}

/* ---- memsnap / memdiff: find the drop point by diffing what moves ---------
 *
 * TD2+ has no RDBGC, and our register dictionary stores libStrataApi symbol
 * addresses rather than S-Channel ones, so there is no name to look up. The
 * memory dictionary does carry real chip bases, so: snapshot every plausible
 * counter memory, do one thing, snapshot again, and print what changed.
 *
 *   scdreset memsnap /tmp/a          28,530 reads, ~56k words
 *   <inject a frame, or wait for traffic>
 *   scdreset memsnap /tmp/b
 *   scdreset memdiff /tmp/a /tmp/b
 *
 * Reads that NACK are recorded as a sentinel and excluded from the diff, so a
 * memory this chip does not implement cannot masquerade as a change.
 */
#include "counter-scan.h"

#define SNAP_MAGIC 0x534e4150u          /* "SNAP" */
#define SNAP_BAD   0xdeadf00du          /* read failed -- not a value */

static int do_memsnap(int cmc, const char *path)
{
    uint32_t *buf, hdr[4];
    size_t off = 0;
    int i, j, bad = 0;
    FILE *f;

    if (!(buf = malloc(sizeof(uint32_t) * CTR_SCAN_WORDS))) {
        printf("memsnap: OOM\n");
        return 1;
    }
    for (i = 0; i < (int)(sizeof(ctr_mems) / sizeof(ctr_mems[0])); i++) {
        const struct ctr_mem *m = &ctr_mems[i];
        for (j = 0; j < (int)m->count; j++) {
            if (mem_get(cmc, m->blk, 0, m->base + j, &buf[off], m->words)) {
                int k;
                for (k = 0; k < m->words; k++) buf[off + k] = SNAP_BAD;
                bad++;
            }
            off += m->words;
        }
    }
    if (!(f = fopen(path, "wb"))) { perror(path); free(buf); return 1; }
    hdr[0] = SNAP_MAGIC;
    hdr[1] = CTR_SCAN_WORDS;
    hdr[2] = (uint32_t)(sizeof(ctr_mems) / sizeof(ctr_mems[0]));
    hdr[3] = CTR_SCAN_LIMIT;
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(buf, sizeof(uint32_t), CTR_SCAN_WORDS, f);
    fclose(f);
    printf("memsnap: %s -- %d memories, %d reads, %d words, %d unreadable\n",
           path, hdr[2], CTR_SCAN_READS, CTR_SCAN_WORDS, bad);
    printf("  NOTE each memory scanned from index 0 for at most %d entries;\n"
           "  anything deeper is NOT covered by this snapshot.\n", CTR_SCAN_LIMIT);
    free(buf);
    return 0;
}

static uint32_t *snap_load(const char *path)
{
    uint32_t hdr[4], *buf;
    FILE *f = fopen(path, "rb");

    if (!f) { perror(path); return NULL; }
    if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != SNAP_MAGIC ||
        hdr[1] != CTR_SCAN_WORDS) {
        printf("%s: not a snapshot from this build (magic/size mismatch)\n", path);
        fclose(f);
        return NULL;
    }
    if (!(buf = malloc(sizeof(uint32_t) * CTR_SCAN_WORDS))) { fclose(f); return NULL; }
    if (fread(buf, sizeof(uint32_t), CTR_SCAN_WORDS, f) != CTR_SCAN_WORDS) {
        printf("%s: short read\n", path);
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    return buf;
}

/* `detail` caps per-entry lines. Comparing two snapshots of the SAME chip
 * across one action, the changed set is tiny and detail is what you want.
 * Comparing OUR bring-up against a snapshot of EOS, thousands of counters
 * differ simply because EOS has been forwarding -- there the useful reading is
 * structural: which memories one side has populated and the other has not. The
 * summary is always printed; detail is capped. */
static int do_memdiff(const char *pa, const char *pb, int detail)
{
    uint32_t *a = snap_load(pa), *b = NULL;
    size_t off = 0, base;
    int i, j, k, changed = 0, skipped = 0, shown = 0;

    if (!a) return 1;
    if (!(b = snap_load(pb))) { free(a); return 1; }

    printf("memdiff: %s -> %s\n", pa, pb);
    printf("  %-34s %6s %6s %6s\n", "memory", "nzA", "nzB", "differ");
    for (i = 0; i < (int)(sizeof(ctr_mems) / sizeof(ctr_mems[0])); i++) {
        const struct ctr_mem *m = &ctr_mems[i];
        int nza = 0, nzb = 0, nd = 0;

        base = off;
        for (j = 0; j < (int)m->count; j++, off += m->words) {
            int diff = 0, unread = 0, ga = 0, gb = 0;
            for (k = 0; k < m->words; k++) {
                if (a[off + k] == SNAP_BAD || b[off + k] == SNAP_BAD) unread = 1;
                if (a[off + k]) ga = 1;
                if (b[off + k]) gb = 1;
                if (a[off + k] != b[off + k]) diff = 1;
            }
            if (unread) { skipped++; continue; }
            nza += ga; nzb += gb; nd += diff;
        }
        changed += nd;
        if (!nza && !nzb && !nd) continue;
        printf("  %-34s %6d %6d %6d%s\n", m->name, nza, nzb, nd,
               (nza && !nzb) ? "   <- populated in A only" :
               (!nza && nzb) ? "   <- populated in B only" : "");
        /* detail, for the small-changed-set case */
        off = base;
        for (j = 0; j < (int)m->count && shown < detail; j++, off += m->words) {
            int diff = 0, unread = 0;
            for (k = 0; k < m->words; k++) {
                if (a[off + k] == SNAP_BAD || b[off + k] == SNAP_BAD) unread = 1;
                if (a[off + k] != b[off + k]) diff = 1;
            }
            if (unread || !diff) continue;
            printf("      [%4d]", j);
            for (k = 0; k < m->words; k++) printf(" %08x", a[off + k]);
            printf("  ->");
            for (k = 0; k < m->words; k++) printf(" %08x", b[off + k]);
            printf("\n");
            shown++;
        }
        off = base + (size_t)m->count * m->words;
    }
    printf("memdiff: %d entries changed, %d unreadable and skipped", changed, skipped);
    if (changed > shown) printf(" (%d detail lines suppressed)", changed - shown);
    printf("\n");
    free(a); free(b);
    return 0;
}

/* ---- regsnap / regdiff: the same instrument, for REGISTERS ---------------
 *
 * memsnap/memdiff only ever covered memories, because memories were the only
 * thing we could enumerate -- artifacts/bcm56860-registers.json holds
 * libStrataApi symbol addresses, not chip addresses. That blind spot shaped
 * days of work. tools/gen-reg-scan.py lifts the real addresses out of the
 * SDK's own tables; this reads them.
 *
 * One 32-bit word per register. Wider registers are truncated, which is fine
 * for a diff and keeps a 32-bit read from NACKing on a 64-bit register.
 */
#include "reg-scan.h"

static int do_regsnap(int cmc, const char *path)
{
    uint32_t *buf, hdr[4];
    size_t off = 0;
    int i, j, bad = 0;
    int n = (int)(sizeof(reg_ents) / sizeof(reg_ents[0]));
    FILE *f;

    if (!(buf = malloc(sizeof(uint32_t) * REG_SCAN_READS))) {
        printf("regsnap: OOM\n");
        return 1;
    }
    for (i = 0; i < n; i++) {
        const struct reg_ent *r = &reg_ents[i];
        for (j = 0; j < (int)r->count; j++, off++) {
            if (reg_get(cmc, r->blk, 0, r->addr + j, &buf[off], 1, 4)) {
                buf[off] = SNAP_BAD;
                bad++;
            }
        }
    }
    if (!(f = fopen(path, "wb"))) { perror(path); free(buf); return 1; }
    hdr[0] = SNAP_MAGIC; hdr[1] = REG_SCAN_READS; hdr[2] = (uint32_t)n; hdr[3] = 0;
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(buf, sizeof(uint32_t), REG_SCAN_READS, f);
    fclose(f);
    printf("regsnap: %s -- %d registers, %d reads, %d unreadable\n",
           path, n, REG_SCAN_READS, bad);
    free(buf);
    return 0;
}

static int do_regdiff(const char *pa, const char *pb, int detail)
{
    uint32_t hdr[4], *a, *b;
    size_t off = 0;
    int i, j, changed = 0, skipped = 0, shown = 0;
    int n = (int)(sizeof(reg_ents) / sizeof(reg_ents[0]));
    FILE *f;

    a = malloc(sizeof(uint32_t) * REG_SCAN_READS);
    b = malloc(sizeof(uint32_t) * REG_SCAN_READS);
    if (!a || !b) { printf("regdiff: OOM\n"); return 1; }
    for (i = 0; i < 2; i++) {
        const char *p = i ? pb : pa;
        uint32_t *d = i ? b : a;
        if (!(f = fopen(p, "rb"))) { perror(p); return 1; }
        if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != SNAP_MAGIC ||
            hdr[1] != REG_SCAN_READS) {
            printf("%s: not a regsnap from this build\n", p);
            fclose(f); return 1;
        }
        if (fread(d, sizeof(uint32_t), REG_SCAN_READS, f) != REG_SCAN_READS) {
            printf("%s: short read\n", p); fclose(f); return 1;
        }
        fclose(f);
    }

    printf("regdiff: %s -> %s\n", pa, pb);
    for (i = 0; i < n; i++) {
        const struct reg_ent *r = &reg_ents[i];
        for (j = 0; j < (int)r->count; j++, off++) {
            if (a[off] == SNAP_BAD || b[off] == SNAP_BAD) { skipped++; continue; }
            if (a[off] == b[off]) continue;
            changed++;
            if (shown++ < detail)
                printf("  %-46s [%3d] %08x -> %08x\n",
                       r->name, j, a[off], b[off]);
        }
    }
    printf("regdiff: %d registers changed, %d unreadable and skipped", changed, skipped);
    if (changed > shown) printf(" (%d suppressed)", changed - shown);
    printf("\n");
    free(a); free(b);
    return 0;
}

static int lane_wr(int cmc, int blk, int lane, uint32_t reg, uint32_t data,
                   uint32_t mask);
static int lane_rd(int cmc, int blk, int lane, uint32_t reg, uint32_t *val);

/* ---- RX polarity: apply EOS's golden d0d3 ---------------------------------
 *
 * `config.bcm` carries EOS's golden RX_PMD_DP_INVERT, swept across all 23
 * macros (AUDIT-2026-08-07.md §47), as `phy_xaui_rx_polarity_flip_N` keyed by
 * LOGICAL PORT -- and flags its own assumption that logical ports map to macro
 * lanes in order. That assumption is wrong: replaying our own bcm_init leaves
 * macros 20, 21, 27, 30 and 31 with no RX inversion where EOS applies one.
 *
 * This table sidesteps the whole question by keying on the XLPORT block and
 * lane directly, which is what the sweep actually measured. No port numbering,
 * no warp-core mapping, nothing to assume.
 *
 * Why it matters, from config.bcm's own note: with RX polarity wrong "the
 * 64b/66b decoder never locks onto real data, while the PCS still reports link
 * up and the MAC never sees a frame to call bad -- link=1, hi_ber=0,
 * BERCNT=0, every RX counter flat at zero."
 *
 * Block 53 is deliberately absent: the sweep covered 23 macros and this chip
 * has 24, so its golden pattern is unknown. It is left alone rather than
 * guessed.
 */
struct rxpol { unsigned char blk; unsigned char lane[4]; };

static const struct rxpol rxpol_golden[] = {
    { 15, { 0, 0, 0, 0 } }, { 18, { 1, 0, 1, 0 } }, { 20, { 1, 0, 1, 0 } },
    { 21, { 1, 0, 1, 0 } }, { 22, { 1, 0, 1, 0 } }, { 23, { 1, 0, 1, 0 } },
    { 27, { 1, 0, 1, 0 } }, { 28, { 1, 0, 1, 0 } }, { 30, { 0, 1, 0, 1 } },
    { 31, { 0, 1, 0, 1 } }, { 32, { 0, 1, 0, 1 } }, { 33, { 1, 0, 1, 0 } },
    { 35, { 0, 1, 0, 1 } }, { 36, { 0, 0, 0, 0 } }, { 37, { 0, 0, 0, 0 } },
    { 38, { 0, 0, 1, 0 } }, { 42, { 0, 0, 1, 0 } }, { 43, { 1, 1, 0, 1 } },
    { 45, { 1, 1, 0, 1 } }, { 46, { 0, 0, 0, 0 } }, { 47, { 0, 0, 0, 0 } },
    { 48, { 0, 0, 0, 0 } }, { 52, { 0, 0, 0, 0 } },
};
#define RXPOL_N ((int)(sizeof(rxpol_golden) / sizeof(rxpol_golden[0])))

/* Report RX_PMD_DP_INVERT per lane against the golden table. */
static int do_polstat(int cmc)
{
    int i, lane, ok = 0, bad = 0, unknown = 0, j;

    printf("polstat: RX_PMD_DP_INVERT (0xd0d3 bit 0) vs EOS golden\n");
    for (i = 0; i < XLPORT_MAP_N; i++) {
        const struct xlport_tsc *e = &xlport_tsc_map[i];
        const struct rxpol *g = NULL;
        int got[4];

        for (j = 0; j < RXPOL_N; j++)
            if (rxpol_golden[j].blk == e->blk) { g = &rxpol_golden[j]; break; }

        for (lane = 0; lane < 4; lane++) {
            uint32_t v = 0;
            got[lane] = lane_rd(cmc, e->blk, lane, 0xd0d3, &v) ? -1 : (int)(v & 1);
        }
        if (!g) {
            printf("  blk %2d  %d%d%d%d  (no golden entry -- left alone)\n",
                   e->blk, got[0], got[1], got[2], got[3]);
            unknown++;
            continue;
        }
        {
            int match = (got[0] == g->lane[0] && got[1] == g->lane[1] &&
                         got[2] == g->lane[2] && got[3] == g->lane[3]);
            printf("  blk %2d  got %d%d%d%d  want %d%d%d%d  %s\n", e->blk,
                   got[0], got[1], got[2], got[3],
                   g->lane[0], g->lane[1], g->lane[2], g->lane[3],
                   match ? "ok" : "MISMATCH");
            if (match) ok++; else bad++;
        }
        fflush(stdout);
    }
    printf("polstat: %d ok, %d mismatch, %d without a golden entry\n",
           ok, bad, unknown);
    return bad ? 1 : 0;
}

static int do_polfix(int cmc)
{
    int i, lane, ok = 0, bad = 0;

    printf("polfix: writing EOS's golden RX_PMD_DP_INVERT on %d macros\n",
           RXPOL_N);
    for (i = 0; i < RXPOL_N; i++) {
        const struct rxpol *g = &rxpol_golden[i];
        if (!xlport_lookup(g->blk)) { bad++; continue; }
        for (lane = 0; lane < 4; lane++) {
            if (lane_wr(cmc, g->blk, lane, 0xd0d3, g->lane[lane], 0x0001))
                bad++;
            else
                ok++;
        }
        fflush(stdout);
    }
    printf("polfix: DONE ok %d bad %d\n", ok, bad);
    return bad ? 1 : 0;
}

/* ---- PRBS: does our transmitter emit a recoverable stream? ----------------
 *
 * Et1 (to the AS5610) reaches RX_LOCK, so the receive path works. Et25 and
 * Et48 are cabled to each other on this switch; both show signal detect, so
 * both transmitters emit light, and neither receiver locks. Either our TX is
 * not producing a lockable stream, or something above the PMD is.
 *
 * PRBS separates those. The TLB (test logic block) generator feeds the PMD
 * directly, bypassing the PCS entirely. If the checker at the far end of a
 * fibre locks, the transmitter, the optics and the receiver are all good and
 * the fault is above the PMD. If it does not lock, the fault is at or below
 * the PMD.
 *
 * All registers devad 1, per lane (bcmi_tsce_xgxs_defs.h):
 *   TLB_TX_PRBS_GEN_CFG   0xd0e1  bit 0 PRBS_GEN_EN, bits 3:1 mode
 *   TLB_RX_PRBS_CHK_CFG   0xd0d1  bit 0 PRBS_CHK_EN, bits 3:1 mode
 *   TLB_RX_PRBS_CHK_LOCK_STS 0xd0d9  bit 0 PRBS_CHK_LOCK
 *   TLB_RX_PRBS_CHK_ERR_CNT_MSB/LSB 0xd0da / 0xd0db
 *   TLB_TX_TLB_TX_MISC_CFG 0xd0e3  bit 0 TX_PMD_DP_INVERT
 *   TLB_RX_TLB_RX_MISC_CFG 0xd0d3  bit 0 RX_PMD_DP_INVERT
 *
 * IMPORTANT: the FDL gives tx_lane and rx_lane SEPARATELY and they differ.
 * Et25 transmits on blk 28 lane 3 and receives on lane 0; Et48 transmits on
 * blk 35 lane 0 and receives on lane 3. So the fibre pairs
 * (28,3) -> (35,3) and (35,0) -> (28,0).
 */
static int lane_wr(int cmc, int blk, int lane, uint32_t reg, uint32_t data,
                   uint32_t mask)
{
    const struct xlport_tsc *e = xlport_lookup(blk);
    if (!e) return -1;
    return sbus_reg_write(cmc, blk, sbus_reg_addr(e, lane, reg | (1u << 27)),
                          (mask << 16) | (data & 0xffff));
}

static int lane_rd(int cmc, int blk, int lane, uint32_t reg, uint32_t *val)
{
    const struct xlport_tsc *e = xlport_lookup(blk);
    uint32_t out[4];
    if (!e) return -1;
    if (sbus_reg_read(cmc, blk, sbus_reg_addr(e, lane, reg | (1u << 27)), out))
        return -1;
    *val = out[0] & 0xffff;
    return 0;
}

/* Field layout, from the wrc_ macros in eagle_tsc_fields.h (addr, mask, shift):
 *   0xd0e1  bit 0 prbs_gen_en,  bits 3:1 prbs_gen_mode_sel,  bit 4 prbs_gen_inv
 *   0xd0d1  bit 0 prbs_chk_en,  bits 3:1 prbs_chk_mode_sel,  bit 4 prbs_chk_inv,
 *                               bits 6:5 prbs_chk_mode  <- the LOCK state machine
 *   0xd0d2  bit 0 dig_lpbk_en
 *
 * The first version of this command wrote only bits 3:0 and so never set
 * prbs_chk_mode. It also printed the raw 32-bit error count, where
 * eagle_tsc_prbs_err_count_state says **bit 31 is lock_lost** and the count is
 * the low 31 bits -- which is why a dead path reported 4294967295 rather than
 * an obvious "no lock, saturated".
 */
static void prbs_report(int cmc, int rb, int rl, const char *tag, int *locked)
{
    uint32_t lock = 0, hi = 0, lo = 0, cnt;

    lane_rd(cmc, rb, rl, 0xd0d9, &lock);
    lane_rd(cmc, rb, rl, 0xd0da, &hi);
    lane_rd(cmc, rb, rl, 0xd0db, &lo);
    cnt = (hi << 16) | lo;
    printf("  %s: lock=%d lock_lost=%d err=%u\n", tag, (lock & 1) ? 1 : 0,
           (cnt >> 31) & 1, cnt & 0x7fffffffu);
    if (locked) *locked = (lock & 1) ? 1 : 0;
}

static int prbs_setup(int cmc, int tb, int tl, int rb, int rl, int mode)
{
    /* generator: en | mode_sel, inv 0 */
    if (lane_wr(cmc, tb, tl, 0xd0e1, ((uint32_t)mode << 1) | 1, 0x001f)) {
        printf("prbs: enabling generator FAILED\n");
        return -1;
    }
    /* checker: en | mode_sel, inv 0, chk_mode 0 (self-synchronising) */
    if (lane_wr(cmc, rb, rl, 0xd0d1, ((uint32_t)mode << 1) | 1, 0x007f)) {
        printf("prbs: enabling checker FAILED\n");
        return -1;
    }
    return 0;
}

static int do_prbs(int cmc, int tb, int tl, int rb, int rl, int mode)
{
    uint32_t cfg = 0;
    int locked = 0;

    printf("prbs: gen blk %d lane %d -> chk blk %d lane %d, mode %d\n",
           tb, tl, rb, rl, mode);
    if (prbs_setup(cmc, tb, tl, rb, rl, mode)) return 1;
    usleep(200000);
    prbs_report(cmc, rb, rl, "sample 1", NULL);
    usleep(300000);
    prbs_report(cmc, rb, rl, "sample 2", &locked);

    lane_rd(cmc, tb, tl, 0xd0e3, &cfg);
    printf("  tx blk %d lane %d TX_PMD_DP_INVERT=%d\n", tb, tl, cfg & 1);
    lane_rd(cmc, rb, rl, 0xd0d3, &cfg);
    printf("  rx blk %d lane %d RX_PMD_DP_INVERT=%d\n", rb, rl, cfg & 1);
    printf("prbs: %s\n", locked ? "*** CHECKER LOCKED ***" : "no lock");
    return locked ? 0 : 1;
}

/* POSITIVE CONTROL. eagle_tsc_dig_lpbk (eagle_tsc_dv_functions_c.h:544) is a
 * single bit: dig_lpbk_en, 0xd0d2 bit 0. It loops the lane's transmit data
 * back into its own receiver inside the PMD, so a PRBS generator and checker
 * on the SAME lane MUST lock. If this does not lock, the fault is in our PRBS
 * setup and nothing can be concluded about any external path. */
static int do_prbslb(int cmc, int blk, int lane, int mode)
{
    int locked = 0;

    printf("prbslb: digital loopback positive control, blk %d lane %d, mode %d\n",
           blk, lane, mode);
    if (lane_wr(cmc, blk, lane, 0xd0d2, 1, 0x0001)) {
        printf("prbslb: enabling dig_lpbk FAILED\n");
        return 1;
    }
    if (prbs_setup(cmc, blk, lane, blk, lane, mode)) return 1;
    usleep(200000);
    prbs_report(cmc, blk, lane, "sample 1", NULL);
    usleep(300000);
    prbs_report(cmc, blk, lane, "sample 2", &locked);

    lane_wr(cmc, blk, lane, 0xd0e1, 0, 0x001f);
    lane_wr(cmc, blk, lane, 0xd0d1, 0, 0x007f);
    lane_wr(cmc, blk, lane, 0xd0d2, 0, 0x0001);
    printf("prbslb: %s\n", locked
           ? "*** LOCKED -- the PRBS path is good, external results are meaningful ***"
           : "NO LOCK -- our PRBS setup is wrong; external results mean nothing");
    return locked ? 0 : 1;
}

static int do_prbsoff(int cmc)
{
    int i, lane;
    for (i = 0; i < XLPORT_MAP_N; i++)
        for (lane = 0; lane < 4; lane++) {
            lane_wr(cmc, xlport_tsc_map[i].blk, lane, 0xd0e1, 0, 0x001f);
            lane_wr(cmc, xlport_tsc_map[i].blk, lane, 0xd0d1, 0, 0x007f);
            lane_wr(cmc, xlport_tsc_map[i].blk, lane, 0xd0d2, 0, 0x0001);
        }
    printf("prbsoff: generators and checkers disabled on all lanes\n");
    return 0;
}

/* ---- analog status: PLL lock, signal detect, RX lock ----------------------
 *
 * The first signals on this chip that are NOT a readback of our own writes.
 *
 *   PMD_X1_STS   0x9012  devad 0, per MACRO   bit 0 PLL_LOCK_STS
 *                                             bit 1 TX_CLK_VLD_STS
 *   PMD_X4_STS   0xc012  devad 0, per LANE    bit 0 RX_LOCK_STS
 *                                             bit 1 SIGNAL_DETECT_STS
 *                                             bit 2 RX_CLK_VLD_STS
 *   SIGDET_STS0  0xd0c8  devad 1, per LANE    bit 0 SIGNAL_DETECT
 *                                             bit 2 ENERGY_DETECT
 *                                             bit 4 SIGNAL_DETECT_RAW
 *
 * (bcmi_tsce_xgxs_defs.h:1199, :12638, :37417. In that header the top half of
 * the address is 0x0001 for PMD lane registers -- devad 1 -- and 0x0000 or
 * 0x0010 for the PCS x4/x1 spaces, which are devad 0.)
 *
 * PLL_LOCK is the one that matters here: it says the analog PLL has locked to
 * the 156.25 MHz reference, which no amount of register writing can fake.
 * SIGNAL_DETECT and RX_LOCK depend on a peer and will read 0 with nothing
 * plugged in -- that is the correct result, not a failure.
 */
static int do_linkstat(int cmc)
{
    int i, lane, pll = 0, sd = 0, rxlk = 0, lanes = 0;

    printf("linkstat: PLL_LOCK per macro; SIGNAL_DETECT / RX_LOCK per lane\n");
    for (i = 0; i < XLPORT_MAP_N; i++) {
        const struct xlport_tsc *e = &xlport_tsc_map[i];
        uint32_t out[4], x1 = 0;

        if (sbus_reg_read(cmc, e->blk, sbus_reg_addr(e, 0, 0x9012), out) == 0)
            x1 = out[0];
        else
            printf("  blk %2d  PMD_X1_STS READ FAILED\n", e->blk);
        if (x1 & 0x1) pll++;

        printf("  blk %2d  x1=0x%04x pll_lock=%d tx_clk=%d |", e->blk, x1,
               (x1 & 0x1) ? 1 : 0, (x1 & 0x2) ? 1 : 0);
        for (lane = 0; lane < 4; lane++) {
            uint32_t x4 = 0, sg = 0;

            if (sbus_reg_read(cmc, e->blk, sbus_reg_addr(e, lane, 0xc012),
                              out) == 0) x4 = out[0];
            if (sbus_reg_read(cmc, e->blk,
                              sbus_reg_addr(e, lane, 0xd0c8 | (1u << 27)),
                              out) == 0) sg = out[0];
            lanes++;
            if (x4 & 0x2) sd++;
            if (x4 & 0x1) rxlk++;
            printf(" L%d[sd=%d rx=%d clk=%d ed=%d]", lane,
                   (x4 & 0x2) ? 1 : 0, (x4 & 0x1) ? 1 : 0,
                   (x4 & 0x4) ? 1 : 0, (sg & 0x4) ? 1 : 0);
        }
        printf("\n");
        fflush(stdout);
    }
    printf("linkstat: pll_lock %d/%d macros; signal_detect %d/%d lanes; "
           "rx_lock %d/%d lanes\n", pll, XLPORT_MAP_N, sd, lanes, rxlk, lanes);
    return pll ? 0 : 1;
}

/* SC_X4_RSLVD0 (0xc072) -- the resolved speed status. 0x0e05 on a configured
 * lane; meaningless before the port layer runs. */
static int do_rslvd(int cmc)
{
    int i, lane, good = 0, total = 0;

    printf("rslvd: SC_X4_RSLVD0 (0xc072), expect 0x0e05 on a configured lane\n");
    for (i = 0; i < XLPORT_MAP_N; i++) {
        const struct xlport_tsc *e = &xlport_tsc_map[i];
        for (lane = 0; lane < 4; lane++) {
            uint32_t out[4];

            total++;
            if (sbus_reg_read(cmc, e->blk, sbus_reg_addr(e, lane, 0xc072), out)) {
                printf("  blk %2d lane %d  READ FAILED\n", e->blk, lane);
                continue;
            }
            if (out[0]) good++;
            if (e->blk == 18 || out[0])
                printf("  blk %2d lane %d  0x%08x%s\n", e->blk, lane, out[0],
                       out[0] == 0x0e05 ? "  *** RESOLVED 0x0e05 ***" : "");
        }
        fflush(stdout);
    }
    printf("rslvd: %d/%d lanes non-zero\n", good, total);
    return good ? 0 : 1;
}

/* ---- PMD per-lane reset release -- what starts the microcontroller ---------
 *
 * PMD_X4_CTL, 0xc010 in the PCS space (devad 0), per lane
 * (bcmi_tsce_xgxs_defs.h:12460):
 *
 *   bit 0  LN_DP_H_RSTB     lane datapath hard reset
 *   bit 1  LN_H_RSTB        lane hard reset
 *   bit 2  LN_TX_H_PWRDN
 *   bit 3  LN_RX_H_PWRDN
 *
 * This is the step that was missing. With the firmware verified in the program
 * RAM, all three micro resets released and UC_ACTIVE set, the core still did
 * nothing and uc_dsc_ready_for_cmd stayed 0 on every lane. Releasing these
 * takes it to 1 on all 96 lanes immediately.
 *
 * xlm_tsc_doc.h:346 says exactly why: LN_H_RSTB "will deassert PMD pin
 * pmd_ln_h_rstb[i] and enable register access to lane associated registers".
 * uc_dsc_ready_for_cmd is a lane register, so until this is released the
 * microcontroller cannot write the bit that says it is alive -- and, being
 * held in lane reset, has nothing to run against either.
 *
 * temod_pmd_x4_reset (temod_cfg_seq.c:3796) toggles both resets low then high,
 * which is what this reproduces; clearing bits 2-3 in the same write powers
 * the lane up, per xlm_tsc_doc.h:323.
 */
#define PMD_X4_CTL      0xc010
#define PMD_X4_RST_MASK 0x000f
#define PMD_X4_RSTB     0x0003          /* LN_H_RSTB | LN_DP_H_RSTB, pwrdn 0 */

static int do_lanerst(int cmc, int only)
{
    int i, lane, ok = 0, bad = 0;

    printf("lanerst: PMD_X4_CTL 0xc010 -- release LN_H_RSTB/LN_DP_H_RSTB, "
           "clear TX/RX power-down\n");
    for (i = 0; i < XLPORT_MAP_N; i++) {
        const struct xlport_tsc *e = &xlport_tsc_map[i];

        if (only >= 0 && e->blk != only) continue;
        for (lane = 0; lane < 4; lane++) {
            uint32_t addr = sbus_reg_addr(e, lane, PMD_X4_CTL);
            int rv = sbus_reg_write(cmc, e->blk, addr,
                                    (PMD_X4_RST_MASK << 16) | 0x0000);
            if (!rv)
                rv = sbus_reg_write(cmc, e->blk, addr,
                                    (PMD_X4_RST_MASK << 16) | PMD_X4_RSTB);
            if (rv) {
                printf("  blk %2d lane %d: FAILED\n", e->blk, lane);
                bad++;
            } else {
                ok++;
            }
        }
        fflush(stdout);
    }
    printf("lanerst: DONE ok %d bad %d\n", ok, bad);
    return bad ? 1 : 0;
}

/* uc_dsc_ready_for_cmd -- d00d bit 7, WRITTEN BY THE MICROCONTROLLER.
 * eagle_tsc_fields.h:667. xlm_tsc_doc.h:335 says to poll it ~10 ms after the
 * 8051 leaves reset. Unlike UC_ACTIVE this is not a bit we can set, so it is
 * the signal that decides whether the core is executing. */
static int do_ucready(int cmc)
{
    int i, lane, ready = 0, total = 0;

    printf("ucready: uc_dsc_ready_for_cmd (0xd00d bit 7), set by the uC\n");
    for (i = 0; i < XLPORT_MAP_N; i++) {
        const struct xlport_tsc *e = &xlport_tsc_map[i];
        for (lane = 0; lane < 4; lane++) {
            uint32_t out[4];
            uint32_t addr = sbus_reg_addr(e, lane, 0xd00d | (1u << 27));

            total++;
            if (sbus_reg_read(cmc, e->blk, addr, out)) {
                printf("  blk %2d lane %d  READ FAILED\n", e->blk, lane);
                continue;
            }
            if (out[0] & 0x0080) ready++;
            if (e->blk == 18 || (out[0] & 0x0080))
                printf("  blk %2d lane %d  0x%08x  ready=%d\n", e->blk, lane,
                       out[0], (out[0] & 0x0080) ? 1 : 0);
        }
        fflush(stdout);
    }
    printf("ucready: %d/%d lanes report uc_dsc_ready_for_cmd\n", ready, total);
    return ready ? 0 : 1;
}

/* Report UC_ACTIVE (d0f4 bit 15) on every macro. */
static int do_ucstat(int cmc)
{
    int i, active = 0;

    printf("ucstat: DIG_TOP_USER_CTL0 (0xd0f4), bit 15 = UC_ACTIVE\n");
    for (i = 0; i < XLPORT_MAP_N; i++) {
        const struct xlport_tsc *e = &xlport_tsc_map[i];
        uint32_t out[4];
        uint32_t addr = sbus_reg_addr(e, 0, 0xd0f4 | (1u << 27));

        if (sbus_reg_read(cmc, e->blk, addr, out)) {
            printf("  blk %2d  READ FAILED\n", e->blk);
            continue;
        }
        if (out[0] & 0x8000) active++;
        printf("  blk %2d  0x%08x  UC_ACTIVE=%d\n", e->blk, out[0],
               (out[0] & 0x8000) ? 1 : 0);
        fflush(stdout);
    }
    printf("ucstat: %d/%d macros report UC_ACTIVE\n", active, XLPORT_MAP_N);
    return active ? 0 : 1;
}

/* ---- UCMEM parallel-bus access mode ----------------------------------------
 *
 * portmod_firmware_set (portmod_common.c:571-611) brackets the microcode load:
 *
 *   XLPORT_WC_UCMEM_CTRL.ACCESS_MODE <- 1     enable parallel bus access
 *   ... write the ucode into XLPORT_UCMEM_DATA ...
 *   XLPORT_WC_UCMEM_CTRL.ACCESS_MODE <- 0     back to MDIO/mailbox access
 *
 * The 08-13 microcode run never did this. With ACCESS_MODE at 0 the memory is
 * the register mailbox, so 59,520 writes were acknowledged and thrown away and
 * the read-back was of the mailbox. That, not dead storage, is the simplest
 * explanation for "ok 59520 failed 0" followed by all-zero reads.
 */
static int ucmem_access(int cmc, int on)
{
    uint32_t v = on ? 1 : 0;
    int i, ok = 0, bad = 0;

    for (i = 0; i < XLPORT_MAP_N; i++) {
        if (reg_put(cmc, xlport_tsc_map[i].blk, 0, XLPORT_WC_UCMEM_CTRL,
                    &v, 1, 4)) bad++;
        else ok++;
    }
    printf("ucmemacc: ACCESS_MODE=%d on %d blocks (ok %d bad %d)\n",
           on, XLPORT_MAP_N, ok, bad);
    return bad ? 1 : 0;
}

/* ---- CMIC MIIM / MDIO ------------------------------------------------------
 *
 * The access path to the SerDes. SerDes (TSC on Trident2+, WarpCore on
 * Trident+) registers are NOT reachable over S-Channel -- they sit behind MDIO,
 * driven by the CMIC's MIIM block. Without this we have no way to talk to the
 * SerDes at all, which is why XLPORT_TSC_PLL_LOCK_STATUS stays 0.
 *
 * Registers (artifacts/cmicm-register-map.json, CMC0 set):
 *   0x031080 PARAM      0x031084 READ_DATA   0x031088 ADDRESS
 *   0x03108c CTRL       0x031090 STAT
 *
 * PARAM fields (fields_c.i, soc_CMIC_CMC0_MIIM_PARAMr_fields):
 *   PHY_DATA [15:0]  PHY_ID [20:16]  C45_SEL [21]
 *   BUS_ID [24:22]   INTERNAL_SEL [25]  MIIM_CYCLE [31:29]
 *
 * Sequence (soc_dcmn_cmicm_miim_operation, miim.c:180-305):
 *   1. write ADDRESS = reg & 0x1f            (clause 22)
 *   2. write PARAM   = built value
 *   3. write CTRL    = 2 to read, 1 to write
 *   4. poll STAT until bit 0 (CMIC_MIIM_OPN_DONE, cmicm.h:147)
 *   5. read READ_DATA
 *
 * The AS5610 (BCM56846) project captured the same interface live at
 * BAR0+0x158 / BAR0+0x4a0 with an identical PARAM layout -- different CMIC
 * generation, same field positions, which is a useful cross-check.
 */
#define MIIM_PARAM(c)  (0x031080 + (c) * 0x1000)
#define MIIM_RDATA(c)  (0x031084 + (c) * 0x1000)
#define MIIM_ADDR(c)   (0x031088 + (c) * 0x1000)
#define MIIM_CTRL(c)   (0x03108c + (c) * 0x1000)
#define MIIM_STAT(c)   (0x031090 + (c) * 0x1000)
#define MIIM_OPN_DONE  0x1

static int miim_op(int cmc, int internal, int c45, int bus, int phy,
                   int reg, int is_write, unsigned data, uint32_t *out)
{
    uint32_t param, st = 0;
    int i;

    wr(asic, MIIM_ADDR(cmc), reg & 0x1f);

    param = (data & 0xffff)
          | ((phy & 0x1f) << 16)
          | ((c45 & 1) << 21)
          | ((bus & 0x7) << 22)
          | ((internal & 1) << 25);
    wr(asic, MIIM_PARAM(cmc), param);
    wr(asic, MIIM_CTRL(cmc), is_write ? 1 : 2);

    for (i = 0; i < 20000; i++) {
        st = rd(asic, MIIM_STAT(cmc));
        if (st & MIIM_OPN_DONE) break;
        usleep(50);
    }
    if (!(st & MIIM_OPN_DONE)) return -1;
    if (out) *out = rd(asic, MIIM_RDATA(cmc)) & 0xffff;
    return 0;
}

/* Read one MDIO register and print it. */
static int do_miimread(int cmc, int internal, int bus, int phy, int reg)
{
    uint32_t v = 0;
    int rv;

    printf("miim: cmc%d internal=%d bus=%d phy=0x%02x reg=0x%02x ... ",
           cmc, internal, bus, phy, reg);
    fflush(stdout);
    msleep(200);
    rv = miim_op(cmc, internal, 0, bus, phy, reg, 0, 0, &v);
    if (rv) { printf("TIMEOUT (no OPN_DONE)\n"); return 1; }
    printf("0x%04x\n", v);
    return 0;
}

/* Sweep PHY addresses on a bus, reading clause-22 register 0 (control) and 2
 * (PHY ID high). A live SerDes answers with something other than 0xffff. */
static int do_miimscan(int cmc, int internal, int bus)
{
    int phy, found = 0;

    printf("miimscan: cmc%d internal=%d bus=%d, phy 0x00..0x1f, reg 2 (PHY ID hi)\n",
           cmc, internal, bus);
    fflush(stdout);
    msleep(200);
    for (phy = 0; phy < 32; phy++) {
        uint32_t v = 0;
        if (miim_op(cmc, internal, 0, bus, phy, 2, 0, 0, &v)) {
            printf("miimscan:   phy 0x%02x  TIMEOUT\n", phy);
            fflush(stdout);
            continue;
        }
        /* Print every value. Filtering 0x0000 as "no response" was a mistake:
         * an absent MDIO PHY normally floats to 0xffff, so 0x0000 is a distinct
         * result worth seeing, not noise to hide. */
        printf("miimscan:   phy 0x%02x  reg2 = 0x%04x%s\n", phy, v,
               (v != 0xffff && v != 0x0000) ? "  *** responds ***" : "");
        fflush(stdout);
        if (v != 0xffff && v != 0x0000) found++;
    }
    printf("miimscan: %d responder(s) on bus %d\n", found, bus);
    return 0;
}

/* Every TOP-block register for BCM56860, from the SDK's allregs_*.i. Used to
 * diff warm (EOS, ports up) against cold and find what we are not setting --
 * specifically the TSC core clock enable. Read-only. */
static const struct { const char *name; uint32_t addr; } TOPREGS[] = {
    { "TOP_SOFT_RESET_REG", 0x02030100 },
    { "TOP_SOFT_RESET_REG_2", 0x02030200 },
    { "TOP_SWITCH_FEATURE_ENABLE", 0x02030300 },
    { "TOP_TAP_CONTROL", 0x02030400 },
    { "TOP_MISC_CONTROL", 0x02030500 },
    { "TOP_MISC_STATUS", 0x02030700 },
    { "TOP_RING_OSC_CTRL", 0x02030800 },
    { "TOP_OSC_COUNT_STAT", 0x02030a00 },
    { "TOP_CORE_PLL_CTRL_0", 0x02030b00 },
    { "TOP_CORE_PLL_CTRL_1", 0x02030c00 },
    { "TOP_CORE_PLL_CTRL_2", 0x02030d00 },
    { "TOP_CORE_PLL_CTRL_3", 0x02030e00 },
    { "TOP_CORE_PLL_CTRL_4", 0x02030f00 },
    { "TOP_CORE_PLL_STATUS", 0x02031000 },
    { "TOP_XG_PLL0_CTRL_0", 0x02031100 },
    { "TOP_XG_PLL0_CTRL_1", 0x02031200 },
    { "TOP_XG_PLL0_CTRL_2", 0x02031300 },
    { "TOP_XG_PLL0_CTRL_3", 0x02031400 },
    { "TOP_XG_PLL0_CTRL_4", 0x02031500 },
    { "TOP_XG_PLL0_STATUS", 0x02031600 },
    { "TOP_XG_PLL1_CTRL_0", 0x02031700 },
    { "TOP_XG_PLL1_CTRL_1", 0x02031800 },
    { "TOP_XG_PLL1_CTRL_2", 0x02031900 },
    { "TOP_XG_PLL1_CTRL_3", 0x02031a00 },
    { "TOP_XG_PLL1_CTRL_4", 0x02031b00 },
    { "TOP_XG_PLL1_STATUS", 0x02031c00 },
    { "TOP_XG_PLL2_CTRL_0", 0x02031d00 },
    { "TOP_XG_PLL2_CTRL_1", 0x02031e00 },
    { "TOP_XG_PLL2_CTRL_2", 0x02031f00 },
    { "TOP_XG_PLL2_CTRL_3", 0x02032000 },
    { "TOP_XG_PLL2_CTRL_4", 0x02032100 },
    { "TOP_XG_PLL2_STATUS", 0x02032200 },
    { "TOP_XG_PLL3_CTRL_0", 0x02032300 },
    { "TOP_XG_PLL3_CTRL_1", 0x02032400 },
    { "TOP_XG_PLL3_CTRL_2", 0x02032500 },
    { "TOP_XG_PLL3_CTRL_3", 0x02032600 },
    { "TOP_XG_PLL3_CTRL_4", 0x02032700 },
    { "TOP_XG_PLL3_STATUS", 0x02032800 },
    { "TOP_MCS_PLL_CTRL_0", 0x02032900 },
    { "TOP_MCS_PLL_CTRL_1", 0x02032a00 },
    { "TOP_MCS_PLL_CTRL_2", 0x02032b00 },
    { "TOP_MCS_PLL_CTRL_3", 0x02032c00 },
    { "TOP_MCS_PLL_CTRL_4", 0x02032d00 },
    { "TOP_MCS_PLL_STATUS", 0x02033000 },
    { "TOP_TS_PLL_CTRL_0", 0x02033100 },
    { "TOP_TS_PLL_CTRL_1", 0x02033200 },
    { "TOP_TS_PLL_CTRL_2", 0x02033300 },
    { "TOP_TS_PLL_CTRL_3", 0x02033400 },
    { "TOP_TS_PLL_CTRL_4", 0x02033500 },
    { "TOP_TS_PLL_STATUS", 0x02033700 },
    { "TOP_BS_PLL_CTRL_0", 0x02033800 },
    { "TOP_BS_PLL_CTRL_1", 0x02033900 },
    { "TOP_BS_PLL_CTRL_2", 0x02033a00 },
    { "TOP_BS_PLL_CTRL_3", 0x02033b00 },
    { "TOP_BS_PLL_CTRL_4", 0x02033c00 },
    { "TOP_BS_PLL_STATUS", 0x02033e00 },
    { "TOP_L1_RCVD_CLK_VALID_STATUS_0", 0x02033f00 },
    { "TOP_L1_RCVD_CLK_VALID_STATUS_1", 0x02034000 },
    { "TOP_L1_RCVD_CLK_VALID_STATUS_2", 0x02034100 },
    { "TOP_PVTMON_CTRL_0", 0x02034200 },
    { "TOP_PVTMON_CTRL_1", 0x02034300 },
    { "TOP_PVTMON_RESULT_0", 0x02034400 },
    { "TOP_PVTMON_RESULT_1", 0x02034500 },
    { "TOP_PVTMON_RESULT_2", 0x02034600 },
    { "TOP_PVTMON_RESULT_3", 0x02034700 },
    { "TOP_PVTMON_RESULT_4", 0x02034800 },
    { "TOP_PVTMON_RESULT_5", 0x02034900 },
    { "TOP_PVTMON_RESULT_6", 0x02034a00 },
    { "TOP_PVTMON_RESULT_7", 0x02034b00 },
    { "TOP_PVTMON_RESULT_8", 0x02034c00 },
    { "TOP_PVTMON_0_INTR_THRESHOLD", 0x02034e00 },
    { "TOP_PVTMON_1_INTR_THRESHOLD", 0x02034f00 },
    { "TOP_PVTMON_2_INTR_THRESHOLD", 0x02035000 },
    { "TOP_PVTMON_3_INTR_THRESHOLD", 0x02035100 },
    { "TOP_PVTMON_4_INTR_THRESHOLD", 0x02035200 },
    { "TOP_PVTMON_5_INTR_THRESHOLD", 0x02035300 },
    { "TOP_PVTMON_6_INTR_THRESHOLD", 0x02035400 },
    { "TOP_PVTMON_7_INTR_THRESHOLD", 0x02035500 },
    { "TOP_PVTMON_8_INTR_THRESHOLD", 0x02035600 },
    { "TOP_TSC_DISABLE", 0x02035800 },
    { "TOP_TSC_AFE_PLL_STATUS", 0x02035900 },
    { "TOP_INT_REV_ID_REG", 0x02037e00 },
    { "TOP_UC_TAP_CONTROL", 0x02037f00 },
    { "TOP_UC_TAP_WRITE_DATA", 0x02038000 },
    { "TOP_UC_TAP_READ_DATA", 0x02038100 },
    { "TOP_CORE_CLK_FREQ_SEL", 0x02038200 },
    { "TOP_CLOCKING_ENFORCE_PSG", 0x02038300 },
    { "TOP_CLOCKING_ENFORCE_PCG", 0x02038400 },
    { "TOP_HW_TAP_CONTROL", 0x02038500 },
    { "TOP_HW_TAP_MEM_DEBUG", 0x02038600 },
    { "TOP_HW_TAP_MEM_ECC_STATUS", 0x02038900 },
    { "TOP_HW_TAP_READ_VAILD_DEBUG_STATUS", 0x02038a00 },
    { "TOP_L1_RCVD_CLK_VALID_STATUS_3", 0x02038b00 },
    { "TOP_MISC_CONTROL_2", 0x02038c00 },
    { "TOP_UPI_CTRL_0", 0x02038f00 },
    { "TOP_UPI_CTRL_1", 0x02039000 },
    { "TOP_UPI_STATUS_0", 0x02039100 },
    { "TOP_UPI_STATUS_1", 0x02039200 },
    { "TOP_UPI_STATUS_2", 0x02039300 },
    { "TOP_UPI_STATUS_3", 0x02039400 },
    { "TOP_UPI_STATUS_4", 0x02039500 },
    { "TOP_UPI_STATUS_5", 0x02039600 },
    { "TOP_UPI_STATUS_6", 0x02039700 },
    { "TOP_UPI_STATUS_7", 0x02039800 },
    { "TOP_UPI_STATUS_8", 0x02039900 },
    { "TOP_UPI_STATUS_9", 0x02039a00 },
    { "TOP_UPI_STATUS_10", 0x02039b00 },
    { "TOP_UPI_STATUS_11", 0x02039c00 },
    { "TOP_UPI_STATUS_12", 0x02039d00 },
    { "TOP_UPI_STATUS_13", 0x02039e00 },
    { "TOP_UPI_STATUS_14", 0x02039f00 },
    { "TOP_UPI_STATUS_15", 0x0203a000 },
    { "TOP_UPI_STATUS_16", 0x0203a100 },
    { "TOP_CPU2TAP_MEM_TM", 0x0203a200 }
};

static int do_topdump(int cmc)
{
    unsigned i;
    uint32_t v;

    printf("topdump: %u TOP registers, block %d\n",
           (unsigned)(sizeof(TOPREGS)/sizeof(TOPREGS[0])), TOP_BLK);
    fflush(stdout);
    for (i = 0; i < sizeof(TOPREGS)/sizeof(TOPREGS[0]); i++) {
        if (schan_get(cmc, TOP_BLK, TOPREGS[i].addr, &v))
            printf("  %-34s TIMEOUT\n", TOPREGS[i].name);
        else if (v)
            printf("  %-34s 0x%08x\n", TOPREGS[i].name, v);
        fflush(stdout);
    }
    printf("topdump: done\n");
    return 0;
}

/* ---- the TD2+ SerDes reset: PGW_TSCn_CTRL_REG ---------------------------
 *
 * This is the "Get Serdes OOR" step of _pm4x10_pm_xlport_init that the project
 * stalled on.  On BCM56860, XLPORT_XGXS0_CTRL_REG does not exist
 * (bcm56860_a0.c:99424 is NULL), so PortMod calls a platform callback instead
 * -- which is why every XLPORT-focused search came up empty:
 *
 *   src/soc/esw/trident2p/portctrl.c:410
 *       pm4x10.portmod_phy_external_reset = soc_esw_portctrl_reset_tsc3_cb
 *   src/soc/esw/portctrl.c:941
 *       "Reset TSC by setting TSC control register ... which resides in PGW
 *        block.  For TD2+ case, the register is PGW_TSC3_CTRL_REG"
 *       -> soc_tsc_xgxs_reset(unit, port, 3)
 *   src/soc/common/drv.c:2938, the SOC_IS_TRIDENT2X branch
 *       -> _soc_xgxs_reset_single_tsc(unit, port, PGW_TSC3_CTRL_REGr)
 *
 * The register is in SOC_BLK_PGW_CL -- S-Channel blocks 6..13, 8 instances
 * (bcm56860_a0.c:132420).  Each PGW_CL covers 16 lanes = 4 TSCs:
 *
 *   TSC0,1,2   the pm12x10 group (portctrl.c:805-809; the core order is
 *              reversed on odd pmid, which does not matter here)
 *   TSC3       the standalone pm4x10
 *
 *   PGW_TSC0_CTRL_REG 0x2003200   PGW_TSC2_CTRL_REG 0x2003400
 *   PGW_TSC1_CTRL_REG 0x2003300   PGW_TSC3_CTRL_REG 0x2003500
 *
 * Fields, all 1 bit (fields_p.i, soc_PGW_TSC0_CTRL_REGr_fields):
 *   RSTB_DVT 0, DVT_EN 1, RSTB_HW 2, REFOUT_EN 3, REFIN_EN 4, PWRDWN 5, IDDQ 6
 *
 * Ground truth read off this board warm, EOS running, all 8 blocks x 4 regs:
 *   0x14 = REFIN_EN | RSTB_HW    27 of 32   brought up
 *   0x30 = REFIN_EN | PWRDWN      5 of 32   still parked
 * so 0x14 is the target, and the sequence below lands exactly on it.
 *
 * Sequence from _soc_xgxs_reset_single_tsc.  Every conditional in that function
 * is for TH3/GH2/iddq_new_default and does not apply to TD2+; RSTB_REFCLK and
 * RSTB_PLL are not fields of this register, so those two writes drop out too.
 */
#define PGW_TSC_CTRL(i)  (0x2003200u + ((uint32_t)(i) << 8))
#define TSC_RSTB_HW      (1u << 2)
#define TSC_REFIN_EN     (1u << 4)
#define TSC_PWRDWN       (1u << 5)
#define TSC_TARGET       0x14u

static void tsc_decode(uint32_t v)
{
    printf("[");
    if (v & (1u << 0)) printf(" RSTB_DVT");
    if (v & (1u << 1)) printf(" DVT_EN");
    if (v & TSC_RSTB_HW) printf(" RSTB_HW");
    if (v & (1u << 3)) printf(" REFOUT_EN");
    if (v & TSC_REFIN_EN) printf(" REFIN_EN");
    if (v & TSC_PWRDWN) printf(" PWRDWN");
    if (v & (1u << 6)) printf(" IDDQ");
    printf(" ]");
}

static int tsc_step(int cmc, int blk, uint32_t addr, uint32_t v,
                    const char *what, int usec)
{
    printf("  %-12s <- 0x%08x ", what, v);
    fflush(stdout);
    if (schan_put(cmc, blk, addr, v) < 0) { printf("WRITE FAILED\n"); return -1; }
    printf("ok\n");
    fflush(stdout);
    if (usec) usleep(usec);
    return 0;
}

static int tscreset(int cmc, int blk, int idx)
{
    uint32_t addr = PGW_TSC_CTRL(idx), v = 0, got = 0;

    if (blk < 6 || blk > 13) { printf("tscreset: PGW_CL blocks are 6..13\n"); return 2; }
    if (idx < 0 || idx > 3)  { printf("tscreset: TSC index is 0..3\n"); return 2; }

    printf("tscreset: PGW_CL%d (blk %d) TSC%d @ 0x%08x, cmc %d\n",
           blk - 6, blk, idx, addr, cmc);

    if (schan_get(cmc, blk, addr, &v) < 0) {
        printf("  read FAILED -- is the PGW block out of reset? (run phaseb)\n");
        return 3;
    }
    printf("  before  0x%08x ", v); tsc_decode(v); printf("\n");

    /* reference clock in: spn_XGXS_LCPLL defaults to 1 on real silicon */
    v |= TSC_REFIN_EN;
    if (tsc_step(cmc, blk, addr, v, "REFIN_EN=1", 0)) return 3;

    v &= ~TSC_PWRDWN;                       /* deassert power down */
    if (tsc_step(cmc, blk, addr, v, "PWRDWN=0", 1100)) return 3;

    v &= ~TSC_RSTB_HW;                      /* hold in reset ... */
    if (tsc_step(cmc, blk, addr, v, "RSTB_HW=0", 1100 + 10000)) return 3;

    v |= TSC_RSTB_HW;                       /* ... then release */
    if (tsc_step(cmc, blk, addr, v, "RSTB_HW=1", 1100)) return 3;

    if (schan_get(cmc, blk, addr, &got) < 0) { printf("  read-back FAILED\n"); return 3; }
    printf("  after   0x%08x ", got); tsc_decode(got); printf("\n");
    printf("  %s\n", got == TSC_TARGET
           ? "MATCHES the warm chip (0x14)"
           : "does NOT match the warm chip's 0x14");
    return got == TSC_TARGET ? 0 : 1;
}

/* Read-only: all 32 TSC control registers. Safe warm on cmc 1. */
static int tscdump(int cmc)
{
    int blk, idx;
    uint32_t v;

    printf("tscdump: PGW_CL TSC control registers (cmc %d)\n", cmc);
    printf("              TSC0       TSC1       TSC2       TSC3\n");
    for (blk = 6; blk <= 13; blk++) {
        printf("PGW_CL%d blk%-2d", blk - 6, blk);
        for (idx = 0; idx < 4; idx++) {
            if (schan_get(cmc, blk, PGW_TSC_CTRL(idx), &v) < 0) printf("  ----------");
            else printf("  0x%08x", v);
            fflush(stdout);
        }
        printf("\n");
    }
    printf("  0x14 = REFIN_EN|RSTB_HW (up)   0x30 = REFIN_EN|PWRDWN (parked)\n");
    return 0;
}

/* ---- CMICm packet DMA ---------------------------------------------------
 *
 * Packet I/O is the other half of "make this switch forward" and it is
 * completely independent of the SerDes: a CPU-injected frame goes host memory
 * -> CMIC DMA -> pipeline, and never touches a port macro. So this can proceed
 * while the SerDes work is blocked.
 *
 * Register offsets are absolute in ASIC BAR0 (include/soc/mcm/cmicm.h), and
 * every one is inside the 0x40000 window we already map. x = CMC (0..2),
 * y = channel (0..3):
 *
 *   CH{y}_DMA_CTRL            0x31140 + y*4      + 0x1000*x
 *   DMA_CH{y}_DESC_HALT_ADDR  0x31120 + y*4
 *   CH{y}_DMA_CURR_DESC       0x311a8 + y*4
 *   DMA_DESC{y}               0x31158 + y*4      first descriptor address
 *   CH{y}_COS_CTRL_RX_0/_1    0x31168 + y*8 / 0x3116c + y*8
 *   PKT_COUNT_CH{y}_RXPKT     0x31480 + y*8
 *   PKT_COUNT_CH{y}_TXPKT     0x31484 + y*8
 *   DMA_STAT                  0x31150            (per CMC)
 *   DMA_STAT_HI               0x31130            (per CMC)
 *
 * CH_DMA_CTRL bits (include/soc/cmicm.h:82-93).
 *
 * The descriptor format is DCB **type 33**, which
 * include/soc/shared/dcbformats/type33.h documents as "used by the 56860
 * (TD2+)" -- 16 words, with the EP_TO_CPU header overlaid on words 2..15 for
 * RX. soc_feature_dcb_type33 is flagged for 56860 (feature.h:59).
 *
 * Reference implementation to follow when driving this: src/soc/common/cmicd_dma.c.
 *
 * This command is READ-ONLY. Its purpose is ground truth: EOS is doing CPU
 * packet DMA continuously on this board (Et1 holds an OSPF adjacency), so a
 * warm dump shows a working channel set up by a working driver -- including the
 * live descriptor physical addresses. Same method that cracked the PGW TSC
 * register.
 */
#define PKTDMA_CTRL(x, y)       (0x31140u + (y) * 4u + (x) * 0x1000u)
#define PKTDMA_HALT_ADDR(x, y)  (0x31120u + (y) * 4u + (x) * 0x1000u)
#define PKTDMA_CURR_DESC(x, y)  (0x311a8u + (y) * 4u + (x) * 0x1000u)
#define PKTDMA_DESC(x, y)       (0x31158u + (y) * 4u + (x) * 0x1000u)
#define PKTDMA_COS_RX0(x, y)    (0x31168u + (y) * 8u + (x) * 0x1000u)
#define PKTDMA_COS_RX1(x, y)    (0x3116cu + (y) * 8u + (x) * 0x1000u)
#define PKTDMA_RXPKT(x, y)      (0x31480u + (y) * 8u + (x) * 0x1000u)
#define PKTDMA_TXPKT(x, y)      (0x31484u + (y) * 8u + (x) * 0x1000u)
#define PKTDMA_STAT(x)          (0x31150u + (x) * 0x1000u)
#define PKTDMA_STAT_HI(x)       (0x31130u + (x) * 0x1000u)
#define PKTDMA_STAT_CLR(x)      (0x311a4u + (x) * 0x1000u)

/* CMIC_CMCx_CHy_DMA_CTRL, include/soc/cmicm.h:83-92. The first version of
 * pkttx/pktrx set DIRECTION|ENABLE only, which is not what the SDK does:
 * cmicd_dma_chan_config adds CNTLD_DESC_INTR_MODE and cmicd_dma_chan_start
 * raises ENABLE|CONTINUOUS_ENABLE together. */
#define PD_DIRECTION            0x00000001u
#define PD_ENABLE               0x00000002u
#define PD_ABORT                0x00000004u
#define PD_CNTLD_DESC_INTR_MODE 0x00000100u
#define PD_CONTINUOUS_ENABLE    0x00000200u
/* CMIC_CMCx_DMA_STAT bits, per channel y */
#define PD_CHAIN_DONE(y)        (0x00000001u << (y))
#define PD_DESC_DONE(y)         (0x00000010u << (y))
#define PD_DMA_ACTIVE(y)        (0x00000100u << (y))
/* CMIC_CMCx_DMA_STAT_CLR bits */
#define PD_DESCRD_CMPLT_CLR(y)  (0x00000001u << (y))
#define PD_CNTLD_DESC_CLR(y)    (0x00000100u << (y))

/* Decode DMA_STAT for one channel -- the bits that say whether the engine
 * touched our descriptor at all. Reading 0x880 as "nothing happened" was wrong:
 * that is DESC_DONE(3) | DMA_ACTIVE(3). */
static void pktdma_stat_show(const char *tag, int cmc, int chan)
{
    uint32_t s = rd(asic, PKTDMA_STAT(cmc));
    uint32_t h = rd(asic, PKTDMA_STAT_HI(cmc));

    printf("  %s DMA_STAT 0x%08x [%s%s%s] STAT_HI 0x%08x%s\n", tag, s,
           (s & PD_CHAIN_DONE(chan)) ? " CHAIN_DONE" : "",
           (s & PD_DESC_DONE(chan))  ? " DESC_DONE"  : "",
           (s & PD_DMA_ACTIVE(chan)) ? " ACTIVE"     : "",
           h, (h & (0x08000000u << chan)) ? " IN_HALT" : "");
}

static void pktdma_decode_ctrl(uint32_t v)
{
    printf("[");
    printf(v & 0x001 ? " TX" : " RX");           /* DIRECTION: 1 = TX */
    if (v & 0x002) printf(" ENABLE");
    if (v & 0x004) printf(" ABORT");
    if (v & 0x008) printf(" INTR_ON_DESC_OR_PKT");
    if (v & 0x010) printf(" BIG_ENDIAN");
    if (v & 0x020) printf(" DESC_BIG_ENDIAN");
    if (v & 0x040) printf(" DROP_RX_ON_CHAIN_END");
    if (v & 0x080) printf(" RLD_STATUS_UPD_DIS");
    if (v & 0x100) printf(" CNTLD_DESC_INTR");
    if (v & 0x200) printf(" CONTINUOUS");
    printf(" ]");
}

static int dmaregs(int cmc)
{
    int y;

    printf("dmaregs: CMICm packet DMA, CMC %d (read-only)\n", cmc);
    printf("  DMA_STAT      0x%08x\n", rd(asic, PKTDMA_STAT(cmc)));
    printf("  DMA_STAT_HI   0x%08x\n", rd(asic, PKTDMA_STAT_HI(cmc)));
    for (y = 0; y < 4; y++) {
        uint32_t ctrl = rd(asic, PKTDMA_CTRL(cmc, y));
        printf("  --- channel %d ---\n", y);
        printf("    DMA_CTRL      0x%08x ", ctrl); pktdma_decode_ctrl(ctrl); printf("\n");
        printf("    DESC          0x%08x   (first descriptor, physical)\n",
               rd(asic, PKTDMA_DESC(cmc, y)));
        printf("    CURR_DESC     0x%08x\n", rd(asic, PKTDMA_CURR_DESC(cmc, y)));
        printf("    DESC_HALT     0x%08x\n", rd(asic, PKTDMA_HALT_ADDR(cmc, y)));
        printf("    COS_CTRL_RX   0x%08x 0x%08x\n",
               rd(asic, PKTDMA_COS_RX0(cmc, y)), rd(asic, PKTDMA_COS_RX1(cmc, y)));
        printf("    PKT_COUNT     rx %u  tx %u\n",
               rd(asic, PKTDMA_RXPKT(cmc, y)), rd(asic, PKTDMA_TXPKT(cmc, y)));
    }
    return 0;
}

/* Read a DCB (type 33, 16 words) out of host physical memory via /dev/mem.
 * The descriptor addresses come from dmaregs; this is how we read the ring a
 * working driver built. Read-only. */
static int dcbdump(unsigned long phys, int n)
{
    int fd, i, w;
    unsigned long pagebase, off;
    size_t len;
    volatile uint32_t *p;
    void *m;

    if ((fd = open("/dev/mem", O_RDONLY | O_SYNC)) < 0) {
        perror("open /dev/mem");
        return 1;
    }
    pagebase = phys & ~0xfffUL;
    off = phys - pagebase;
    len = off + (size_t)n * 64 + 0x1000;
    m = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, (off_t)pagebase);
    if (m == MAP_FAILED) { perror("mmap /dev/mem"); close(fd); return 1; }
    p = (volatile uint32_t *)((char *)m + off);

    printf("dcbdump: %d DCB(s) of type 33 (16 words) at phys 0x%lx\n", n, phys);
    for (i = 0; i < n; i++) {
        const volatile uint32_t *d = p + i * 16;
        uint32_t c = d[1];
        printf("  [%d] @0x%lx\n", i, phys + (unsigned long)i * 64);
        uint32_t st = d[15];
        printf("      addr  0x%08x   ctrl 0x%08x  count %u%s%s%s%s\n",
               d[0], c, c & 0xffff,
               (c & (1u << 16)) ? " CHAIN"  : "",
               (c & (1u << 17)) ? " SG"     : "",
               (c & (1u << 18)) ? " RELOAD" : "",
               (c & (1u << 19)) ? " HIGIG"  : "");
        /* T33.15 is "DMA Status 0": low 16 bits are the transferred byte
         * count, so a filled RX descriptor shows the real frame length. */
        printf("      stat  0x%08x  xferred %u bytes%s\n",
               st, st & 0xffff, (st & 0x80000000u) ? " DONE" : "");
        /* words 2..14 are the EP_TO_CPU header on RX */
        for (w = 2; w < 15; w += 4) {
            int k, n4 = (15 - w) < 4 ? (15 - w) : 4;
            printf("      w%-2d  ", w);
            for (k = 0; k < n4; k++) printf(" 0x%08x", d[w + k]);
            printf("\n");
        }
    }
    munmap(m, len);
    close(fd);
    return 0;
}

/* ---- DMA buffers -------------------------------------------------------
 *
 * The gate on packet I/O is a buffer whose PHYSICAL address we know: the DCB's
 * word 0 is a physical address, and the chip DMAs to it directly.
 *
 * The obvious route -- reserve memory with `mem=` on the kernel cmdline and
 * mmap /dev/mem -- needs a reboot and a custom cmdline. This does not: allocate
 * a locked anonymous page and resolve its physical address through
 * /proc/self/pagemap. A single 4 KB page is contiguous by definition, and a DCB
 * (64 B) plus a small frame both fit inside one. Works identically under EOS
 * (kernel 3.4) and EdgeNOS (6.12), and needs no reboot.
 *
 * pagemap gives one 64-bit entry per virtual page:
 *   bit 63      page present
 *   bit 62      swapped
 *   bits 0..54  PFN
 * PFN * page_size + (vaddr & (page_size-1)) is the physical address. Reading
 * the PFN needs CAP_SYS_ADMIN on modern kernels; we run as root.
 *
 * MAP_LOCKED plus an explicit mlock() keep the page resident so the PFN cannot
 * change under us. The page is also touched before the lookup -- pagemap
 * reports "not present" for a page that has never been faulted in, which reads
 * as a bogus physical address of 0.
 *
 * The DCB address field is 32 bits, so the buffer must live below 4 GB. This
 * board has RAM at 0x01000000-0xdffcffff and 0x100000000-0x11effffff, so a page
 * CAN land too high; that is checked rather than assumed.
 */
struct dmabuf {
    void          *va;
    unsigned long  pa;
    size_t         len;
};

static int dmabuf_alloc(struct dmabuf *b, size_t len);
static void dmabuf_free(struct dmabuf *b);

/* Allocate a buffer that is actually reachable by a 32-bit DCB address field.
 *
 * This board has RAM at 0x100000000-0x11effffff, and the allocator hands out
 * pages from up there often enough to matter: a 50-frame test silently degraded
 * to almost no frames sent because every allocation aborted, and the result
 * looked exactly like "the packet was dropped in the pipeline". Retry, holding
 * on to the too-high pages so the allocator cannot hand back the same one, then
 * release them.
 */
static int dmabuf_alloc_low_n(struct dmabuf *out, int want);

static int dmabuf_alloc_low(struct dmabuf *b, size_t len)
{
    (void)len;
    return dmabuf_alloc_low_n(b, 1);
}

static int dmabuf_alloc_low_n(struct dmabuf *out, int want)
{
    long pgsz = sysconf(_SC_PAGESIZE);
    const char *e = getenv("DMABUF_SCAN_MB");
    size_t scan = (size_t)(e ? atoi(e) : 256) * 1024 * 1024;
    unsigned long long ent;
    size_t npages, i;
    size_t keep[64];
    unsigned long keeppa[64];
    int got = 0, k;
    unsigned long pa = 0;
    char *region;
    int fd;

    if (want > 64) want = 64;
    /* Page-at-a-time retry does not work here. This kernel serves userspace
     * top-down and the whole 0x100000000-0x11effffff region is free, so 4096
     * consecutive single-page allocations all landed above 4 GB -- we would
     * have to exhaust ~500 MB one page at a time to reach low memory.
     *
     * Instead take one large mapping, find a page inside it that is below
     * 4 GB, and give back everything else. MAP_POPULATE faults it all in so
     * pagemap has real PFNs to report. */
    region = mmap(NULL, scan, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_POPULATE, -1, 0);
    if (region == MAP_FAILED) { perror("mmap scan region"); return -1; }

    if ((fd = open("/proc/self/pagemap", O_RDONLY)) < 0) {
        perror("open /proc/self/pagemap");
        munmap(region, scan);
        return -1;
    }
    npages = scan / (size_t)pgsz;
    for (i = 0; i < npages; i++) {
        unsigned long vpn = ((unsigned long)region + i * (unsigned long)pgsz)
                            / (unsigned long)pgsz;
        if (pread(fd, &ent, sizeof(ent), (off_t)vpn * 8) != (ssize_t)sizeof(ent)) continue;
        if (!(ent & (1ULL << 63))) continue;
        pa = (unsigned long)((ent & ((1ULL << 55) - 1)) * (unsigned long long)pgsz);
        if (pa && pa + (unsigned long)pgsz <= 0x100000000UL) {
            keep[got] = i; keeppa[got] = pa;
            if (++got == want) break;
        }
    }
    close(fd);

    if (got < want) {
        printf("dmabuf: only %d of %d pages below 4 GB in %zu MB "
               "(raise DMABUF_SCAN_MB)\n", got, want, scan / (1024 * 1024));
        munmap(region, scan);
        return -1;
    }

    /* Keep the chosen pages, hand the rest back. Unmapping around them one
     * gap at a time keeps this simple; the kept pages stay resident. */
    for (k = 0; k < got; k++) {
        out[k].va  = region + keep[k] * (size_t)pgsz;
        out[k].pa  = keeppa[k];
        out[k].len = (size_t)pgsz;
        if (mlock(out[k].va, out[k].len) < 0) perror("mlock (continuing)");
        memset(out[k].va, 0, out[k].len);
    }
    {
        size_t prev = 0;
        for (k = 0; k < got; k++) {
            if (keep[k] > prev)
                munmap(region + prev * (size_t)pgsz, (keep[k] - prev) * (size_t)pgsz);
            prev = keep[k] + 1;
        }
        if (prev < npages)
            munmap(region + prev * (size_t)pgsz, (npages - prev) * (size_t)pgsz);
    }
    printf("  scanned %zu MB, kept %d low page(s): phys 0x%lx%s\n",
           scan / (1024 * 1024), got, out[0].pa, got > 1 ? " ..." : "");
    return 0;
}

static int dmabuf_alloc(struct dmabuf *b, size_t len)
{
    long pgsz = sysconf(_SC_PAGESIZE);
    unsigned long long ent;
    unsigned long vpn;
    int fd;

    b->va = NULL;
    b->pa = 0;
    b->len = len = (len + (size_t)pgsz - 1) & ~((size_t)pgsz - 1);

    b->va = mmap(NULL, len, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
    if (b->va == MAP_FAILED) { perror("mmap anon"); b->va = NULL; return -1; }
    if (mlock(b->va, len) < 0) perror("mlock (continuing)");
    memset(b->va, 0, len);              /* fault it in before the lookup */

    if ((fd = open("/proc/self/pagemap", O_RDONLY)) < 0) {
        perror("open /proc/self/pagemap");
        return -1;
    }
    vpn = (unsigned long)b->va / (unsigned long)pgsz;
    if (pread(fd, &ent, sizeof(ent), (off_t)vpn * 8) != (ssize_t)sizeof(ent)) {
        perror("pread pagemap");
        close(fd);
        return -1;
    }
    close(fd);

    if (!(ent & (1ULL << 63))) {
        printf("dmabuf: page not present (pagemap 0x%llx) -- cannot resolve PFN\n", ent);
        return -1;
    }
    b->pa = (unsigned long)((ent & ((1ULL << 55) - 1)) * (unsigned long long)pgsz);
    if (!b->pa) {
        printf("dmabuf: PFN reads 0 -- kernel is hiding it (need root/CAP_SYS_ADMIN)\n");
        return -1;
    }
    return 0;
}

static void dmabuf_free(struct dmabuf *b)
{
    if (b->va) { munlock(b->va, b->len); munmap(b->va, b->len); b->va = NULL; }
}

/* Independently confirm a virtual->physical mapping by reading the same bytes
 * back through /dev/mem. If pagemap lied, or the page moved, the pattern will
 * not be there -- and we find out now rather than by DMAing into a random page
 * of somebody else's memory. */
static int dmabuf_verify(struct dmabuf *b)
{
    volatile uint32_t *p = (volatile uint32_t *)b->va;
    unsigned long pagebase = b->pa & ~0xfffUL, off = b->pa & 0xfffUL;
    const uint32_t pat[4] = { 0xa5a5a5a5u, 0x5a5a5a5au, 0xdeadbeefu, 0x12345678u };
    volatile uint32_t *q;
    size_t len = off + 0x1000;
    int fd, i, ok = 1;
    void *m;

    for (i = 0; i < 4; i++) p[i] = pat[i];
    __sync_synchronize();

    if ((fd = open("/dev/mem", O_RDONLY | O_SYNC)) < 0) { perror("open /dev/mem"); return -1; }
    m = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, (off_t)pagebase);
    if (m == MAP_FAILED) { perror("mmap /dev/mem"); close(fd); return -1; }
    q = (volatile uint32_t *)((char *)m + off);

    printf("  verify via /dev/mem @0x%lx:\n", b->pa);
    for (i = 0; i < 4; i++) {
        uint32_t got = q[i];
        printf("    [%d] wrote 0x%08x  read 0x%08x  %s\n",
               i, pat[i], got, got == pat[i] ? "ok" : "MISMATCH");
        if (got != pat[i]) ok = 0;
    }
    munmap(m, len);
    close(fd);
    return ok ? 0 : 1;
}

static int do_dmabuf(size_t len)
{
    struct dmabuf b;
    int rv;

    printf("dmabuf: allocating %zu bytes, locked, resolving physical address\n", len);
    if (dmabuf_alloc(&b, len)) { dmabuf_free(&b); return 1; }
    printf("  virt 0x%lx  ->  phys 0x%lx  (%zu bytes)\n",
           (unsigned long)b.va, b.pa, b.len);
    if (b.pa + b.len > 0x100000000UL) {
        printf("  ** above 4 GB -- a 32-bit DCB address field cannot reach this.\n");
        printf("     Retry; the allocator usually lands in low memory.\n");
        dmabuf_free(&b);
        return 1;
    }
    printf("  below 4 GB, reachable by a 32-bit DCB address field\n");
    rv = dmabuf_verify(&b);
    printf("dmabuf: %s\n", rv == 0 ? "PHYSICAL ADDRESS CONFIRMED"
                                   : "verification FAILED -- do not DMA here");
    dmabuf_free(&b);
    return rv;
}

/* ---- packet TX ---------------------------------------------------------
 *
 * The first DMA the chip performs on our behalf. Build one type-33 DCB in a
 * page whose physical address we have proven (dmabuf), point the channel at it,
 * set ENABLE.
 *
 * Start sequence, from cmicd_dma.c:233-240:
 *   write CMIC_CMCx_DMA_DESCy = physical address of the DCB
 *   CHy_DMA_CTRL |= PKTDMA_ENABLE            (DIRECTION already set for TX)
 *
 * Channel choice: EOS drives channel 0 (TX) and 1 (RX) continuously on CMC0.
 * Channels 2 and 3 read all zero and are untouched, so we use channel 2 and
 * stay out of the running driver's way. 0/1 are refused unless PKTTX_FORCE=1.
 *
 * The PURGE bit (control word bit 22, "Purge packet (TX)") is set by default.
 * The descriptor still goes through the DMA engine -- the point of the test --
 * but the chip discards the frame rather than letting it into the pipeline of a
 * switch currently holding an OSPF adjacency. PKTTX_REAL=1 sends it for real.
 *
 * Success = the chip writes the DCB status word (T33.15) and/or
 * PKT_COUNT_CHy_TXPKT increments: the engine read our descriptor out of our
 * page and acted on it. Whether the frame then forwards is a separate question
 * about pipeline state.
 *
 * Layout in the one page: DCB at offset 0, frame at offset 0x200.
 */
#define DCB_OFF    0x000
#define PKT_OFF    0x200
#define DCB_HIGIG  (1u << 19)
#define DCB_STAT   (1u << 20)
#define DCB_PURGE  (1u << 22)

static int pkttx(int cmc, int chan, int len)
{
    /* Unmistakable by construction. An earlier run identified 12-byte RX
     * descriptors as "our frames" from a 34 35 36 ... 3f payload tail, but the
     * same truncation happens to background traffic, so that identification did
     * not hold. DEADBEEF throughout, and a DA nothing else on this link uses,
     * removes the ambiguity. */
    static const uint8_t hdr[14] = {
        0x02,0xde,0xad,0xbe,0xef,0x01,   /* dst, locally administered */
        0x02,0xde,0xad,0xbe,0xef,0x02,   /* src                       */
        0x88,0xb5                        /* IEEE local experimental   */
    };
    static const uint8_t pat[4] = { 0xde, 0xad, 0xbe, 0xef };
    struct dmabuf b;
    volatile uint32_t *dcb;
    volatile uint8_t  *pkt;
    uint32_t ctrl, before, after, st = 0, save;
    int purge = getenv("PKTTX_REAL") == NULL;
    const char *dp = getenv("PKTTX_DPORT");
    int dport = dp ? atoi(dp) : -1;
    int hlen = 0, total;
    int i, rv = 1;

    if ((chan == 0 || chan == 1) && !getenv("PKTTX_FORCE")) {
        printf("pkttx: channel %d is EOS's (0=TX 1=RX). Use 2 or 3, "
               "or set PKTTX_FORCE=1.\n", chan);
        return 2;
    }
    if (len < 64)  len = 64;
    if (len > 512) len = 512;

    if (dmabuf_alloc_low(&b, 4096)) { dmabuf_free(&b); return 1; }
    dcb = (volatile uint32_t *)((char *)b.va + DCB_OFF);
    pkt = (volatile uint8_t  *)((char *)b.va + PKT_OFF);

    /* The module header goes in the DCB, words 2..4 -- NOT prepended to the
     * packet buffer. Captured live off EOS's TX ring (scdreset txsnoop), which
     * is the only way we could get it: EOS builds CPU TX in its l2mod_dma
     * KERNEL module, so 150 s of gdb on soc_pbsmh_field_set and
     * soc_pbsmh_array_set in libStrataXgsApi caught zero calls.
     *
     * Two real descriptors, snapshotted mid-flight:
     *
     *   LLDP -> Ethernet1   w2 81000000  w3 00000006  w4 20101400  ctrl ..HIGIG
     *   OSPFv3 multicast    w2 81000000  w3 00000205  w4 9805d800  ctrl ..HIGIG
     *
     * w2 = 0x81000000 is exactly the derived hdr[0]: start = 0x2 at bit 30,
     * header_type = SOC_SOBMH_FROM_CPU (0x1) at bit 24. dst_port sits in w3 at
     * bits 2..8 -- LLDP's 6 >> 2 = 1, and Ethernet1 is physical port 1, which
     * also settles the physical-vs-logical question. unicast is w4 bit 20: set
     * on the unicast LLDP, clear on the multicast OSPF. So the derivation in
     * docs/PACKET-DMA.md section 7 was right about the bits and wrong about
     * where the header lives.
     *
     * c_hg (control bit 19) is set on every EOS TX descriptor and flags that
     * the DCB carries the header.
     *
     * w4 is taken verbatim from the captured unicast case rather than rebuilt
     * from the field table: it carries queue_num and other fields whose exact
     * placement is not worth re-deriving when a known-good value is in hand.
     * PKTTX_W4 overrides it for experimenting.
     */
    for (i = 0; i < 14; i++)  pkt[i] = hdr[i];
    for (i = 14; i < len; i++) pkt[i] = pat[(i - 14) & 3];
    total = len;

    ctrl = (uint32_t)total | DCB_STAT | (purge ? DCB_PURGE : 0u)
           | (dport >= 0 ? DCB_HIGIG : 0u);
    for (i = 0; i < 16; i++) dcb[i] = 0;
    dcb[0] = (uint32_t)(b.pa + PKT_OFF);
    dcb[1] = ctrl;
    if (dport >= 0) {
        const char *w4env = getenv("PKTTX_W4");
        dcb[2] = 0x81000000u;                             /* start | SOBMH_FROM_CPU */
        /* dst_port at bits 2..8, plus bit 1 which EOS always sets on a
         * unicast port send (its LLDP to Ethernet1 reads 0x6, not 0x4).
         * Overridable with PKTTX_W3 while the remaining fields are unknown. */
        {
            const char *w3env = getenv("PKTTX_W3");
            dcb[3] = w3env ? (uint32_t)strtoul(w3env, NULL, 0)
                           : ((((uint32_t)(dport & 0x7f)) << 2) | 0x2u);
        }
        dcb[4] = w4env ? (uint32_t)strtoul(w4env, NULL, 0) : 0x20101400u;
        /* queue_num is w4 bits 8..19 (PBSMH_queue_num, HW 40 width 12). EOS's
         * captured unicast value carries 20 there. For a CPU-port-destined
         * frame this is what should pick the CPU CoS queue, and therefore
         * which RX channel claims it via COS_CTRL_RX -- EOS's channel 1 leaves
         * CoS 48..57 unclaimed, so that gap is the only one we can win warm. */
        {
            const char *q = getenv("PKTTX_QUEUE");
            if (q) {
                uint32_t qn = (uint32_t)strtoul(q, NULL, 0) & 0xfff;
                dcb[4] = (dcb[4] & ~0x000fff00u) | (qn << 8);
            }
        }
        hlen = 12;
    }
    __sync_synchronize();

    printf("pkttx: cmc %d chan %d, %d byte frame%s%s\n",
           cmc, chan, len, purge ? " (PURGE -- chip discards it)" : " (REAL SEND)",
           (dport >= 0) ? "" : ", no module header (pipeline will drop it)");
    if (dport >= 0)
        printf("  SOBMH    in DCB w2..w4: %08x %08x %08x  -> dst_port %d, c_hg set\n",
               dcb[2], dcb[3], dcb[4], dport);
    printf("  buffer   virt 0x%lx phys 0x%lx\n", (unsigned long)b.va, b.pa);
    printf("  DCB      @phys 0x%lx  addr 0x%08x  ctrl 0x%08x\n",
           b.pa + DCB_OFF, dcb[0], dcb[1]);

    save   = rd(asic, PKTDMA_CTRL(cmc, chan));
    before = rd(asic, PKTDMA_TXPKT(cmc, chan));
    printf("  CH_DMA_CTRL before 0x%08x, TXPKT before %u\n", save, before);

    /* Follow cmicd_dma.c: clear this channel's stale status, configure the
     * channel (DIRECTION + CNTLD_DESC_INTR_MODE), point it at the descriptor,
     * then raise ENABLE and CONTINUOUS_ENABLE together. The earlier version
     * wrote 0x1 then 0x3 and left out both of the SDK's other bits. */
    {
        uint32_t sc0 = rd(asic, PKTDMA_STAT_CLR(cmc));
        wr(asic, PKTDMA_STAT_CLR(cmc),
           PD_CNTLD_DESC_CLR(chan) | PD_DESCRD_CMPLT_CLR(chan));
        wr(asic, PKTDMA_STAT_CLR(cmc), sc0);
    }
    pktdma_stat_show("before ", cmc, chan);

    {
        const char *ce = getenv("PKTTX_CTRL");
        uint32_t cfg   = PD_DIRECTION | PD_CNTLD_DESC_INTR_MODE;
        uint32_t start = ce ? (uint32_t)strtoul(ce, NULL, 0)
                            : (cfg | PD_ENABLE | PD_CONTINUOUS_ENABLE);

        wr(asic, PKTDMA_CTRL(cmc, chan), cfg);
        wr(asic, PKTDMA_DESC(cmc, chan), (uint32_t)(b.pa + DCB_OFF));
        __sync_synchronize();
        printf("  DMA_DESC%d <- 0x%08x, CH_DMA_CTRL <- 0x%08x ...\n",
               chan, (uint32_t)(b.pa + DCB_OFF), start);
        wr(asic, PKTDMA_CTRL(cmc, chan), start);
    }

    for (i = 0; i < 2000; i++) {
        __sync_synchronize();
        st = dcb[15];
        if (st) break;
        if (rd(asic, PKTDMA_STAT(cmc)) & PD_CHAIN_DONE(chan)) break;
        usleep(500);
    }
    after = rd(asic, PKTDMA_TXPKT(cmc, chan));

    printf("  polled %d, DCB status 0x%08x (%u bytes)%s\n",
           i, st, st & 0xffff, (st & 0x80000000u) ? " DONE" : "");
    pktdma_stat_show("after  ", cmc, chan);
    printf("  CURR_DESC 0x%08x  CH_DMA_CTRL 0x%08x\n",
           rd(asic, PKTDMA_CURR_DESC(cmc, chan)),
           rd(asic, PKTDMA_CTRL(cmc, chan)));
    printf("  TXPKT %u -> %u\n", before, after);

    if (st || after != before) {
        printf("pkttx: DMA ENGINE MOVED OUR DESCRIPTOR\n");
        rv = 0;
    } else {
        printf("pkttx: no status write, no counter change -- not consumed\n");
    }

    wr(asic, PKTDMA_CTRL(cmc, chan), 0x1u | 0x4u);          /* ABORT */
    usleep(1000);
    wr(asic, PKTDMA_CTRL(cmc, chan), 0u);
    wr(asic, PKTDMA_DESC(cmc, chan), 0u);
    wr(asic, PKTDMA_CTRL(cmc, chan), save);
    printf("  channel restored (CH_DMA_CTRL 0x%08x)\n",
           rd(asic, PKTDMA_CTRL(cmc, chan)));

    dmabuf_free(&b);
    return rv;
}


/* Hex-dump host physical memory. Used to read the PBSMH + frame that a TX
 * descriptor points at, i.e. a real CPU-TX header built by a working driver
 * for this exact chip -- far more reliable than deriving the bit packing from
 * soc_pbsmh_v10_field_attr by hand. Read-only. */
static int pmem(unsigned long phys, int len)
{
    unsigned long pagebase = phys & ~0xfffUL, off = phys & 0xfffUL;
    size_t maplen = off + (size_t)len + 0x1000;
    volatile uint8_t *p;
    int fd, i;
    void *m;

    if ((fd = open("/dev/mem", O_RDONLY | O_SYNC)) < 0) { perror("open /dev/mem"); return 1; }
    m = mmap(NULL, maplen, PROT_READ, MAP_SHARED, fd, (off_t)pagebase);
    if (m == MAP_FAILED) { perror("mmap /dev/mem"); close(fd); return 1; }
    p = (volatile uint8_t *)m + off;

    for (i = 0; i < len; i += 16) {
        int k;
        printf("  %08lx ", phys + (unsigned long)i);
        for (k = 0; k < 16 && i + k < len; k++) printf(" %02x", p[i + k]);
        printf("  |");
        for (k = 0; k < 16 && i + k < len; k++) {
            uint8_t c = p[i + k];
            printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
        }
        printf("|\n");
    }
    munmap(m, maplen);
    close(fd);
    return 0;
}


/* ---- txsnoop: catch a live TX descriptor and dump its buffer -------------
 *
 * The goal is a real PBSMH built for this chip. EOS does NOT build it in
 * userspace -- 150 s of gdb on soc_pbsmh_field_set AND soc_pbsmh_array_set in
 * libStrataXgsApi caught zero calls, because CPU TX goes through Arista's
 * l2mod_dma kernel module instead.
 *
 * So catch it on the wire side of the ring. TX runs about one packet every 8 s,
 * and a descriptor holds a live buffer pointer only until the driver reclaims
 * it, so this polls in-process with everything already mapped rather than
 * re-running the tool (six separate invocations caught nothing).
 *
 * The ring base moves as the driver rebuilds it, so DMA_DESC0 is re-read every
 * pass and the page remapped only when it changes.
 */
static int txsnoop(int cmc, int chan, int secs)
{
    int fd, i, hits = 0;
    unsigned long ringpa = 0, mapped = 0;
    volatile uint32_t *ring = NULL;
    void *m = NULL;
    time_t deadline;

    if ((fd = open("/dev/mem", O_RDONLY | O_SYNC)) < 0) { perror("open /dev/mem"); return 1; }
    printf("txsnoop: cmc %d chan %d, up to %ds -- waiting for a live TX descriptor\n",
           cmc, chan, secs);
    fflush(stdout);

    deadline = time(NULL) + secs;
    while (time(NULL) < deadline && hits < 3) {
        ringpa = rd(asic, PKTDMA_DESC(cmc, chan));
        if (!ringpa) continue;
        if ((ringpa & ~0xfffUL) != mapped) {
            if (m) munmap(m, 0x2000);
            mapped = ringpa & ~0xfffUL;
            m = mmap(NULL, 0x2000, PROT_READ, MAP_SHARED, fd, (off_t)mapped);
            if (m == MAP_FAILED) { m = NULL; mapped = 0; continue; }
            ring = (volatile uint32_t *)m;
        }
        if (!ring) continue;

        /* 4 KB of ring = 64 descriptors of 16 words */
        for (i = 0; i < 64; i++) {
            const volatile uint32_t *d = ring + ((ringpa & 0xfffUL) / 4) + i * 16;
            uint32_t a = d[0], c = d[1];
            if (a && (c & 0xffff) && a < 0xfff00000u) {
                /* Snapshot the whole DCB NOW. The driver clears it within
                 * microseconds of the transfer completing, so re-reading it
                 * after printing the header returns all zeros -- which is
                 * exactly what the first version of this did. */
                uint32_t snap[16];
                unsigned long pb, po;
                void *bm;
                int w;
                for (w = 0; w < 16; w++) snap[w] = d[w];
                pb = a & ~0xfffUL; po = a & 0xfffUL;
                bm = mmap(NULL, po + 256, PROT_READ, MAP_SHARED, fd, (off_t)pb);
                printf("  HIT desc[%d] addr 0x%08x count %u ctrl 0x%08x%s%s%s%s\n",
                       i, a, c & 0xffff, c,
                       (c & (1u<<16)) ? " CHAIN" : "", (c & (1u<<19)) ? " HIGIG" : "",
                       (c & (1u<<20)) ? " STAT"  : "", (c & (1u<<22)) ? " PURGE" : "");
                printf("    DCB words (snapshot):");
                for (w = 0; w < 16; w++) {
                    if (w % 4 == 0) printf("\n      w%-2d ", w);
                    printf(" %08x", snap[w]);
                }
                printf("\n");
                if (bm != MAP_FAILED) {
                    volatile uint8_t *p = (volatile uint8_t *)bm + po;
                    int k, lead = (po >= 16) ? 16 : (int)po;
                    printf("    %d bytes BEFORE addr (is there a header?):\n     ", lead);
                    for (k = -lead; k < 0; k++) printf(" %02x", p[k]);
                    printf("\n    from addr:");
                    for (k = 0; k < 32; k++) {
                        if (k % 16 == 0) printf("\n     ");
                        printf(" %02x", p[k]);
                    }
                    printf("\n");
                    munmap(bm, po + 256);
                }
                fflush(stdout);
                hits++;
                break;
            }
        }
    }
    if (m) munmap(m, 0x2000);
    close(fd);
    printf("txsnoop: %d hit(s)\n", hits);
    return hits ? 0 : 1;
}


/* ---- packet RX ----------------------------------------------------------
 *
 * Receive on a channel of our own. TX proved host->chip; this proves chip->host
 * and, together with a frame addressed to the CPU port, closes the
 * CPU -> pipeline -> CPU loop.
 *
 * Ring layout: one low page holds the DCB ring, and each descriptor points at
 * its own separate low page. A 4 KB page cannot hold both the ring and the
 * buffers, and physically-contiguous multi-page memory is not something we can
 * ask this kernel for -- but nothing requires the buffers to be contiguous with
 * the ring or with each other, only that each buffer fits in one page.
 *
 * Every descriptor is CHAINed so the engine walks the ring; the driver fills
 * word 15 (DMA Status 0) with the received byte count as each is used.
 *
 * COS_CTRL_RX_0/_1 is a 64-bit bitmap of which CPU CoS queues land on this
 * channel. EOS's channel 1 holds 0xffffffff / 0xfc00ffff, i.e. everything
 * except CoS 48..57, so warm we can only legitimately claim that gap. Cold, we
 * own all four channels and can take everything.
 */
/* Receive ring size. Was a hard 8, which meant every pktrx run filled every
 * descriptor it was given and reported "8 of 8" -- a floor, never a rate. The
 * chip was never the limit; this array was. */
#define PKTRX_MAX_DESC 64

static int pktrx(int cmc, int chan, int ndesc, int secs, uint32_t cos0, uint32_t cos1)
{
    struct dmabuf pages[1 + PKTRX_MAX_DESC];
    volatile uint32_t *dcb;
    uint32_t save, savec0, savec1, before, after;
    int i, npages, got = 0;
    time_t deadline;

    if (chan == 0 || chan == 1) {
        if (!getenv("PKTRX_FORCE")) {
            printf("pktrx: channel %d is EOS's (0=TX 1=RX). Use 2 or 3, "
                   "or set PKTRX_FORCE=1.\n", chan);
            return 2;
        }
    }
    if (ndesc < 1) ndesc = 1;
    if (ndesc > PKTRX_MAX_DESC) ndesc = PKTRX_MAX_DESC;
    npages = 1 + ndesc;

    if (dmabuf_alloc_low_n(pages, npages)) return 1;
    dcb = (volatile uint32_t *)pages[0].va;

    for (i = 0; i < ndesc; i++) {
        volatile uint32_t *d = dcb + i * 16;
        int w;
        for (w = 0; w < 16; w++) d[w] = 0;
        d[0] = (uint32_t)pages[1 + i].pa;
        /* CHAIN on every descriptor EXCEPT the last. Setting it on the last
         * too makes the engine walk off the end of the ring into zeroed
         * memory instead of stopping -- the suspected cause of descriptors
         * reporting exactly 12 bytes and holding only a frame's DA+SA. */
        d[1] = 2048u | ((i < ndesc - 1) ? (1u << 16) : 0u);
    }
    __sync_synchronize();

    printf("pktrx: cmc %d chan %d, %d descriptors, %ds\n", cmc, chan, ndesc, secs);
    printf("  ring @phys 0x%lx, buffers:", pages[0].pa);
    for (i = 0; i < ndesc; i++) printf(" 0x%lx", pages[1 + i].pa);
    printf("\n");

    save   = rd(asic, PKTDMA_CTRL(cmc, chan));
    savec0 = rd(asic, PKTDMA_COS_RX0(cmc, chan));
    savec1 = rd(asic, PKTDMA_COS_RX1(cmc, chan));
    before = rd(asic, PKTDMA_RXPKT(cmc, chan));
    printf("  CTRL before 0x%08x  COS 0x%08x/0x%08x  RXPKT %u\n",
           save, savec0, savec1, before);

    {
        uint32_t sc0 = rd(asic, PKTDMA_STAT_CLR(cmc));
        wr(asic, PKTDMA_STAT_CLR(cmc),
           PD_CNTLD_DESC_CLR(chan) | PD_DESCRD_CMPLT_CLR(chan));
        wr(asic, PKTDMA_STAT_CLR(cmc), sc0);
    }
    pktdma_stat_show("before ", cmc, chan);
    {
        const char *ce = getenv("PKTRX_CTRL");
        uint32_t cfg   = PD_CNTLD_DESC_INTR_MODE;   /* DIRECTION clear = RX */
        uint32_t start = ce ? (uint32_t)strtoul(ce, NULL, 0)
                            : (cfg | PD_ENABLE | PD_CONTINUOUS_ENABLE);

        wr(asic, PKTDMA_CTRL(cmc, chan), cfg);
        wr(asic, PKTDMA_COS_RX0(cmc, chan), cos0);
        wr(asic, PKTDMA_COS_RX1(cmc, chan), cos1);
        wr(asic, PKTDMA_DESC(cmc, chan), (uint32_t)pages[0].pa);
        __sync_synchronize();
        printf("  COS <- 0x%08x/0x%08x, DESC <- 0x%08lx, CH_DMA_CTRL <- 0x%08x\n",
               cos0, cos1, pages[0].pa, start);
        wr(asic, PKTDMA_CTRL(cmc, chan), start);
    }

    deadline = time(NULL) + secs;
    while (time(NULL) < deadline) {
        for (i = 0; i < ndesc; i++) {
            volatile uint32_t *d = dcb + i * 16;
            uint32_t st = d[15];
            if (st && !(st & 0x40000000u)) {
                volatile uint8_t *p = (volatile uint8_t *)pages[1 + i].va;
                /* Dump a fixed span, NOT just `count` bytes: the open question
                 * is whether the missing 52 bytes are absent or merely
                 * unreported, and truncating to `count` cannot tell them
                 * apart. PKTRX_DUMP overrides the span. */
                const char *de = getenv("PKTRX_DUMP");
                int k, n = de ? atoi(de) : 80;
                printf("  RX desc[%d] stat 0x%08x -> %u bytes\n", i, st, st & 0xffff);
                printf("    DCB w2..w14 (EP_TO_CPU region):");
                for (k = 2; k < 15; k++) {
                    if ((k - 2) % 5 == 0) printf("\n     ");
                    printf(" %08x", d[k]);
                }
                printf("\n    buffer:");
                for (k = 0; k < n; k++) {
                    if (k % 16 == 0) printf("\n    ");
                    printf(" %02x", p[k]);
                }
                printf("\n");
                d[15] |= 0x40000000u;               /* our own "seen" marker */
                got++;
                fflush(stdout);
            }
        }
        usleep(2000);
    }
    after = rd(asic, PKTDMA_RXPKT(cmc, chan));
    printf("  RXPKT %u -> %u,  %d descriptor(s) filled\n", before, after, got);
    pktdma_stat_show("after  ", cmc, chan);
    printf("  CURR_DESC 0x%08x  CH_DMA_CTRL 0x%08x\n",
           rd(asic, PKTDMA_CURR_DESC(cmc, chan)),
           rd(asic, PKTDMA_CTRL(cmc, chan)));

    wr(asic, PKTDMA_CTRL(cmc, chan), 0x4u);          /* ABORT */
    usleep(2000);
    wr(asic, PKTDMA_CTRL(cmc, chan), 0u);
    wr(asic, PKTDMA_DESC(cmc, chan), 0u);
    wr(asic, PKTDMA_COS_RX0(cmc, chan), savec0);
    wr(asic, PKTDMA_COS_RX1(cmc, chan), savec1);
    wr(asic, PKTDMA_CTRL(cmc, chan), save);
    printf("  channel restored (CTRL 0x%08x COS 0x%08x/0x%08x)\n",
           rd(asic, PKTDMA_CTRL(cmc, chan)), rd(asic, PKTDMA_COS_RX0(cmc, chan)),
           rd(asic, PKTDMA_COS_RX1(cmc, chan)));

    for (i = 0; i < npages; i++) dmabuf_free(&pages[i]);
    return (got || after != before) ? 0 : 1;
}


/* ---- SerDes register read, the PortMod way -------------------------------
 *
 * sbmdio implements soc_sbus_mdio_reg_read (esw/drv.c) -- the LEGACY path. EOS
 * does not use it: the legacy symbols got zero breakpoint hits while the
 * PortMod ones fired, and trident2.c:16374 even carries a disabled guard
 * saying "TSC reg read/write is handled by PortMod internally".
 *
 * portmod_common_phy_sbus_reg_read (portmod_common.c:363) is a different
 * protocol, and the differences are exactly where a silent zero would come
 * from:
 *
 *   legacy (what we had)              PortMod (what EOS uses)
 *   ---------------------------       -----------------------------------
 *   write AER entry (reg 0xffde)      -- no AER write at all
 *   write target entry, entry[2]=0    write ONE entry, entry[2]=0
 *   read back, take entry[0]          read back, take entry[1]
 *
 * entry[0] = reg_addr | ((core_addr & 0x1f) << 19), with reg_addr carrying its
 * own devad/lane bits already. entry[2] = 0 selects read, 1 selects write.
 *
 * reg_val_offset defaults to 1 (PORTMOD_USER_ACCESS_REG_VAL_OFFSET_ZERO_GET is
 * the exception, not the rule) -- so taking entry[0] returns the address we
 * just wrote rather than the data, which reads as a plausible zero.
 */
static int pmread(int cmc, int blk, uint32_t core, uint32_t reg, uint32_t *out)
{
    uint32_t e[4];

    e[0] = reg | ((core & 0x1f) << 19);
    e[1] = 0;
    e[2] = 0;                 /* 0 = read, 1 = write */
    e[3] = 0;
    if (mem_put(cmc, blk, 0, WC_UCMEM_DATA, e, 4, 16)) return -1;
    usleep(1000);
    if (mem_get(cmc, blk, 0, WC_UCMEM_DATA, e, 4)) return -2;
    if (getenv("PMREAD_VERBOSE"))
        printf("[entry %08x %08x %08x %08x] ", e[0], e[1], e[2], e[3]);
    if (out) *out = e[1];     /* reg_val_offset = 1 */
    return 0;
}

static int do_pmread(int cmc, int blk, uint32_t core, uint32_t reg)
{
    uint32_t v = 0;
    int rv;

    printf("pmread: blk %d core 0x%02x reg 0x%08x ... ", blk, core, reg);
    fflush(stdout);
    rv = pmread(cmc, blk, core, reg, &v);
    if (rv) { printf("FAILED (%d)\n", rv); return 1; }
    printf("0x%08x\n", v);
    return 0;
}


/* ------------------------------------------------------------------ SMBus
 * SCD SMBus accelerator: read one byte from a device register.
 *
 * Ported from Arista's GPL scd-smbus.c (scd_smbus_master_xfer). The board's own
 * description names the accelerators and which bus each component sits on --
 * PortolaSPlus-7050SX2-72Q.fdl.py, smbusAccelInfo: nine accelerators at 0x8000
 * stride 0x80, and componentSmbusInfo pairing (accelId, busId):
 *
 *   Lm73    0x4a  "front-panel temp sensor"  accel 0 (0x8000) bus 0
 *   Max6658 0x4c  "board sensor"             accel 1 (0x8080) bus 0
 *   PSU 0 / PSU 1                            accel 1 buses 4 and 5
 *   power controller 0x4e                    accel 1 bus 6
 *
 * An SMBus read-byte-data is two i2c messages -- write the register index, then
 * read one byte -- so ss (the total transfer size) is (1+1) + (1+1) = 4, and the
 * accelerator is fed four request words. The last response carries the data.
 */
#define SMB_BASE(accel)  (0x8000u + 0x80u * (unsigned)(accel))
#define SMB_REQ_OFF      0x10u
#define SMB_CS_OFF       0x20u
#define SMB_RESP_OFF     0x30u

/* request: d:8 ss:6 ed:1 br:1 dat:2 t:2 sp:1 da:1 dod:1 st:1 bs:4 ti:4 */
static uint32_t smb_req(unsigned d, unsigned ss, unsigned ed, unsigned br,
                        unsigned t, unsigned sp, unsigned da, unsigned dod,
                        unsigned st, unsigned bs, unsigned ti)
{
    return ((d & 0xff)) | ((ss & 0x3f) << 8) | ((ed & 1) << 14) |
           ((br & 1) << 15) | ((t & 3) << 18) | ((sp & 1) << 20) |
           ((da & 1) << 21) | ((dod & 1) << 22) | ((st & 1) << 23) |
           ((bs & 0xf) << 24) | ((ti & 0xf) << 28);
}
#define CS_NRS(v)  ((v) & 0x3ff)
#define CS_NRQ(v)  (((v) >> 16) & 0x3ff)
#define CS_BRB(v)  (((v) >> 26) & 1)
#define CS_FE(v)   (((v) >> 30) & 1)
#define RESP_D(v)        ((v) & 0xff)
#define RESP_BUSERR(v)   (((v) >> 8) & 1)
#define RESP_TIMEOUT(v)  (((v) >> 9) & 1)
#define RESP_ACKERR(v)   (((v) >> 10) & 1)
#define RESP_FE(v)       (((v) >> 31) & 1)

static int smb_enter(uint32_t base)
{
    uint32_t cs = rd(scd, base + SMB_CS_OFF);
    int i;

    /* A previously asserted reset must be released before anything else, and
     * it is not covered by the fe/brb/nrq/nrs test below: an idle-but-held
     * master reads cs=0x90002400, where every one of those fields is clear and
     * only bit 31 is set, so the test passes and the master stays down. */
    if (cs & (1u << 31)) {
        wr(scd, base + SMB_CS_OFF, cs & ~(1u << 31));
        usleep(1000);
        cs = rd(scd, base + SMB_CS_OFF);
    }

    if (CS_FE(cs) || CS_BRB(cs) || CS_NRQ(cs) || CS_NRS(cs)) {
        wr(scd, base + SMB_CS_OFF, cs | (1u << 31));      /* rst */
        for (i = 1; i <= 8; i *= 2) {
            usleep(i * 1000);
            cs = rd(scd, base + SMB_CS_OFF);
            if (!CS_FE(cs) && !CS_BRB(cs) && !CS_NRQ(cs) && !CS_NRS(cs)) {
                /* RELEASE THE RESET. Leaving rst asserted holds the master
                 * down and every later transaction on that accelerator returns
                 * "no response" with bit 31 still set in cs -- observed as
                 * cs=0x90002400 on the second read after a successful first. */
                wr(scd, base + SMB_CS_OFF, cs & ~(1u << 31));
                return 0;
            }
        }
        fprintf(stderr, "smbus: master would not reset, cs=0x%08x\n", cs);
        return -1;
    }
    return 0;
}

static int do_smbrd(int accel, int bus, int addr, int reg)
{
    uint32_t base = SMB_BASE(accel), cs, resp = 0;
    const int ss = 4;
    int i, ti = 0, got = 0;

    if (smb_enter(base) < 0) return 1;

    /* Timing and end-of-data come from the driver's per-address bus_params,
     * defaulting to { .t = 1, .datw = 3, .datr = 3, .ed = 0 }. Use those
     * defaults rather than inventing values -- an earlier version passed t=0
     * and ed=1, which happened to read these two sensors correctly but is not
     * what the reference implementation does. */
    const unsigned PT = 1, PED = 0;      /* default_smbus_params.t / .ed */

    /* msg 0: write the register index */
    wr(scd, base + SMB_REQ_OFF,
       smb_req((unsigned)(addr << 1) | 0, ss, 0, 0, PT, 0, 0, 1, 1, bus, ti++));
    wr(scd, base + SMB_REQ_OFF,
       smb_req((unsigned)reg, 0, PED, 0, PT, 0, 0, 1, 0, bus, ti++));
    /* msg 1: read one byte back */
    wr(scd, base + SMB_REQ_OFF,
       smb_req((unsigned)(addr << 1) | 1, 0, 0, 0, PT, 0, 0, 1, 1, bus, ti++));
    wr(scd, base + SMB_REQ_OFF,
       smb_req(0, 0, PED, 0, PT, 1, 0, 0, 0, bus, ti++));  /* sp=1, last word */

    for (i = 0; i < 200; i++) {                          /* ~200 ms */
        cs = rd(scd, base + SMB_CS_OFF);
        if (CS_NRS(cs) >= (unsigned)ss) break;
        usleep(1000);
    }
    cs = rd(scd, base + SMB_CS_OFF);
    if (CS_NRS(cs) == 0) {
        printf("smbus a%d b%d 0x%02x reg 0x%02x: no response, cs=0x%08x\n",
               accel, bus, addr, reg, cs);
        return 1;
    }
    for (i = 0; i < (int)CS_NRS(cs) && i < 8; i++) {
        resp = rd(scd, base + SMB_RESP_OFF);
        got++;
        if (RESP_BUSERR(resp) || RESP_TIMEOUT(resp) || RESP_ACKERR(resp)) {
            printf("smbus a%d b%d 0x%02x reg 0x%02x: error resp=0x%08x%s%s%s\n",
                   accel, bus, addr, reg, resp,
                   RESP_BUSERR(resp)  ? " bus_conflict" : "",
                   RESP_TIMEOUT(resp) ? " timeout"      : "",
                   RESP_ACKERR(resp)  ? " ack_err"      : "");
            return 1;
        }
    }
    printf("smbus a%d b%d 0x%02x reg 0x%02x = 0x%02x (%d)  [%d resp, cs=0x%08x]\n",
           accel, bus, addr, reg, RESP_D(resp), RESP_D(resp), got, cs);
    return 0;
}


/* ------------------------------------------------------- Crow fan CPLD
 *
 * THE FAN CONTROLLER, AND HOW ITS REGISTER MAP WAS OBTAINED.
 *
 * This board's fans hang off a CPLD at SMBus address 0x60 on accelerator 0
 * bus 0 -- the CPU card's bus. Two earlier attempts to find its PWM register by
 * sweeping it POWERED THE SWITCH OFF, once through our own reads and once
 * through the vendor's own tool; see docs/HAZARD-FAN-CONTROLLER-PROBING.md.
 *
 * None of the map below was probed. It is read from Arista's own GPL-2.0
 * sources, which describe this exact part:
 *
 *   src/crow-fan-driver.c        "crow-cpld-fans", 4 fans, the register
 *                                numbers and the tach conversion
 *   arista/platforms/cpu/crow.py cpld = scd.newComponent(CrowFanCpld,
 *                                          addr=scd.i2cAddr(hwmonBus, 0x60))
 *
 * That second line is what ties the driver to this board: the fan CPLD is an
 * I2C device at 0x60 on an SCD bus, which is exactly where ours answers. Note
 * that Arista's OTHER CPLD, the system one, is at 0x23 on the x86 SMBus -- a
 * different device entirely, and conflating the two is easy.
 *
 * Register map, from crow-fan-driver.c:
 *
 *   0x00..0x07   tach 1..4, low byte then high byte
 *   0x10..0x13   PWM for fan 1..4, 0..255            <- the thing we were hunting
 *   0x18..0x1B   fan ID 1..4
 *   0x21         presence bitmap
 *   0x24 / 0x25  green / red LED
 *   0x40         CPLD revision
 *
 *   RPM = 6000000 / ((high << 8) | low)
 */
#define CROW_ADDR      0x60
#define CROW_ACCEL     0
#define CROW_BUS       0
#define CROW_TACH(n)   (unsigned)((n) * 2)      /* n = 0..3, low byte */
#define CROW_PWM(n)    (unsigned)(0x10 + (n))
#define CROW_ID(n)     (unsigned)(0x18 + (n))
#define CROW_PRESENT   0x21u
#define CROW_REV       0x40u
#define CROW_NFAN      4
#define CROW_MAX_PWM   255

/* Write one byte to a device register over an SCD SMBus master.
 *
 * Same shape as do_smbrd's first half: address+W, register, data. There is no
 * read phase and no repeated start, so three request words rather than four. */
static int smb_wr_reg(int accel, int bus, int addr, unsigned reg,
                      unsigned val)
{
    uint32_t base = SMB_BASE(accel), cs, resp;
    const int ss = 3;
    const unsigned PT = 1, PED = 0;
    int i;

    if (smb_enter(base) < 0) return -1;

    wr(scd, base + SMB_REQ_OFF,
       smb_req((unsigned)(addr << 1) | 0, ss, 0, 0, PT, 0, 0, 1, 1, bus, 0));
    wr(scd, base + SMB_REQ_OFF,
       smb_req(reg & 0xff, 0, 0,   0, PT, 0, 0, 1, 0, bus, 1));
    wr(scd, base + SMB_REQ_OFF,
       smb_req(val & 0xff, 0, PED, 0, PT, 1, 0, 1, 0, bus, 2));  /* sp=1 */

    for (i = 0; i < 200; i++) {
        cs = rd(scd, base + SMB_CS_OFF);
        if (CS_NRS(cs) >= (unsigned)ss) break;
        usleep(1000);
    }
    cs = rd(scd, base + SMB_CS_OFF);
    if (CS_NRS(cs) == 0) return -1;
    for (i = 0; i < (int)CS_NRS(cs) && i < 8; i++) {
        resp = rd(scd, base + SMB_RESP_OFF);
        if (RESP_BUSERR(resp) || RESP_TIMEOUT(resp) || RESP_ACKERR(resp))
            return -1;
    }
    return 0;
}

/* Quiet read-byte: do_smbrd prints, which is wrong inside a control loop. */
static int smb_rd_reg(int accel, int bus, int addr, unsigned reg)
{
    uint32_t base = SMB_BASE(accel), cs, resp = 0;
    const int ss = 4;
    const unsigned PT = 1, PED = 0;
    int i, ti = 0;

    if (smb_enter(base) < 0) return -1;
    wr(scd, base + SMB_REQ_OFF,
       smb_req((unsigned)(addr << 1) | 0, ss, 0, 0, PT, 0, 0, 1, 1, bus, ti++));
    wr(scd, base + SMB_REQ_OFF,
       smb_req(reg & 0xff, 0, PED, 0, PT, 0, 0, 1, 0, bus, ti++));
    wr(scd, base + SMB_REQ_OFF,
       smb_req((unsigned)(addr << 1) | 1, 0, 0, 0, PT, 0, 0, 1, 1, bus, ti++));
    wr(scd, base + SMB_REQ_OFF,
       smb_req(0, 0, PED, 0, PT, 1, 0, 0, 0, bus, ti++));

    for (i = 0; i < 200; i++) {
        cs = rd(scd, base + SMB_CS_OFF);
        if (CS_NRS(cs) >= (unsigned)ss) break;
        usleep(1000);
    }
    cs = rd(scd, base + SMB_CS_OFF);
    if (CS_NRS(cs) == 0) return -1;
    for (i = 0; i < (int)CS_NRS(cs) && i < 8; i++) {
        resp = rd(scd, base + SMB_RESP_OFF);
        if (RESP_BUSERR(resp) || RESP_TIMEOUT(resp) || RESP_ACKERR(resp))
            return -1;
    }
    return (int)RESP_D(resp);
}

static int crow_rpm(int fan)
{
    int lo = smb_rd_reg(CROW_ACCEL, CROW_BUS, CROW_ADDR, CROW_TACH(fan));
    int hi = smb_rd_reg(CROW_ACCEL, CROW_BUS, CROW_ADDR, CROW_TACH(fan) + 1);
    unsigned t;

    if (lo < 0 || hi < 0) return -1;
    t = ((unsigned)hi << 8) | (unsigned)lo;
    if (!t) t = 1;                       /* the driver does exactly this */
    return (int)(6000000u / t);
}

/* Identity check before we ever write. A CPLD revision that reads back as
 * 0x00 or 0xff means we are not talking to what we think we are, and writing a
 * PWM value into an unknown device on a live switch is not something to do on
 * an assumption. */
static int crow_probe(void)
{
    int rev = smb_rd_reg(CROW_ACCEL, CROW_BUS, CROW_ADDR, CROW_REV);

    if (rev < 0) {
        fprintf(stderr, "crow: no response from 0x%02x on accel %d bus %d\n",
                CROW_ADDR, CROW_ACCEL, CROW_BUS);
        return -1;
    }
    if (rev == 0x00 || rev == 0xff) {
        fprintf(stderr, "crow: implausible CPLD revision 0x%02x -- refusing "
                "to write\n", rev);
        return -1;
    }
    return rev;
}

/* A FLOOR, ALWAYS.
 *
 * The vendor's own thermal control never commands below 30%, and neither do we.
 * A switch whose fans are commanded to zero because of an arithmetic slip does
 * not report a bug, it cooks. `fanset N 0` is therefore clamped, and the only
 * way past the floor is an explicit override that has to be typed. */
#define CROW_MIN_PCT 30

static int crow_set_pct(int fan, int pct, int allow_below_floor)
{
    int pwm;

    if (pct > 100) pct = 100;
    if (pct < CROW_MIN_PCT && !allow_below_floor) {
        fprintf(stderr, "crow: %d%% is below the %d%% floor, clamping\n",
                pct, CROW_MIN_PCT);
        pct = CROW_MIN_PCT;
    }
    if (pct < 0) pct = 0;
    pwm = pct * CROW_MAX_PWM / 100;
    return smb_wr_reg(CROW_ACCEL, CROW_BUS, CROW_ADDR, CROW_PWM(fan), pwm);
}

/* Temperatures, from the two sensors this board carries.
 *
 *   Lm73    0x4a  accel 0 bus 0   front-panel / inlet
 *   Max6658 0x4c  accel 1 bus 0   board
 *
 * Both report whole degrees in the high byte of register 0x00, which is all a
 * cooling loop needs -- the fractional bits below it are not worth the extra
 * transaction. A sensor that will not answer returns -1 and the caller treats
 * that as "assume the worst", never as "assume fine".
 */
static int temp_lm73(void)   { return smb_rd_reg(0, 0, 0x4a, 0x00); }
static int temp_max6658(void){ return smb_rd_reg(1, 0, 0x4c, 0x00); }

static int temp_hottest(int *inlet, int *board)
{
    int a = temp_lm73(), b = temp_max6658(), hot = -1;

    if (inlet) *inlet = a;
    if (board) *board = b;
    if (a > hot) hot = a;
    if (b > hot) hot = b;
    return hot;
}

/* THE COOLING LOOP.
 *
 * Proportional on the hottest sensor, with a floor and a ceiling, and hysteresis
 * in the form of a slew limit so the fans do not audibly hunt.
 *
 *   at or below TMIN  ->  MIN%          (30, matching the vendor's own floor)
 *   at or above TMAX  ->  100%
 *   between           ->  linear
 *
 * Deliberately NOT a PID. The vendor's runs one per sensor with an integral
 * term, and an integral term on a loop this slow mostly buys overshoot and a
 * windup bug. Proportional with a floor is the behaviour that matters: fans
 * rise with temperature and never stop.
 *
 * FAILURE IS NOT QUIET. If a sensor cannot be read, or the CPLD stops
 * answering, the loop commands 100% and says so. The failure mode of a cooling
 * loop must be "too much cooling", and the one thing it must never do is keep
 * the last value because it could not measure anything.
 */
#define TH_TMIN   35      /* degC at or below which fans sit at the floor */
#define TH_TMAX   65      /* degC at or above which fans are flat out     */
#define TH_MAXPCT 100

static int thermal_target(int hot)
{
    if (hot < 0)        return TH_MAXPCT;             /* unreadable -> flat out */
    if (hot <= TH_TMIN) return CROW_MIN_PCT;
    if (hot >= TH_TMAX) return TH_MAXPCT;
    return CROW_MIN_PCT +
           (hot - TH_TMIN) * (TH_MAXPCT - CROW_MIN_PCT) / (TH_TMAX - TH_TMIN);
}

static volatile int th_stop;
static void th_sigint(int sig) { (void)sig; th_stop = 1; }

static int do_thermal(int interval, int once)
{
    int cur = TH_MAXPCT, i;

    if (crow_probe() < 0) return 1;

    /* Start at full and come down. Starting low and ramping up would leave the
     * box under-cooled for the first interval if it is already hot. */
    for (i = 0; i < CROW_NFAN; i++) crow_set_pct(i, TH_MAXPCT, 0);

    signal(SIGINT,  th_sigint);
    signal(SIGTERM, th_sigint);

    printf("thermal: floor %d%%, %dC->%dC maps %d%%->%d%%, every %ds\n",
           CROW_MIN_PCT, TH_TMIN, TH_TMAX, CROW_MIN_PCT, TH_MAXPCT, interval);
    fflush(stdout);

    while (!th_stop) {
        int inlet, board, hot = temp_hottest(&inlet, &board);
        int want = thermal_target(hot), step, bad = 0;

        /* Slew limit: up fast, down slowly. Reacting instantly downward makes
         * the fans oscillate around a threshold; reacting slowly upward would
         * be a thermal risk, so the asymmetry is deliberate. */
        step = (want > cur) ? (want - cur) : ((cur - want) > 5 ? 5 : (cur - want));
        cur  = (want > cur) ? want : cur - step;

        for (i = 0; i < CROW_NFAN; i++)
            if (crow_set_pct(i, cur, 0) < 0) bad++;

        printf("thermal: inlet %3dC board %3dC hot %3dC -> %3d%%%s\n",
               inlet, board, hot, cur,
               bad ? "   *** CPLD WRITE FAILED ***" : "");
        fflush(stdout);

        if (bad) {
            /* Cannot command the fans. Say so loudly and keep trying; the
             * hardware holds its last value, which is why we start high. */
            fprintf(stderr, "thermal: %d of %d fans did not accept a setting\n",
                    bad, CROW_NFAN);
        }
        if (once) break;
        for (i = 0; i < interval && !th_stop; i++) sleep(1);
    }

    /* LEAVE THE BOX SAFE. Whatever stopped us -- Ctrl-C, SIGTERM, a crash of
     * the surrounding script -- the fans must not be left at whatever low value
     * a cool moment happened to command. */
    printf("thermal: stopping, setting fans to %d%%\n", TH_MAXPCT);
    for (i = 0; i < CROW_NFAN; i++) crow_set_pct(i, TH_MAXPCT, 0);
    fflush(stdout);
    return 0;
}

static int do_fanshow(void)
{
    int rev = crow_probe(), present, i;

    if (rev < 0) return 1;
    present = smb_rd_reg(CROW_ACCEL, CROW_BUS, CROW_ADDR, CROW_PRESENT);
    printf("crow fan cpld 0x%02x  rev 0x%02x  present 0x%02x\n",
           CROW_ADDR, rev, present < 0 ? 0 : present);
    for (i = 0; i < CROW_NFAN; i++) {
        int pwm = smb_rd_reg(CROW_ACCEL, CROW_BUS, CROW_ADDR, CROW_PWM(i));
        int id  = smb_rd_reg(CROW_ACCEL, CROW_BUS, CROW_ADDR, CROW_ID(i));
        int rpm = crow_rpm(i);
        /* PRESENCE IS ACTIVE LOW. crow-fan-driver.c reads it as
         *     *present = ~(data >> index) & 0x01;
         * so a CLEAR bit means the fan is there. Reading it the obvious way
         * round reports every fan absent while all four are plainly spinning. */
        int here = (present < 0) ? 1 : !((present >> i) & 1);
        printf("  fan%d  pwm %3d (%3d%%)  rpm %5d  id 0x%02x%s\n",
               i + 1, pwm, pwm < 0 ? 0 : pwm * 100 / CROW_MAX_PWM, rpm, id,
               here ? "" : "   [absent]");
    }
    return 0;
}


/* --------------------------------------------------------------- Linux I2C
 * Read a byte from a device register on a standard Linux i2c bus.
 *
 * This is a DIFFERENT path from smbrd above. The SCD's own accelerators carry
 * the temperature sensors and the PSUs; the CPLD -- which is the fan controller
 * -- hangs off the x86 southbridge SMBus instead, as /dev/i2c-1, and EOS reaches
 * it with `i2cget -y 1 0x23 <reg>` (FpgaPlugin/Alameda.py). busybox has no
 * i2cget, hence this.
 *
 * The ioctl structures are declared here rather than pulled from
 * <linux/i2c-dev.h> so the build does not depend on that header being present.
 */
#define I2C_SLAVE_IOCTL   0x0703
#define I2C_SMBUS_IOCTL   0x0720
#define I2C_SMBUS_READ    1
#define I2C_SMBUS_BYTE_DATA 2

union i2c_smbus_data_u {
    uint8_t  byte;
    uint16_t word;
    uint8_t  block[34];
};
struct i2c_smbus_ioctl_data_s {
    uint8_t  read_write;
    uint8_t  command;
    uint32_t size;
    union i2c_smbus_data_u *data;
};

static int do_i2crd(int bus, int addr, int reg)
{
    struct i2c_smbus_ioctl_data_s args;
    union i2c_smbus_data_u data;
    char path[32];
    int fd, rv;

    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
    fd = open(path, O_RDWR);
    if (fd < 0) {
        printf("i2c: open %s: %s (kernel needs CONFIG_I2C_CHARDEV + a bus driver)\n",
               path, strerror(errno));
        return 1;
    }
    if (ioctl(fd, I2C_SLAVE_IOCTL, addr) < 0) {
        printf("i2c: bus %d addr 0x%02x: %s\n", bus, addr, strerror(errno));
        close(fd);
        return 1;
    }
    memset(&data, 0, sizeof(data));
    args.read_write = I2C_SMBUS_READ;
    args.command    = (uint8_t)reg;
    args.size       = I2C_SMBUS_BYTE_DATA;
    args.data       = &data;
    rv = ioctl(fd, I2C_SMBUS_IOCTL, &args);
    close(fd);
    if (rv < 0) {
        printf("i2c-%d 0x%02x reg 0x%02x: %s\n", bus, addr, reg, strerror(errno));
        return 1;
    }
    printf("i2c-%d 0x%02x reg 0x%02x = 0x%02x (%d)\n",
           bus, addr, reg, data.byte, data.byte);
    return 0;
}

static void usage(void)
{
    printf("usage:\n"
           "  scdreset status                     SCD reset register + watchdog\n"
           "  scdreset release                    clear core+pcie reset, PCI rescan\n"
           "  scdreset wd <deciseconds>           arm watchdog (0 = disarm)\n"
           "  scdreset pet <deciseconds>          fork a loop that re-arms every 5s\n"
           "  scdreset peek <off> [n]             read SCD BAR0\n"
           "  scdreset poke <off> <val>           write SCD BAR0\n"
           "  scdreset apeek <off> [n]            read ASIC BAR0\n"
           "  scdreset cfgdump                    dump ASIC PCI config space\n"
           "  scdreset pcicfg                     apply arista-bde PCI setup\n"
           "  scdreset coldprobe                  READ-ONLY escalating CMIC probe\n"
           "  scdreset apoke <off> <val>          write ASIC BAR0 (no read-back)\n"
           "  scdreset phasea3                    ring map then timeout (SDK order) + verify\n"
           "  scdreset uchalt                     halt uC cores via MCS PIO window\n"
           "  scdreset phasep                     PCIe USERIF timeout + PIO purge (do FIRST)\n"
           "  scdreset phase0                     endian + CPS reset (pre-chip_reset)\n"
           "  scdreset phasea                     SBUS timeout + ring map (verifies)\n"
           "  scdreset phasea2                    ring map + timeout, NO read-back, then schan\n"
           "  scdreset schan-regs [cmc]           dump SCHAN registers (default cmc 0)\n"
           "  scdreset schan-read <hdr> <addr> <nwords> [cmc]\n"
           "  scdreset schan-write <hdr> <addr> <data> [cmc]   (SCHAN_FORCE=1 to override deny)\n"
           "  scdreset schan-wtest <blk> <addr> <pattern> [cmc]  save/write/verify/restore\n"
           "  scdreset phaseb [cmc]               release pipeline block resets (TOP_SOFT_RESET_REG)\n"
           "  scdreset llsreset [cmc]             reset the LLS scheduler nodes (soc_td2_lls_reset)\n"
           "  scdreset llsinit  [cmc]             full soc_td2_lls_init sequence\n"
           "  scdreset thdinit  [cmc]             MMU per-port buffer/threshold config\n"
           "  scdreset phasec [cmc]               LCPLL config + PLL reset release, check lock\n"
           "  scdreset portdump [blk]             read XLPORT/XLMAC regs (default blk 15)\n"
           "  scdreset dmabuf [bytes]             alloc locked buf, resolve+verify phys addr\n"
           "  scdreset pkttx [chan] [len] [cmc]   TX one frame via packet DMA (purge by default)\n"
           "  scdreset pktrx [chan] [ndesc] [secs] [cos0] [cos1]  RX on our own channel\n"
           "  scdreset dmaregs [cmc]              CMICm packet DMA registers (read-only)\n"
           "  scdreset dcbdump <phys> [n]         dump type-33 DCBs from host memory (read-only)\n"
           "  scdreset pmem <phys> [len]          hex-dump host physical memory (read-only)\n"
           "  scdreset txsnoop [chan] [secs] [cmc]  catch a live TX descriptor + dump its buffer\n"
           "  scdreset tscdump [cmc]              read all 32 PGW_CL TSC ctrl regs (read-only)\n"
           "  scdreset tscreset <pgwblk 6-13> <tsc 0-3> [cmc]   TD2+ SerDes out-of-reset\n"
           "  scdreset topdump                    dump all TOP-block regs (non-zero)\n"
           "  scdreset sbmdio <blk> <phy> <reg>          SBUS-MDIO read (legacy path)\n"
           "  scdreset pmread <blk> <core> <reg> [cmc]  SerDes read, PortMod protocol\n"
           "  scdreset sbmdioscan <blk> <reg>            sweep phy addrs over SBUS-MDIO\n"
           "  scdreset miimread <int> <bus> <phy> <reg>   MDIO clause-22 read\n"
           "  scdreset miimscan <int> <bus>              sweep PHY addrs on a bus\n"
           "  scdreset scdscan <lo> <hi>          non-zero SCD regs in a range (read-only)\n"
           "  scdreset smbdump                    dump SCD SMBus master registers\n"
           "  scdreset smbscan <master> <bus> [lo hi]  probe I2C addrs (read-only)\n"
           "  scdreset smbmux <master> <bus> <muxaddr> [lo hi]  scan behind a PCA954x mux\n"
           "  scdreset smbwrite <master> <bus> <addr> <byte>    single-byte I2C write\n"
           "  scdreset memwtest <blk> <acc> <addr> <dlen> <w0> [w1..]  READ/WRITE_MEM round trip\n"
           "  scdreset memr <blk> <acc> <addr> [words]            READ_MEM, print\n"
           "  scdreset memw <blk> <acc> <addr> <dlen> <w0> [w1..]  WRITE_MEM and LEAVE it\n"
           "  scdreset dumpset <file> [cmc]       read a list of table entries\n"
           "  scdreset fanread                    fan rpm/pwm/presence (SB800 PM2)\n"
           "  scdreset fanset <1..4|all> <pwm>    set fan PWM, clamped to a floor\n"
           "  scdreset replay <file> [cmc]        re-issue a captured write sequence\n"
           "        REPLAY_FROM/REPLAY_MAX/REPLAY_SKIP_BLK/REPLAY_STOP_ON_ERR\n");
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "status";
    const char *e;

    /* Unbuffered: if a write wedges the host, the last line printed is the
     * last thing attempted. Buffering would lose exactly that. */
    setvbuf(stdout, NULL, _IONBF, 0);

    if ((e = getenv("CORE_BIT"))) core_bit = atoi(e);
    if ((e = getenv("PCIE_BIT"))) pcie_bit = atoi(e);

    if (!strcmp(cmd, "status")) {
        if (!(scd = map_res(SCD_RES, SCD_MAP_SIZE, 0))) return 1;
        show_reset(); show_wd(); return 0;
    }
    if (!strcmp(cmd, "release")) {
        if (!(scd = map_res(SCD_RES, SCD_MAP_SIZE, 1))) return 1;
        return do_release();
    }
    if (!strcmp(cmd, "wd") && argc > 2) {
        uint32_t v = wd_value((unsigned)strtoul(argv[2], NULL, 0));
        if (!(scd = map_res(SCD_RES, SCD_MAP_SIZE, 1))) return 1;
        printf("writing 0x%08x to 0x%04x\n", v, WD_REG);
        wr(scd, WD_REG, v);
        show_wd();
        return 0;
    }
    if (!strcmp(cmd, "pet") && argc > 2) {
        uint32_t v = wd_value((unsigned)strtoul(argv[2], NULL, 0));
        pid_t pid;
        if (!(scd = map_res(SCD_RES, SCD_MAP_SIZE, 1))) return 1;
        pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid > 0) {
            printf("petting 0x%08x every 5s in pid %d (kill %d to stop)\n",
                   v, (int)pid, (int)pid);
            return 0;
        }
        setsid();
        for (;;) { wr(scd, WD_REG, v); sleep(5); }
    }
    if (!strcmp(cmd, "peek") && argc > 2) {
        uint32_t off = strtoul(argv[2], NULL, 0);
        int n = argc > 3 ? atoi(argv[3]) : 1;
        if (off >= SCD_MAP_SIZE) { printf("offset out of range\n"); return 2; }
        if (!(scd = map_res(SCD_RES, SCD_MAP_SIZE, 0))) return 1;
        for (int i = 0; i < n; i++)
            printf("0x%04x = 0x%08x\n", off + i * 4, rd(scd, off + i * 4));
        return 0;
    }
    if (!strcmp(cmd, "poke") && argc > 3) {
        uint32_t off = strtoul(argv[2], NULL, 0), val = strtoul(argv[3], NULL, 0);
        if (off >= SCD_MAP_SIZE) { printf("offset out of range\n"); return 2; }
        if (!(scd = map_res(SCD_RES, SCD_MAP_SIZE, 1))) return 1;
        printf("write 0x%08x -> 0x%04x\n", val, off);
        wr(scd, off, val);
        printf("readback 0x%04x = 0x%08x\n", off, rd(scd, off));
        return 0;
    }
    /* apeeklist <file> -- read every offset in <file> in ONE process.
     *
     * Dumping the 4,063 named CMIC registers as 4,063 separate apeek
     * invocations does not fit in a boot window: each one spawns a process and
     * re-mmaps the BAR. This maps once and walks the list.
     *
     * Deliberately reads only offsets it is GIVEN. The docs record that blind
     * MMIO sweeps reset this box twice, so the caller supplies a list built
     * from cmicm-register-map.json with the known cold-chip read hazards
     * (0x0100b8-0x0100d4, 0x0101d8) already removed. Do not "optimise" this
     * into a range scan.
     */
    if (!strcmp(cmd, "apeeklist") && argc > 2) {
        char line[256];
        FILE *f = fopen(argv[2], "r");
        if (!f) { perror(argv[2]); return 1; }
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 0))) { fclose(f); return 1; }
        /*
         * Stop the moment a read returns 0xffffffff.
         *
         * Unpowered CMIC blocks read as all-ones and the NEXT read into them
         * hangs the host. Found the hard way three times: the SER block at
         * 0x12000 (0xffffffff at 0x12008, hang at 0x1200c) and the MMU block at
         * 0x23000 (0xffffffff at 0x23000, hang after). Each cost a boot cycle
         * and a watchdog recovery to discover.
         *
         * Treating all-ones as "this block is dead, stop here" turns that into
         * one skipped chunk instead of a hang, so a whole-space dump can find
         * every dead block in a single run rather than one per boot.
         */
        while (fgets(line, sizeof line, f)) {
            unsigned long off; char name[128] = ""; uint32_t v;
            if (sscanf(line, "%lx %127s", &off, name) < 1) continue;
            if (off >= ASIC_MAP_SIZE) continue;
            v = rd(asic, (uint32_t)off);
            printf("%08lx %s 0x%08x\n", off, name, v);
            if (v == 0xffffffffu) {
                printf("%08lx DEAD-BLOCK-STOP %s\n", off, name);
                break;
            }
        }
        fclose(f);
        return 0;
    }
    if (!strcmp(cmd, "apeek") && argc > 2) {
        uint32_t off = strtoul(argv[2], NULL, 0);
        int n = argc > 3 ? atoi(argv[3]) : 1;
        if (off >= ASIC_MAP_SIZE) { printf("offset out of range\n"); return 2; }
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 0))) return 1;
        for (int i = 0; i < n; i++)
            printf("0x%06x = 0x%08x\n", off + i * 4, rd(asic, off + i * 4));
        return 0;
    }
    if (!strcmp(cmd, "cfgdump")) return do_cfgdump();
    if (!strcmp(cmd, "pcicfg")) return do_pcicfg();
    if (!strcmp(cmd, "coldprobe")) {
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 0))) return 1;
        return do_coldprobe();
    }
    if (!strcmp(cmd, "apoke") && argc > 3) {
        uint32_t off = strtoul(argv[2], NULL, 0), val = strtoul(argv[3], NULL, 0);
        if (off >= ASIC_MAP_SIZE) { printf("offset out of range\n"); return 2; }
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        printf("write 0x%08x -> 0x%06x (no read-back)\n", val, off);
        wr(asic, off, val);
        printf("issued\n");
        return 0;
    }
    if (!strcmp(cmd, "uchalt")) {
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_uchalt();
    }
    if (!strcmp(cmd, "phasea3")) {
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return phaseA3(0, 0x1c10c200, 0x38400000, 13);
    }
    if (!strcmp(cmd, "phasep")) {
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return phaseP();
    }
    if (!strcmp(cmd, "phase0")) {
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return phase0();
    }
    if (!strcmp(cmd, "phasea2")) {
        /* ground-truth read captured from EOS via gdb: READ_MEM_CMD */
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return phaseA_noverify(0, 0x1c10c200, 0x38400000, 13);
    }
    if (!strcmp(cmd, "phasea") || !strcmp(cmd, "ringmap")) {
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return phaseA();
    }
    if (!strcmp(cmd, "schan-regs")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 0))) return 1;
        schan_regs(cmc);
        return 0;
    }
    /* memwtest <blk> <acc> <addr> <dlen> <w0> [w1 w2 ...]
     * The pattern is given as explicit words so the caller controls exactly
     * what lands in each one -- important when a field is narrower than the
     * entry and the upper bits read back as zero. */
    if (!strcmp(cmd, "memwtest") && argc > 6) {
        uint32_t pat[MEM_MAX_WORDS];
        int blk = atoi(argv[2]), acc = atoi(argv[3]);
        uint32_t addr = strtoul(argv[4], NULL, 0);
        uint32_t dlen = strtoul(argv[5], NULL, 0);
        int words = argc - 6, i;
        if (words > MEM_MAX_WORDS) words = MEM_MAX_WORDS;
        for (i = 0; i < words; i++)
            pat[i] = strtoul(argv[6 + i], NULL, 0);
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return mem_wtest(0, blk, acc, addr, pat, words, dlen);
    }
    /* replay <file> [cmc] -- re-issue a captured write sequence */
    if (!strcmp(cmd, "replay") && argc > 2) {
        int cmc = argc > 3 ? atoi(argv[3]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_replay(argv[2], cmc);
    }
    /* dumpset <file> [cmc] */
    if (!strcmp(cmd, "dumpset") && argc > 2) {
        int cmc = argc > 3 ? atoi(argv[3]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_dumpset(argv[2], cmc);
    }
    /* memr <blk> <acc> <addr> [words] */
    /* schancmd <opc> <blk> <acc> <dlen> [w0 ...] -- raw S-Channel command.
     * 0x15 is INIT_CFAP. Reply words are dumped so a NACK is distinguishable
     * from a command the chip simply ignored. */
    if (!strcmp(cmd, "schancmd") && argc > 5) {
        uint32_t val[MEM_MAX_WORDS];
        uint32_t opc  = strtoul(argv[2], NULL, 0);
        int blk = atoi(argv[3]), acc = atoi(argv[4]);
        uint32_t dlen = strtoul(argv[5], NULL, 0);
        int words = argc - 6, i;
        if (words < 0) words = 0;
        if (words > MEM_MAX_WORDS) words = MEM_MAX_WORDS;
        for (i = 0; i < words; i++) val[i] = strtoul(argv[6 + i], NULL, 0);
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return schan_cmd(0, opc, blk, acc, dlen, val, words, 4);
    }
    /* regw <blk> <acc> <addr> <dlen> <w0> [w1 ...] -- WRITE a register */
    if (!strcmp(cmd, "regw") && argc > 6) {
        uint32_t val[MEM_MAX_WORDS];
        int blk = atoi(argv[2]), acc = atoi(argv[3]);
        uint32_t addr = strtoul(argv[4], NULL, 0);
        uint32_t dlen = strtoul(argv[5], NULL, 0);
        int words = argc - 6, i;
        if (words > MEM_MAX_WORDS) words = MEM_MAX_WORDS;
        for (i = 0; i < words; i++) val[i] = strtoul(argv[6 + i], NULL, 0);
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return reg_write_cmd(0, blk, acc, addr, val, words, dlen);
    }
    /* regr <blk> <acc> <addr> [dlen] [words] -- READ a register, not a memory */
    if (!strcmp(cmd, "regr") && argc > 4) {
        int blk = atoi(argv[2]), acc = atoi(argv[3]);
        uint32_t addr = strtoul(argv[4], NULL, 0);
        uint32_t dlen = argc > 5 ? strtoul(argv[5], NULL, 0) : 4;
        int words = argc > 6 ? atoi(argv[6]) : 1;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return reg_read_cmd(0, blk, acc, addr, dlen, words);
    }
    if (!strcmp(cmd, "memr") && argc > 4) {
        int blk = atoi(argv[2]), acc = atoi(argv[3]);
        uint32_t addr = strtoul(argv[4], NULL, 0);
        int words = argc > 5 ? atoi(argv[5]) : 1;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return mem_read_cmd(0, blk, acc, addr, words);
    }
    /* memw <blk> <acc> <addr> <dlen> <w0> [w1 ...] -- write and LEAVE it */
    if (!strcmp(cmd, "memw") && argc > 6) {
        uint32_t val[MEM_MAX_WORDS];
        int blk = atoi(argv[2]), acc = atoi(argv[3]);
        uint32_t addr = strtoul(argv[4], NULL, 0);
        uint32_t dlen = strtoul(argv[5], NULL, 0);
        int words = argc - 6, i;
        if (words > MEM_MAX_WORDS) words = MEM_MAX_WORDS;
        for (i = 0; i < words; i++) val[i] = strtoul(argv[6 + i], NULL, 0);
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return mem_write_cmd(0, blk, acc, addr, val, words, dlen);
    }
    if (!strcmp(cmd, "scdscan") && argc > 3) {
        uint32_t lo = strtoul(argv[2], NULL, 0), hi = strtoul(argv[3], NULL, 0);
        uint32_t step = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : 0x10;
        if (!(scd = map_res(SCD_RES, SCD_MAP_SIZE, 0))) return 1;
        return do_scdscan(lo, hi, step);
    }
    if (!strcmp(cmd, "smbdump")) {
        if (!(scd = map_res(SCD_RES, SCD_MAP_SIZE, 0))) return 1;
        return do_smbdump();
    }
    if (!strcmp(cmd, "smbscan") && argc > 3) {
        int master = atoi(argv[2]), bus = atoi(argv[3]);
        int lo = argc > 4 ? (int)strtoul(argv[4], NULL, 0) : 0x08;
        int hi = argc > 5 ? (int)strtoul(argv[5], NULL, 0) : 0x77;
        if (!(scd = map_res(SCD_RES, SCD_MAP_SIZE, 1))) return 1;
        return do_smbscan(master, bus, lo, hi);
    }
    if (!strcmp(cmd, "smbmux") && argc > 4) {
        int master = atoi(argv[2]), bus = atoi(argv[3]);
        int mux = (int)strtoul(argv[4], NULL, 0);
        int lo = argc > 5 ? (int)strtoul(argv[5], NULL, 0) : 0x08;
        int hi = argc > 6 ? (int)strtoul(argv[6], NULL, 0) : 0x77;
        if (!(scd = map_res(SCD_RES, SCD_MAP_SIZE, 1))) return 1;
        return do_smbmux(master, bus, mux, lo, hi);
    }
    if (!strcmp(cmd, "smbwrite") && argc > 5) {
        int master = atoi(argv[2]), bus = atoi(argv[3]);
        int addr = (int)strtoul(argv[4], NULL, 0);
        unsigned val = (unsigned)strtoul(argv[5], NULL, 0);
        int rv;
        if (!(scd = map_res(SCD_RES, SCD_MAP_SIZE, 1))) return 1;
        rv = smb_write_byte(master, bus, addr, val);
        printf("smbwrite: m%d b%d 0x%02x <- 0x%02x : %s\n", master, bus, addr,
               val, rv == 0 ? "ACK" : (rv == -1 ? "no completion" : "no ACK"));
        return rv;
    }
    if (!strcmp(cmd, "pmread") && argc > 4) {
        int blk = atoi(argv[2]);
        uint32_t core = strtoul(argv[3], NULL, 0);
        uint32_t reg = strtoul(argv[4], NULL, 0);
        int cmc = argc > 5 ? atoi(argv[5]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_pmread(cmc, blk, core, reg);
    }
    if (!strcmp(cmd, "sbmdio") && argc > 4) {
        int blk = atoi(argv[2]);
        uint32_t phy = strtoul(argv[3], NULL, 0), reg = strtoul(argv[4], NULL, 0);
        int cmc = argc > 5 ? atoi(argv[5]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_sbmdio(cmc, blk, phy, reg);
    }
    if (!strcmp(cmd, "sbmdioscan") && argc > 3) {
        int blk = atoi(argv[2]);
        uint32_t reg = strtoul(argv[3], NULL, 0);
        int cmc = argc > 4 ? atoi(argv[4]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_sbmdioscan(cmc, blk, reg);
    }
    if (!strcmp(cmd, "miimread") && argc > 5) {
        int internal = atoi(argv[2]), bus = atoi(argv[3]);
        int phy = (int)strtoul(argv[4], NULL, 0);
        int reg = (int)strtoul(argv[5], NULL, 0);
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_miimread(0, internal, bus, phy, reg);
    }
    if (!strcmp(cmd, "miimscan") && argc > 3) {
        int internal = atoi(argv[2]), bus = atoi(argv[3]);
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_miimscan(0, internal, bus);
    }
    if (!strcmp(cmd, "topdump")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_topdump(cmc);
    }
    if (!strcmp(cmd, "portdump")) {
        int blk = argc > 2 ? atoi(argv[2]) : 15;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return port_dump(0, blk);
    }
    if (!strcmp(cmd, "phasec")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return phaseC(cmc);
    }
    if (!strcmp(cmd, "tscreset")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return tsc_reset(cmc);
    }
    if (!strcmp(cmd, "tscinit")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return tsc_init(cmc);
    }
    if (!strcmp(cmd, "xlportbring")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return xlport_bring(cmc);
    }
    if (!strcmp(cmd, "i2crd")) {
        int bus, addr, reg;
        if (argc < 5) {
            printf("usage: scdreset i2crd <bus> <addr> <reg>\n");
            printf("  e.g. i2crd 1 0x23 0x00   CPLD fan tach (RPM = 6000000/tach)\n");
            return 2;
        }
        bus  = (int)strtoul(argv[2], NULL, 0);
        addr = (int)strtoul(argv[3], NULL, 0);
        reg  = (int)strtoul(argv[4], NULL, 0);
        return do_i2crd(bus, addr, reg);
    }
    /* ---- front-panel LEDs -----------------------------------------------
     * Only enough to blank them. Driving them from link state needs the SDK,
     * which knows it for all 72 ports, and that lives in the bridge
     * (tools/sdkshim/ledsync.c). */
    if (!strcmp(cmd, "ledclear")) {
        unsigned i;
        if (!(scd = map_res(SCD_RES, SCD_MAP_SIZE, 1))) return 1;
        /* DARK IS HONEST; STALE IS NOT.
         *
         * Between power-on and the bridge starting, these registers hold
         * whatever the previous occupant of this machine left in them -- the
         * vendor OS's last picture of its own link state, which is not ours.
         * A panel showing links that may not exist is worse than a dark one,
         * so blank it and let the bridge light what is genuinely up. */
        for (i = 0; i < 48; i++) wr(scd, 0x6100u + 0x10u * i, 0);   /* SFP+ */
        for (i = 0; i < 24; i++) wr(scd, 0x6400u + 0x10u * i, 0);   /* QSFP */
        printf("ledclear: 72 port LEDs blanked\n");
        return 0;
    }

    /* ---- fans and cooling ------------------------------------------------
     * The register map behind these comes from Arista's GPL crow-fan-driver.c,
     * not from probing this device -- probing it powered the box off twice.
     * See docs/HAZARD-FAN-CONTROLLER-PROBING.md. */
    if (!strcmp(cmd, "fanshow")) {
        if (!(scd = map_res(SCD_RES, SCD_MAP_SIZE, 1))) return 1;
        return do_fanshow();
    }
    if (!strcmp(cmd, "fanpct")) {
        int fan, pct, force = 0;
        if (argc < 4) {
            printf("usage: scdreset fanpct <fan 1-4|all> <percent> [force]\n");
            printf("  percent is clamped to a %d%% floor unless 'force' is\n",
                   CROW_MIN_PCT);
            printf("  given -- a switch with its fans commanded to zero does\n");
            printf("  not report a fault, it cooks.\n");
            return 2;
        }
        pct   = (int)strtol(argv[3], NULL, 0);
        force = (argc > 4 && !strcmp(argv[4], "force"));
        if (!(scd = map_res(SCD_RES, SCD_MAP_SIZE, 1))) return 1;
        if (crow_probe() < 0) return 1;
        if (!strcmp(argv[2], "all")) {
            int i, bad = 0;
            for (i = 0; i < CROW_NFAN; i++)
                if (crow_set_pct(i, pct, force) < 0) bad++;
            printf("fanpct: all fans -> %d%%%s\n", pct,
                   bad ? "  (some writes FAILED)" : "");
            return bad ? 1 : 0;
        }
        fan = (int)strtol(argv[2], NULL, 0) - 1;
        if (fan < 0 || fan >= CROW_NFAN) {
            fprintf(stderr, "fanpct: fan must be 1..%d or 'all'\n", CROW_NFAN);
            return 2;
        }
        if (crow_set_pct(fan, pct, force) < 0) {
            fprintf(stderr, "fanpct: write FAILED\n");
            return 1;
        }
        printf("fanpct: fan%d -> %d%%\n", fan + 1, pct);
        return 0;
    }
    if (!strcmp(cmd, "thermal")) {
        int interval = (argc > 2) ? (int)strtol(argv[2], NULL, 0) : 10;
        int once     = (argc > 3 && !strcmp(argv[3], "once"));
        if (interval < 1) interval = 1;
        if (!(scd = map_res(SCD_RES, SCD_MAP_SIZE, 1))) return 1;
        return do_thermal(interval, once);
    }

    if (!strcmp(cmd, "smbrd")) {
        int accel, bus, addr, reg;
        if (argc < 6) {
            printf("usage: scdreset smbrd <accel> <bus> <addr> <reg>\n");
            printf("  e.g. smbrd 0 0 0x4a 0x00   Lm73 front-panel temp\n");
            printf("       smbrd 1 0 0x4c 0x00   Max6658 board temp\n");
            return 2;
        }
        accel = (int)strtoul(argv[2], NULL, 0);
        bus   = (int)strtoul(argv[3], NULL, 0);
        addr  = (int)strtoul(argv[4], NULL, 0);
        reg   = (int)strtoul(argv[5], NULL, 0);
        if (!(scd = map_res(SCD_RES, SCD_MAP_SIZE, 1))) return 1;
        return do_smbrd(accel, bus, addr, reg);
    }
    if (!strcmp(cmd, "sbusrd")) {
        int blk, lane, cmc;
        uint32_t reg;
        if (argc < 5) {
            printf("usage: scdreset sbusrd <xlport-blk> <lane> <reg> [cmc]\n");
            return 1;
        }
        blk = (int)strtol(argv[2], NULL, 0);
        lane = (int)strtol(argv[3], NULL, 0);
        reg = (uint32_t)strtoul(argv[4], NULL, 0);
        cmc = argc > 5 ? atoi(argv[5]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_sbusrd(cmc, blk, lane, reg);
    }
    if (!strcmp(cmd, "tscmicropre")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return lane_ops_run(cmc, "tscmicropre", tsc_micro_pre,
                            (int)(sizeof(tsc_micro_pre) / sizeof(tsc_micro_pre[0])));
    }
    if (!strcmp(cmd, "tscmicropost")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return lane_ops_run(cmc, "tscmicropost", tsc_micro_post,
                            (int)(sizeof(tsc_micro_post) / sizeof(tsc_micro_post[0])));
    }
    if (!strcmp(cmd, "ucodemdio") || !strcmp(cmd, "ucodever")) {
        int only = -1, cmc = 0, verify = !strcmp(cmd, "ucodever");
        if (argc < 3) {
            printf("usage: scdreset %s <file> [xlport-blk] [cmc]\n", cmd);
            return 1;
        }
        if (argc > 3) only = (int)strtol(argv[3], NULL, 0);
        if (argc > 4) cmc = atoi(argv[4]);
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_ucodemdio(cmc, argv[2], only, verify);
    }
    if (!strcmp(cmd, "portinit")) {
        int only = argc > 2 ? (int)strtol(argv[2], NULL, 0) : -1;
        int cmc = argc > 3 ? atoi(argv[3]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_portinit(cmc, only);
    }
    if (!strcmp(cmd, "portfull")) {
        int only = argc > 2 ? (int)strtol(argv[2], NULL, 0) : -1;
        int cmc = argc > 3 ? atoi(argv[3]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_portfull(cmc, only);
    }
    if (!strcmp(cmd, "memsnap") && argc > 2) {
        int cmc = argc > 3 ? atoi(argv[3]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_memsnap(cmc, argv[2]);
    }
    if (!strcmp(cmd, "regsnap") && argc > 2) {
        int cmc = argc > 3 ? atoi(argv[3]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_regsnap(cmc, argv[2]);
    }
    if (!strcmp(cmd, "regdiff") && argc > 3) {
        int detail = argc > 4 ? atoi(argv[4]) : 200;
        return do_regdiff(argv[2], argv[3], detail);
    }
    if (!strcmp(cmd, "memdiff") && argc > 3) {
        int detail = argc > 4 ? atoi(argv[4]) : 200;
        return do_memdiff(argv[2], argv[3], detail);
    }
    if (!strcmp(cmd, "chipinit") || !strcmp(cmd, "chippost")) {
        int only = argc > 2 ? (int)strtol(argv[2], NULL, 0) : -1;
        int cmc = argc > 3 ? atoi(argv[3]) : 0;
        int n = (int)(sizeof(chip_ops) / sizeof(chip_ops[0]));
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        if (!strcmp(cmd, "chipinit"))
            return do_chipreplay(cmc, 0, CHIP_SPLIT, only, "chipinit");
        return do_chipreplay(cmc, CHIP_SPLIT, n, only, "chippost");
    }
    if (!strcmp(cmd, "polfix")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_polfix(cmc);
    }
    if (!strcmp(cmd, "polstat")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_polstat(cmc);
    }
    if (!strcmp(cmd, "prbs")) {
        int tb, tl, rb, rl, mode = 0, cmc = 0;
        if (argc < 6) {
            printf("usage: scdreset prbs <txblk> <txlane> <rxblk> <rxlane> [mode] [cmc]\n");
            return 1;
        }
        tb = (int)strtol(argv[2], NULL, 0); tl = (int)strtol(argv[3], NULL, 0);
        rb = (int)strtol(argv[4], NULL, 0); rl = (int)strtol(argv[5], NULL, 0);
        if (argc > 6) mode = (int)strtol(argv[6], NULL, 0);
        if (argc > 7) cmc = atoi(argv[7]);
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_prbs(cmc, tb, tl, rb, rl, mode);
    }
    if (!strcmp(cmd, "prbslb")) {
        int blk, lane, mode = 0, cmc = 0;
        if (argc < 4) {
            printf("usage: scdreset prbslb <blk> <lane> [mode] [cmc]\n");
            return 1;
        }
        blk = (int)strtol(argv[2], NULL, 0); lane = (int)strtol(argv[3], NULL, 0);
        if (argc > 4) mode = (int)strtol(argv[4], NULL, 0);
        if (argc > 5) cmc = atoi(argv[5]);
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_prbslb(cmc, blk, lane, mode);
    }
    if (!strcmp(cmd, "prbsoff")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_prbsoff(cmc);
    }
    if (!strcmp(cmd, "linkstat")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_linkstat(cmc);
    }
    if (!strcmp(cmd, "rslvd")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_rslvd(cmc);
    }
    if (!strcmp(cmd, "lanerst")) {
        int only = argc > 2 ? (int)strtol(argv[2], NULL, 0) : -1;
        int cmc = argc > 3 ? atoi(argv[3]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_lanerst(cmc, only);
    }
    if (!strcmp(cmd, "pmdinit")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_pmdinit(cmc);
    }
    if (!strcmp(cmd, "ucready")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_ucready(cmc);
    }
    if (!strcmp(cmd, "ucstat")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_ucstat(cmc);
    }
    if (!strcmp(cmd, "sbuswr")) {
        int blk, lane, cmc;
        uint32_t reg, val;
        if (argc < 6) {
            printf("usage: scdreset sbuswr <xlport-blk> <lane> <reg> <val> [cmc]\n");
            return 1;
        }
        blk = (int)strtol(argv[2], NULL, 0);
        lane = (int)strtol(argv[3], NULL, 0);
        reg = (uint32_t)strtoul(argv[4], NULL, 0);
        val = (uint32_t)strtoul(argv[5], NULL, 0);
        cmc = argc > 6 ? atoi(argv[6]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_sbuswr(cmc, blk, lane, reg, val);
    }
    if (!strcmp(cmd, "sbusid")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return do_sbusid(cmc);
    }
    if (!strcmp(cmd, "ucmemacc")) {
        int on, cmc;
        if (argc < 3) {
            printf("usage: scdreset ucmemacc <0|1> [cmc]\n");
            return 1;
        }
        on = (int)strtol(argv[2], NULL, 0);
        cmc = argc > 3 ? atoi(argv[3]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return ucmem_access(cmc, on);
    }
    if (!strcmp(cmd, "thdinit")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return thd_init(cmc);
    }
    if (!strcmp(cmd, "llsinit")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return lls_init(cmc);
    }
    if (!strcmp(cmd, "tdminit")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return tdm_init(cmc);
    }
    if (!strcmp(cmd, "physdump")) {
        if (argc < 4) { printf("usage: scdreset physdump <phys> <len>\n"); return 1; }
        return phys_dump(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
    }
    if (!strcmp(cmd, "fanread")) {
        return fan_read();
    }
    if (!strcmp(cmd, "fanset")) {
        if (argc < 4) { printf("usage: scdreset fanset <1..4|all> <pwm 0..255>\n"); return 1; }
        return fan_set(argv[2], argv[3]);
    }
    if (!strcmp(cmd, "xlportinit")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return xlport_init(cmc);
    }
    if (!strcmp(cmd, "tdmverify")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return tdm_verify(cmc);
    }
    if (!strcmp(cmd, "llsreset")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return lls_reset(cmc);
    }
    if (!strcmp(cmd, "phaseb")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return phaseB(cmc);
    }
    if (!strcmp(cmd, "schan-write") && argc > 4) {
        uint32_t hdr = strtoul(argv[2], NULL, 0);
        uint32_t addr = strtoul(argv[3], NULL, 0);
        uint32_t data = strtoul(argv[4], NULL, 0);
        int cmc = argc > 5 ? atoi(argv[5]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return schan_write_op(cmc, hdr, addr, data);
    }
    if (!strcmp(cmd, "schan-wtest") && argc > 4) {
        int blk = atoi(argv[2]);
        uint32_t addr = strtoul(argv[3], NULL, 0);
        uint32_t pat = strtoul(argv[4], NULL, 0);
        int cmc = argc > 5 ? atoi(argv[5]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return schan_wtest(cmc, blk, addr, pat);
    }
    if (!strcmp(cmd, "pktrx")) {
        int chan  = argc > 2 ? atoi(argv[2]) : 3;
        int nd    = argc > 3 ? atoi(argv[3]) : 4;
        int secs  = argc > 4 ? atoi(argv[4]) : 20;
        uint32_t c0 = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 0) : 0xffffffffu;
        uint32_t c1 = argc > 6 ? (uint32_t)strtoul(argv[6], NULL, 0) : 0xffffffffu;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return pktrx(0, chan, nd, secs, c0, c1);
    }
    if (!strcmp(cmd, "pkttx")) {
        int chan = argc > 2 ? atoi(argv[2]) : 2;
        int len  = argc > 3 ? atoi(argv[3]) : 64;
        int cmc  = argc > 4 ? atoi(argv[4]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return pkttx(cmc, chan, len);
    }
    if (!strcmp(cmd, "dmabuf")) {
        size_t len = argc > 2 ? (size_t)strtoul(argv[2], NULL, 0) : 4096;
        return do_dmabuf(len);
    }
    if (!strcmp(cmd, "dmaregs")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return dmaregs(cmc);
    }
    if (!strcmp(cmd, "txsnoop")) {
        int chan = argc > 2 ? atoi(argv[2]) : 0;
        int secs = argc > 3 ? atoi(argv[3]) : 60;
        int cmc  = argc > 4 ? atoi(argv[4]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return txsnoop(cmc, chan, secs);
    }
    if (!strcmp(cmd, "pmem") && argc > 2) {
        unsigned long phys = strtoul(argv[2], NULL, 0);
        int len = argc > 3 ? atoi(argv[3]) : 64;
        return pmem(phys, len);
    }
    if (!strcmp(cmd, "dcbdump") && argc > 2) {
        unsigned long phys = strtoul(argv[2], NULL, 0);
        int n = argc > 3 ? atoi(argv[3]) : 1;
        return dcbdump(phys, n);
    }
    if (!strcmp(cmd, "tscdump")) {
        int cmc = argc > 2 ? atoi(argv[2]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return tscdump(cmc);
    }
    if (!strcmp(cmd, "tscreset") && argc > 3) {
        int blk = atoi(argv[2]);
        int idx = atoi(argv[3]);
        int cmc = argc > 4 ? atoi(argv[4]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return tscreset(cmc, blk, idx);
    }
    if (!strcmp(cmd, "schan-read") && argc > 4) {
        uint32_t hdr = strtoul(argv[2], NULL, 0);
        uint32_t addr = strtoul(argv[3], NULL, 0);
        int n = atoi(argv[4]);
        int cmc = argc > 5 ? atoi(argv[5]) : 0;
        if (!(asic = map_res(ASIC_RES, ASIC_MAP_SIZE, 1))) return 1;
        return schan_read(cmc, hdr, addr, n);
    }

    usage();
    return 2;
}
