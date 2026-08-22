/*
 * Proof of concept: bring the BCM56855 up with the real Broadcom SDK, driven
 * by our own user-space BDE (bde_shim.c), on a chip we reset ourselves.
 *
 * The point is not to ship the SDK. It is to get ONE known-good cold
 * initialisation on this board so we can diff our own sequence against it --
 * `dumpset` and the state-diff tooling already exist for exactly that. Once we
 * know what we are missing, the SDK comes back out.
 *
 * Sequence, all unmodified SDK above the shim:
 *
 *   sal_core_init / sal_appl_init   SDK's own threading, locks, timers
 *   soc_cm_init                     config manager
 *   soc_cm_device_create            register the device by PCI id
 *   soc_cm_device_init              hand it our vectors -- the BDE substitution
 *   soc_attach                      allocates soc_control_t. NOTHING ELSE.
 *                                   The SDK's own comment at drv.c:21205 is
 *                                   explicit: "No chip initialization is done
 *                                   other than masking all interrupts, see
 *                                   soc_init or soc_reset_init." An earlier
 *                                   version of this header claimed soc_attach
 *                                   was "soc_reset + soc_do_init, the real
 *                                   thing" -- it is not, and that is why a
 *                                   successful soc_attach issued ZERO
 *                                   S-Channel operations.
 *   soc_reset_init                  the real thing: chip reset + soc_do_init,
 *                                   including _soc_trident2_mmu_init and
 *                                   soc_td2_lls_init
 *
 * Run it with the ASIC already released from reset and on the PCI bus
 * (`scdreset release` does that), and with the DMA pool reserved on the kernel
 * command line -- see docs/SDK-ROUTE.md.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

#include <shared/bsltypes.h>
#include <shared/bslenum.h>
#include <shared/bslext.h>
#include <shared/bsl.h>

#include <sal/types.h>
#include <sal/core/boot.h>
#include <sal/core/libc.h>
#include <sal/appl/sal.h>
#include <soc/cm.h>
#include <soc/cmext.h>
#include <soc/cmic.h>    /* soc_esw_miim_read/write -- cmic.h:931,941 */
#include <soc/drv.h>
#include <soc/devids.h>
#include <soc/schanmsg.h>
#include <soc/mem.h>
#include <bcm/init.h>
#include <bcm/port.h>
#include <bcm/link.h>
#include <bcm/rx.h>
#include <bcm/vlan.h>
#include <bcm/l3.h>
#include <bcm/switch.h>
#include <bcm/l2.h>
#include <bcm/multicast.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <bcm/pkt.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <execinfo.h>
#include <soc/error.h>   /* soc_errmsg() is a macro over _SHR_ERRMSG */
#include <sal/appl/config.h>
#include <sys/stat.h>   /* mkfifo -- SDKPOC_DAEMON */
#include <sys/mman.h>   /* SCD BAR for the copper PHY bus */
#include <fcntl.h>
#include <soc/phy/phyctrl.h>   /* phy_i2c_bus_func_hook_set */
#include <errno.h>

extern soc_cm_device_vectors_t bde_shim_vectors;
extern int bde_shim_init(void);
extern uint32_t bde_shim_devid(uint16_t *dev_id, uint8_t *rev_id);
extern void bde_shim_dma_stats(void);
extern void bde_shim_config_stats(void);
extern void bde_shim_mmio_flush(void);
extern void bde_shim_mmio_stats(void);
extern void bde_shim_sbd_flush(void);
extern void bde_shim_sbd_stats(void);

/*
 * The SDK expects the application to supply compiled-in config defaults --
 * this is the hook that stands in for a config.bcm file.
 *
 * "Empty is correct for a first run" was wrong, and it cost three boots. With
 * no properties set, soc_cm_device_init fails: the recovered known-good binary
 * returns SOC_E_MEMORY there and this build segfaults instead. config.bcm's own
 * header says why -- the shim connects no interrupt, so polled_irq_mode must be
 * set or soc_attach dies at drv.c:21184. Loading it is what took the known-good
 * binary from "rv=-2 Out of memory" to "soc_attached(0) = 1".
 *
 * Parsed here rather than in bde_shim because this is the hook the SAL already
 * calls at the right moment, before any device exists.
 */
static int config_loaded;

void sal_config_init_defaults(void)
{
    const char *path = getenv("SDKPOC_CONFIG");
    char line[512];
    FILE *f;
    int n = 0;

    /* The SAL is supposed to call this itself, but it is a weak hook and if it
     * is never invoked the properties silently do not exist -- which looks
     * exactly like the failure we are fixing. main() calls it again as a
     * backstop, so guard against loading twice. */
    if (config_loaded) {
        return;
    }
    config_loaded = 1;

    if (path == NULL) {
        printf("config: SDKPOC_CONFIG unset -- running on driver defaults, "
               "which is known NOT to survive soc_cm_device_init\n");
        return;
    }
    if ((f = fopen(path, "r")) == NULL) {
        printf("config: cannot open %s\n", path);
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *key = line, *val, *end;

        while (*key == ' ' || *key == '\t') key++;
        if (*key == '#' || *key == '\n' || *key == '\r' || *key == '\0') {
            continue;
        }
        if ((val = strchr(key, '=')) == NULL) {
            continue;
        }
        *val++ = '\0';
        for (end = val - 2; end >= key && (*end == ' ' || *end == '\t'); end--) {
            *end = '\0';
        }
        while (*val == ' ' || *val == '\t') val++;
        /* Strip a trailing comment. config.bcm writes
         *     portmap_1=13:10   # Ethernet1, MMU port 1
         * and passing the whole tail as the value made the SDK reject every
         * portmap entry -- "Port config error !!" and soc_attach failed. Only
         * leading-# lines were being skipped before. */
        {
            char *hash = strchr(val, '#');
            if (hash != NULL) *hash = '\0';
        }
        for (end = val + strlen(val) - 1;
             end >= val && (*end == '\n' || *end == '\r' ||
                            *end == ' '  || *end == '\t'); end--) {
            *end = '\0';
        }
        if (*key != '\0' && sal_config_set(key, val) >= 0) {
            n++;
        }
    }
    fclose(f);
    printf("config: %d properties from %s\n", n, path);
}

/*
 * S-Channel tracing.
 *
 * The SDK already contains a complete S-Channel tracer -- soc_schan_dump()
 * (src/soc/common/schan.c:408) is called from reg.c, mem.c and all three CMIC
 * drivers behind a plain runtime `if (LOG_CHECK(BSL_LS_SOC_SCHAN|BSL_VERBOSE))`,
 * with no compile-time gate. Nothing needed to be added to the SDK; it only had
 * to be switched on.
 *
 * That matters because it is the one thing the EOS capture cannot give us. EOS
 * yields 245,761 anonymous writes -- no function names, no call structure, no
 * way to tell where one subsystem ends. This yields the same traffic on the
 * same chip WITH the issuing function and source line attached, which is what
 * turns "replay these opaque records" into "implement these named steps".
 *
 * Two hooks and no libdiag: bsl_init() takes an output hook and a check hook
 * directly, so we do not drag the diag shell in just to set a severity.
 */
static FILE   *trace_fp;
static long    trace_bytes, trace_cap, trace_dropped, trace_ops;
static long    trace_bt_max;      /* SDKPOC_SCHAN_BT: ops to backtrace */
static long    mem_calls, mem_entries;

static int trace_out(bsl_meta_t *meta, const char *fmt, va_list args)
{
    /* Anything that is not S-Channel chatter belongs on the console, so
     * soc_attach's own errors are not swallowed by the trace file. */
    if (meta == NULL || meta->source != bslSourceSchan) {
        return vprintf(fmt, args);
    }
    if (trace_fp == NULL) {
        return 0;
    }
    /* A capped trace that lies about being complete is worse than no trace;
     * count what is dropped and report it. */
    if (trace_cap > 0 && trace_bytes >= trace_cap) {
        trace_dropped++;
        return 0;
    }
    /* Attribution is the whole point -- the function and line that issued the
     * op are what the EOS capture can never supply. */
    trace_bytes += fprintf(trace_fp, "%s:%d:%s|",
                           meta->file ? meta->file : "?", meta->line,
                           meta->func ? meta->func : "?");
    trace_bytes += vfprintf(trace_fp, fmt, args);
    return 0;
}

/* soc_attach's `error:` label returns rv, which is INITIALISED to
 * SOC_E_MEMORY (drv.c). So "rv=-2 Out of memory" is simply the default for any
 * `goto error` that never set rv -- it is not a memory failure, and 0 salloc
 * calls proves it. Finding WHICH branch bailed needs the SDK's own log lines,
 * and the interesting ones are at INFO/VERBOSE. SDKPOC_LOG_LEVEL raises the
 * non-S-Channel threshold (5 = verbose); default stays warn-and-worse. */
static int log_level = bslSeverityWarn;

/*
 * The BSL S-Channel verbose stream is the reason the port-layer capture came
 * back truncated: 274,784 ops were recorded and 1,748,658 messages were DROPPED
 * at a 256 MB cap (PORT-LAYER-CAPTURED-20260812.md).
 *
 * It is also redundant. Attribution -- the whole point of these traces -- comes
 * from sdkpoc_schan_hook's return-address chain resolved by addr2line, not from
 * BSL. BSL's own meta reports `schan.c:429:soc_schan_dump` on every single line
 * and names no caller (see the comment on the hook). So the stream that fills
 * the file contributes nothing the analysis reads.
 *
 * Off by default now. SDKPOC_BSL_SCHAN=1 restores it if the raw SDK text is
 * ever wanted. This is a size decision, NOT a filter on the ops themselves --
 * sdkpoc_schan_hook still records every op, unconditionally. Given this
 * project's history of filtering away the answer, that distinction is the
 * whole justification: nothing an op could tell us is being dropped.
 */
static int bsl_schan;

static int trace_check(bsl_packed_meta_t chk)
{
    if (BSL_LAYER_GET(chk) == bslLayerSoc &&
        BSL_SOURCE_GET(chk) == bslSourceSchan) {
        return bsl_schan ? (BSL_SEVERITY_GET(chk) <= bslSeverityVerbose) : 0;
    }
    /* The shell's own output is not logging, it is the answer to the command
     * the caller typed. Raising log_level to Info to let it through raised
     * every other Info line with it -- one `ps` produced 38,374 lines of SDK
     * chatter around 80 lines of port status. So let bslSourceShell through on
     * its own account and leave the threshold at warn-and-worse. bslSourceEcho
     * comes with it: that is sal_readline echoing the line it just read, and
     * without it a piped session gives answers with no way to tell which
     * command produced which.
     *
     * bslSourceRx joins them for the same reason: rxmon's callback reports
     * every received packet through LOG_INFO(BSL_LS_APPL_RX), so with the
     * threshold at warn the shell says "Active bitmap for RX is 2" and then
     * prints nothing per packet -- which reads as "CPU RX does not work". It
     * is the same trap cli_out was in. */
    if (BSL_LAYER_GET(chk) == bslLayerAppl &&
        (BSL_SOURCE_GET(chk) == bslSourceShell ||
         BSL_SOURCE_GET(chk) == bslSourceEcho ||
         BSL_SOURCE_GET(chk) == bslSourceRx)) {
        return 1;
    }
    return BSL_SEVERITY_GET(chk) <= log_level;
}

/*
 * Install the BSL hooks. Kept SEPARATE from the S-Channel trace, because it
 * was not: bsl_init() used to be reachable only through trace_start(), which
 * ran only when SDKPOC_SCHAN_TRACE was set -- and returned early, before
 * bsl_init(), if the trace file would not open. So an untraced run had NO
 * output hook at all, and every diag-shell command printed nothing.
 *
 * The shell's cli_out is `#define cli_out bsl_printf` at shared/bsl.h:76, and
 * it logs at BSL_LSS_CLI = BSL_L_APPL | BSL_S_SHELL | BSL_INFO. BSL_INFO is
 * severity 4 (bslenum.h) and log_level defaults to bslSeverityWarn = 3, so
 * trace_check's `severity <= log_level` dropped shell output even once the
 * hook existed. Hence bsl_want_cli.
 */
static int bsl_started;

static void bsl_start(int want_cli)
{
    bsl_config_t cfg;
    const char *lvl = getenv("SDKPOC_LOG_LEVEL");

    (void)want_cli;   /* shell output now passes trace_check by source */
    if (lvl != NULL) {
        log_level = atoi(lvl);
    }
    if (bsl_started) {
        return;
    }
    bsl_config_t_init(&cfg);
    cfg.out_hook = trace_out;
    cfg.check_hook = trace_check;
    bsl_init(&cfg);
    bsl_started = 1;
    printf("bsl: hooks installed, non-schan log level %d\n", log_level);
}

static void trace_start(const char *path)
{
    const char *cap = getenv("SDKPOC_TRACE_CAP");
    const char *bt = getenv("SDKPOC_SCHAN_BT");

    trace_bt_max = bt ? atol(bt) : 200;
    trace_cap = cap ? atol(cap) : (64L * 1024 * 1024);
    bsl_schan = getenv("SDKPOC_BSL_SCHAN") != NULL;
    printf("trace: BSL schan stream %s (attribution comes from the hook)\n",
           bsl_schan ? "ON" : "off");
    bsl_start(0);
    trace_fp = fopen(path, "w");
    if (trace_fp == NULL) {
        printf("trace: cannot open %s -- continuing untraced "
               "(BSL hooks are installed regardless)\n", path);
        return;
    }
    printf("trace: S-Channel -> %s (cap %ld bytes)\n", path, trace_cap);
    /* Ask the SDK's own gate whether S-Channel verbose is open, exactly as
     * soc_schan_dump's call sites do. The 04:52 run completed soc_attach with
     * a 0-byte trace and zero HDR[ lines anywhere, so rather than reason about
     * why, make the binary answer it. */
    {
        bsl_packed_meta_t chk = BSL_LS_SOC_SCHAN | BSL_VERBOSE;
        printf("trace: gate chk=0x%08x layer=%d source=%d severity=%d "
               "LOG_CHECK=%d (want layer %d source %d)\n",
               (unsigned)chk, BSL_LAYER_GET(chk), BSL_SOURCE_GET(chk),
               BSL_SEVERITY_GET(chk), LOG_CHECK(chk),
               bslLayerSoc, bslSourceSchan);
    }
}

/*
 * S-Channel trace hook, called from our local patch at schan.c soc_schan_op --
 * the single dispatch point every operation passes through.
 *
 * The SDK's own soc_schan_dump() cannot do this job: its cmicm call site sits
 * after an early `break` taken whenever a successful reply opcode differs from
 * the request. Proven on hardware -- LOG_CHECK returned 1, the gate was open,
 * and a fully successful soc_attach still emitted zero HDR[ lines.
 *
 * Emitted in the same field order as our replay blobs (opc blk acc dlen addr
 * data...) so a trace can be diffed directly against the EOS capture.
 */
void sdkpoc_schan_hook(int unit, schan_msg_t *msg, int dwc_write,
                       int dwc_read, uint32 flags, int rv)
{
    int words, i;

    COMPILER_REFERENCE(unit);
    COMPILER_REFERENCE(flags);
    if (trace_fp == NULL || msg == NULL) {
        return;
    }
    if (trace_cap > 0 && trace_bytes >= trace_cap) {
        trace_dropped++;
        return;
    }
    words = dwc_write > dwc_read ? dwc_write : dwc_read;
    if (words > 24) {
        words = 24;
    }
    trace_bytes += fprintf(trace_fp, "op %2u blk %3u acc %u dlen %4u rv %d",
                           (unsigned)msg->header.v4.opcode,
                           (unsigned)msg->header.v4.dst_blk,
                           (unsigned)msg->header.v4.acc_type,
                           (unsigned)msg->header.v4.data_byte_len, rv);
    for (i = 1; i < words; i++) {
        trace_bytes += fprintf(trace_fp, " %08x", msg->dwords[i]);
    }
    trace_bytes += fprintf(trace_fp, "\n");
    /* Real attribution. BSL's meta is useless for this -- it reports the
     * LOG_VERBOSE call site, which is schan.c:429:soc_schan_dump on every
     * line, never the reg.c/mem.c function that issued the op. The return
     * address chain is the only thing that names the actual caller.
     * Bounded: the first SDKPOC_SCHAN_BT ops only, because backtrace() per op
     * over a full init would dominate the run. */
    if (trace_ops < trace_bt_max) {
        void *bt[8];
        int k = backtrace(bt, 8), i;

        trace_bytes += fprintf(trace_fp, "  bt");
        for (i = 2; i < k; i++) {
            trace_bytes += fprintf(trace_fp, " %llx",
                                   (unsigned long long)(uintptr_t)bt[i]);
        }
        trace_bytes += fprintf(trace_fp, "\n");
    }
    trace_ops++;
}

/*
 * Bulk table-write hook, from our patch at soc_mem_write_range (mem.c:17499).
 *
 * The S-Channel hook saw only 172 ops for a whole soc_reset_init, against
 * EOS's 245,761 writes. Bulk table init can be carried by SBUS DMA, which
 * never passes through soc_schan_op, so this measures whether the rest of the
 * init is here rather than assuming it.
 */
void sdkpoc_mem_hook(int unit, int mem, int copyno, int index_min,
                     int index_max, int rv)
{
    COMPILER_REFERENCE(unit);
    COMPILER_REFERENCE(copyno);
    mem_calls++;
    if (index_max >= index_min) {
        mem_entries += (long)(index_max - index_min + 1);
    }
    if (trace_fp != NULL && (trace_cap <= 0 || trace_bytes < trace_cap)) {
        trace_bytes += fprintf(trace_fp, "mem %d range %d..%d rv %d\n",
                               mem, index_min, index_max, rv);
    }
}

/* Called from bde_shim's SIGSEGV handler. Not async-signal-safe, but the
 * process is dying anyway and the buffered trace is the evidence. */
void sdkpoc_trace_flush(void)
{
    if (trace_fp != NULL) {
        fflush(trace_fp);
    }
}

static void trace_finish(void)
{
    /* The MMIO trace is independent of the S-Channel one and must be written
     * on both exit paths, including the failure path at step 4 -- so it hangs
     * here rather than at the end of main, and it comes BEFORE the early
     * return that an unset SDKPOC_SCHAN_TRACE takes. */
    bde_shim_mmio_stats();
    bde_shim_mmio_flush();
    bde_shim_sbd_stats();
    bde_shim_sbd_flush();

    if (trace_fp == NULL) {
        return;
    }
    fflush(trace_fp);
    fclose(trace_fp);
    printf("trace: %ld schan ops, %ld mem_write_range calls covering %ld "
           "entries, %ld bytes written",
           trace_ops, mem_calls, mem_entries, trace_bytes);
    if (trace_dropped) {
        printf(", %ld messages DROPPED at the cap -- trace is INCOMPLETE",
               trace_dropped);
    }
    printf("\n");
}

#define STEP(fmt, ...) printf("\n=== " fmt " ===\n", ##__VA_ARGS__); fflush(stdout)

static void report(const char *what, int rv)
{
    printf("%-28s rv=%d %s\n", what, rv, rv == 0 ? "OK" : soc_errmsg(rv));
    fflush(stdout);
}

/* libdiag.a's sysconf.c is the SDK's own device-probe path and references the
 * standard BDE (`bde`, `bde_create`) from liblubde.a -- which this build
 * deliberately filters out, because sdkpoc attaches through its own user-space
 * shim on the CM vectors instead. sysconf_probe is never called here: the
 * attach has already happened by the time the shell starts. These stubs exist
 * only to satisfy the linker, and say so loudly if anything ever calls them.
 */
ibde_t *bde = 0;   /* ibde.h is already in scope via soc/drv.h */
int bde_create(void);
int bde_create(void)
{
    fprintf(stderr, "bde_create: sdkpoc attaches through its own shim; the "
                    "SDK's probe path is not wired up. Refusing.\n");
    return -1;
}

/* The diag shell's `version` command prints a build stamp that the SDK's own
 * build system generates into a version.c we do not use. Declared in
 * appl/diag/system.h; supplied here so the banner says where this binary
 * actually came from rather than pretending to be a Broadcom release. */
char *_build_release   = "sdkpoc/6.5.24";
char *_build_host      = "edgenos-builder";
char *_build_user      = "edgenos";
char *_build_date      = __DATE__ " " __TIME__;
char *_build_datestamp = __DATE__;
char *_build_tree      = "OpenBCM/sdk-6.5.24 + sdkshim";

/* appl/diag/opennsa_diag.h drags in more than this file wants; these two
 * prototypes are all we need and they are stable across the SDK. */
extern int sh_process(int unit, const char *pfx, int eof);
extern int diag_init(void);   /* appl/diag/system.c:476 -- registers the command table */
extern void command_mode_set(int unit, int mode);  /* appl/diag/diag.h:95 */
extern void cmdlist_init(void);                    /* appl/diag/cmdlist.h:27 */
extern int soc_ndev;
extern int sh_process_command(int unit, char *c);

/* ---- the copper PHYs: SCD MDIO as the SDK's PHY bus --------------------
 *
 * The 48 10GBASE-T ports use BCM84848s that are NOT on the ASIC's MDIO bus --
 * they hang off the SCD's three MDIO accelerators (docs/PHY-BCM84848.md), so
 * bcm_port_probe binds every port to the internal SerDes and the copper ports
 * stay down. tools/scdmdio.c proved all 48 answer over that path from user
 * space (0x600d/0x84f9 on every one).
 *
 * Rather than drive them ourselves, hand the path to the SDK. phyctrl.c:529:
 *
 *     if (soc_property_port_get(unit, port, spn_PHY_BUS_I2C, 0)) {
 *         ext_pc.read  = phy_i2c_miireg_read;
 *         ext_pc.write = phy_i2c_miireg_write;
 *     }
 *
 * so a port with phy_bus_i2c_<port>=1 has its EXTERNAL phy access routed to
 * phy_i2c_miireg_read/write, which call whatever we install with
 * phy_i2c_bus_func_hook_set (phyi2c.c:123). The property is named for I2C but
 * the hook is just "application-provided PHY bus" -- nothing about it is I2C
 * specific. INCLUDE_I2C is already in our build defines.
 *
 * The win is that phy_8481drv_xe -- Broadcom's own BCM84848 driver, compiled
 * in under INCLUDE_PHY_8481, with the bcm_84844_firmware blob -- then does the
 * bring-up, instead of us reimplementing a 10GBASE-T PHY driver.
 *
 * Address decoding. The SDK passes the phy_id from port_phy_addr_<port>, which
 * on this board encodes the accelerator, bus and address
 * (docs/PHY-BCM84848.md). Verified against config.bcm: port 1 -> 0x001,
 * port 9 -> 0x021, port 48 -> 0x126, all exact.
 *
 *     accel = ((id >> 8) << 1) | ((id >> 6) & 1)
 *     bus   =  (id >> 5) & 1
 *     prtad =   id & 0x1f
 *
 * Clause 45 packs the register as (devad & 0x3F) << 16 | regad
 * (shared/phyreg.h:37).
 */
#define SCD_BAR_PHYS      0xf6000000UL
#define SCD_MAP_LEN       0x00100000UL
#define MDIO_REQ_LO       0x00
#define MDIO_REQ_HI       0x10
#define MDIO_CS           0x20
#define MDIO_RESP         0x30
#define MDIO_OP_SET       0
#define MDIO_OP_WRITE     1
#define MDIO_OP_READ      3

static volatile uint32_t *scd_bar;
static unsigned           scd_mdio_speed = 10;
static unsigned           scd_req_id;
static long               phybus_rd_calls, phybus_wr_calls, phybus_errs;

static const unsigned long scd_accel_base[3] = { 0xB000, 0xB100, 0xB200 };

static uint32_t scd_rd(unsigned long off)
{
    return scd_bar[off / 4];
}

static void scd_wr(unsigned long off, uint32_t v)
{
    scd_bar[off / 4] = v;
}

/* ⚠ Instrumentation, because "bcm_init stalls with the PHY bus" was diagnosed
 * three times from an absence -- a frozen log and a busy process -- and never
 * from a number. Each failing transaction costs ~1 s of exponential backoff and
 * there are TWO per PHY register access, so a small failure rate is
 * indistinguishable from a hang unless it is counted. */
static unsigned long scd_mdio_xacts, scd_mdio_fails, scd_mdio_usec;
static unsigned long scd_mdio_slowest;

static void scd_mdio_note(unsigned long t_start, int failed)
{
    unsigned long took = sal_time_usecs() - t_start;

    scd_mdio_xacts++;
    scd_mdio_usec += took;
    if (failed) scd_mdio_fails++;
    if (took > scd_mdio_slowest) scd_mdio_slowest = took;

    /* Report often enough that a stalled-looking run shows whether it is
     * progressing, and rarely enough not to drown the log. */
    if ((scd_mdio_xacts % 2000) == 0) {
        printf("  MDIO: %lu xacts, %lu failed (%lu%%), %lu ms total, "
               "slowest %lu us\n",
               scd_mdio_xacts, scd_mdio_fails,
               scd_mdio_fails * 100 / scd_mdio_xacts,
               scd_mdio_usec / 1000, scd_mdio_slowest);
        fflush(stdout);
    }
}

static int scd_mdio_xact(int accel, int bus, int prtad, int devad, int op,
                         uint16_t data)
{
    unsigned long t_start = sal_time_usecs();
    unsigned long base = scd_accel_base[accel];
    uint32_t cs_def = (uint32_t)(scd_mdio_speed & 3) << 26;
    unsigned long delay = 1;
    uint32_t lo, resp;

    scd_wr(base + MDIO_CS, cs_def | (1u << 30));      /* reset interrupt */

    lo  =  (uint32_t)data;
    lo |= ((uint32_t)(devad & 0x1f)) << 16;
    lo |= ((uint32_t)(prtad & 0x1f)) << 21;
    lo |= ((uint32_t)(op    & 0x03)) << 26;
    lo |= 1u << 28;                                    /* t = clause 45 */
    lo |= ((uint32_t)(bus   & 0x07)) << 29;
    scd_wr(base + MDIO_REQ_LO, lo);
    scd_wr(base + MDIO_REQ_HI, ((uint32_t)(scd_req_id++ & 0xff)) << 16);

    while (delay < 1000000UL) {
        uint32_t cs = scd_rd(base + MDIO_CS);
        unsigned res = cs & 0x3ff;
        if (res == 1) {
            break;
        }
        if (res != 0) {
            scd_mdio_note(t_start, 1);
            return -1;
        }
        sal_usleep(delay);
        delay *= 2;
    }
    if (delay >= 1000000UL) {
        scd_mdio_note(t_start, 1);
        return -1;
    }
    scd_wr(base + MDIO_CS, cs_def | (1u << 30));
    resp = scd_rd(base + MDIO_RESP);
    if (((resp >> 31) & 1) != 1 || ((resp >> 30) & 1) != 0) {
        scd_mdio_note(t_start, 1);
        return -1;
    }
    scd_mdio_note(t_start, 0);
    return (int)(resp & 0xffff);
}

/* Two transactions per access: SET the register number, then READ/WRITE. */
static int scd_phy_access(uint32 phy_id, uint32 reg, int is_write,
                          uint16_t wval, uint16 *rval)
{
    int accel = (int)(((phy_id >> 8) << 1) | ((phy_id >> 6) & 1));
    int bus   = (int)((phy_id >> 5) & 1);
    int prtad = (int)(phy_id & 0x1f);
    int devad = (int)((reg >> 16) & 0x3f);
    int regad = (int)(reg & 0xffff);
    int rv;

    if (scd_bar == NULL || accel < 0 || accel > 2) {
        return SOC_E_PARAM;
    }
    rv = scd_mdio_xact(accel, bus, prtad, devad, MDIO_OP_SET,
                       (uint16_t)regad);
    if (rv < 0) {
        phybus_errs++;
        return SOC_E_FAIL;
    }
    rv = scd_mdio_xact(accel, bus, prtad, devad,
                       is_write ? MDIO_OP_WRITE : MDIO_OP_READ, wval);
    if (rv < 0) {
        phybus_errs++;
        return SOC_E_FAIL;
    }
    if (!is_write && rval != NULL) {
        *rval = (uint16)rv;
    }
    return SOC_E_NONE;
}

static int sdkpoc_phy_bus_rd(int unit, uint32 phy_id, uint32 reg, uint16 *data)
{
    COMPILER_REFERENCE(unit);
    phybus_rd_calls++;
    return scd_phy_access(phy_id, reg, 0, 0, data);
}

static int sdkpoc_phy_bus_wr(int unit, uint32 phy_id, uint32 reg, uint16 data)
{
    COMPILER_REFERENCE(unit);
    phybus_wr_calls++;
    return scd_phy_access(phy_id, reg, 1, data, NULL);
}

static int scd_phy_bus_init(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);

    if (fd < 0) {
        printf("  ** cannot open /dev/mem for the SCD BAR\n");
        return -1;
    }
    scd_bar = mmap(NULL, SCD_MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, SCD_BAR_PHYS);
    if (scd_bar == MAP_FAILED) {
        scd_bar = NULL;
        printf("  ** cannot mmap the SCD BAR at 0x%lx\n", SCD_BAR_PHYS);
        return -1;
    }
    return 0;
}

/* ---- FIB sync: mirror the kernel routing table into the chip -----------
 *
 * OSPF converges and 33 routes land in the kernel FIB, and the ASIC forwards
 * none of them -- `l3 defip show` is empty and every packet crossing the box
 * goes up to the CPU for Linux to route. That is a control plane with a
 * software datapath, which is not what the hardware is for.
 *
 * The EdgeNOS project already has an agent for this (`edged`), but its l3.c
 * does not use the OpenBCM SDK -- its own comments say "when OpenBCM SDK is
 * integrated, this calls bcm_l3_route_add()". The netlink half is the reusable
 * idea; the chip half is written here against the SDK we actually have.
 *
 * Model, which is the standard Broadcom one:
 *
 *   neighbour (ARP)  -> bcm_l3_egress_create()  next-hop MAC + port + vlan
 *   route            -> bcm_l3_route_add()      subnet/mask -> that egress
 *
 * A route cannot be programmed before its gateway resolves, because the egress
 * object needs the next-hop MAC. Routes whose gateway is not yet known are
 * therefore kept pending and programmed when the neighbour appears -- which is
 * the normal order of events, since OSPF installs routes as soon as it
 * converges but ARP may not have run yet.
 *
 *   SDKPOC_FIBSYNC=1
 */
/* One entry per routed port. This was a single set of globals -- the last
 * routed port simply overwrote the previous -- which worked while only one port
 * carried an adjacency. With OSPF on two ports it silently dropped every route
 * that pointed out the other one: 33 routes in the kernel, 5 in the chip. */
struct fib_intf {
    bcm_if_t   l3_intf;
    bcm_vlan_t vlan;
    bcm_port_t port;
    int        ifindex;             /* Linux ifindex of this port's tap */
};

#define FIB_MAX_INTF 16
static struct fib_intf fib_intfs[FIB_MAX_INTF];
static int             fib_nintf;

static struct fib_intf *fib_intf_by_ifindex(int ifindex)
{
    int i;
    for (i = 0; i < fib_nintf; i++) {
        if (fib_intfs[i].ifindex == ifindex) {
            return &fib_intfs[i];
        }
    }
    return NULL;
}

#define FIB_MAX_NH     64
#define FIB_MAX_ROUTES 512

struct fib_nh {
    uint32_t   gw;              /* network order */
    bcm_if_t   egress;          /* chip egress object */
    int        valid;
};

struct fib_rt {
    uint32_t   dst;             /* network order */
    int        len;
    uint32_t   gw;
    int        programmed;
    int        valid;
};

static struct fib_nh fib_nhs[FIB_MAX_NH];
static struct fib_rt fib_rts[FIB_MAX_ROUTES];
static int  fib_unit;
static long fib_routes_added, fib_nh_added, fib_errors, fib_pending;
/* Resolved once at start. Addresses on this interface are terminated in the
 * chip even though it is not a front port -- see fib_handle(). */
static int  fib_lo_ifindex;
static long fib_routes_deleted;   /* withdrawals actually removed from the chip */
static long fib_hosts_added;
static pthread_t fib_thread;

static struct fib_nh *fib_nh_find(uint32_t gw)
{
    int i;
    for (i = 0; i < FIB_MAX_NH; i++) {
        if (fib_nhs[i].valid && fib_nhs[i].gw == gw) {
            return &fib_nhs[i];
        }
    }
    return NULL;
}

/* Build the chip egress object for a resolved neighbour. */
static struct fib_nh *fib_nh_add(uint32_t gw, const uint8 *mac,
                                 struct fib_intf *fi)
{
    struct fib_nh *nh = fib_nh_find(gw);
    bcm_l3_egress_t eg;
    bcm_if_t eid = 0;
    int i, prv;

    if (nh != NULL) {
        return nh;              /* already have it */
    }
    for (i = 0; i < FIB_MAX_NH; i++) {
        if (!fib_nhs[i].valid) break;
    }
    if (i == FIB_MAX_NH) {
        return NULL;
    }

    bcm_l3_egress_t_init(&eg);
    sal_memcpy(eg.mac_addr, mac, 6);
    eg.intf   = fi->l3_intf;        /* the routed interface it was seen on */
    eg.vlan   = fi->vlan;
    eg.port   = fi->port;
    eg.module = 0;
    prv = bcm_l3_egress_create(fib_unit, 0, &eg, &eid);
    if (prv != BCM_E_NONE) {
        fib_errors++;
        /* Report it. "2 errors" with no detail cost a whole run: the counter
         * says something failed but not what, and this is the one call that
         * everything downstream depends on. */
        if (fib_errors <= 5) {
            printf("  FIB: egress_create for %u.%u.%u.%u rv=%d %s "
                   "(intf %d vlan %d port %d)\n",
                   (gw) & 0xff, (gw >> 8) & 0xff, (gw >> 16) & 0xff,
                   (gw >> 24) & 0xff, prv, bcm_errmsg(prv),
                   fi->l3_intf, fi->vlan, fi->port);
            fflush(stdout);
        }
        return NULL;
    }
    /* A route sync needs HOSTS as well as routes. Connected destinations --
     * the next hop itself, and anything else on a directly attached subnet --
     * are not covered by any defip entry, so without a /32 host entry the chip
     * misses on them and punts to the CPU. Traffic still flows, in software,
     * which looks like success until you check where it went. */
    {
        bcm_l3_host_t host;
        int hrv;

        bcm_l3_host_t_init(&host);
        host.l3a_ip_addr = ntohl(gw);
        host.l3a_intf    = eid;
        hrv = bcm_l3_host_add(fib_unit, &host);
        if (hrv != BCM_E_NONE && hrv != BCM_E_EXISTS) {
            printf("  FIB: l3_host_add %u.%u.%u.%u rv=%d %s\n",
                   (gw) & 0xff, (gw >> 8) & 0xff, (gw >> 16) & 0xff,
                   (gw >> 24) & 0xff, hrv, bcm_errmsg(hrv));
        } else {
            fib_hosts_added++;
        }
    }
    if (fib_nh_added < 3) {
        printf("  FIB: next hop %u.%u.%u.%u -> egress %d (host entry added)\n",
               (gw) & 0xff, (gw >> 8) & 0xff, (gw >> 16) & 0xff,
               (gw >> 24) & 0xff, eid);
        fflush(stdout);
    }
    fib_nhs[i].gw     = gw;
    fib_nhs[i].egress = eid;
    fib_nhs[i].valid  = 1;
    fib_nh_added++;
    return &fib_nhs[i];
}

/* Withdraw a route from the chip.
 *
 * ⚠ This did not exist until 2026-08-19, so the FIB sync was ADD-ONLY: anything
 * the kernel withdrew stayed programmed in hardware for ever. Demonstrated by
 * deleting a test prefix -- the kernel had 0 matching routes and `l3 defip show`
 * still listed 192.0.2.0/24 pointing at its old egress. The consequence is worse
 * than a leak: traffic for a withdrawn prefix keeps being forwarded to a peer
 * that no longer has a route for it, instead of being dropped.
 */
static void fib_route_forget(uint32_t dst, int len)
{
    bcm_l3_route_t rt;
    int i;

    for (i = 0; i < FIB_MAX_ROUTES; i++) {
        if (!fib_rts[i].valid || fib_rts[i].dst != dst || fib_rts[i].len != len) {
            continue;
        }
        if (fib_rts[i].programmed) {
            bcm_l3_route_t_init(&rt);
            rt.l3a_subnet  = ntohl(dst);
            rt.l3a_ip_mask = len ? (0xffffffffu << (32 - len)) : 0;
            if (bcm_l3_route_delete(fib_unit, &rt) != BCM_E_NONE) {
                fib_errors++;
            } else {
                fib_routes_deleted++;
            }
        }
        fib_rts[i].valid = 0;
        return;
    }
}

static int fib_route_program(struct fib_rt *r)
{
    struct fib_nh *nh = fib_nh_find(r->gw);
    bcm_l3_route_t rt;
    int prv;

    if (nh == NULL) {
        return -1;                          /* gateway not resolved yet */
    }
    bcm_l3_route_t_init(&rt);
    rt.l3a_subnet  = ntohl(r->dst);
    rt.l3a_ip_mask = r->len ? (0xffffffffu << (32 - r->len)) : 0;
    rt.l3a_intf    = nh->egress;
    /* ⚠ BCM_L3_REPLACE is what makes a next-hop change actually take. Without
     * it bcm_l3_route_add() on an existing prefix returns BCM_E_EXISTS, which
     * the check below treats as success -- so the counter increments, no error
     * is raised, and the chip keeps the OLD egress. Tolerating E_EXISTS without
     * asking for a replace is how a stale route looks like a programmed one. */
    rt.l3a_flags  |= BCM_L3_REPLACE;
    prv = bcm_l3_route_add(fib_unit, &rt);
    if (prv != BCM_E_NONE && prv != BCM_E_EXISTS) {
        fib_errors++;
        return -1;
    }
    r->programmed = 1;
    fib_routes_added++;
    return 0;
}

static void fib_route_note(uint32_t dst, int len, uint32_t gw)
{
    int i, free_slot = -1;

    for (i = 0; i < FIB_MAX_ROUTES; i++) {
        if (fib_rts[i].valid && fib_rts[i].dst == dst &&
            fib_rts[i].len == len) {
            /* ⚠ The key is dst+len, but a route is (dst, len, GATEWAY). This
             * used to `return` here unconditionally as "already known", which
             * silently dropped every next-hop CHANGE: the kernel moves a prefix
             * to a different peer, we ignore it, and the chip keeps forwarding
             * out the old port.
             *
             * Found 2026-08-19 when the copper OSPF adjacency finally came up
             * and Linux moved 29 of 35 prefixes onto xe47 -- the ASIC still had
             * all 33 pointing at the 40G egress, so the hardware was actively
             * forwarding those out the wrong port while every counter read
             * "0 errors". */
            if (fib_rts[i].gw == gw) {
                return;                     /* genuinely unchanged */
            }
            fib_rts[i].gw         = gw;
            fib_rts[i].programmed = 0;      /* reprogram with the new next hop */
            return;
        }
        if (!fib_rts[i].valid && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return;
    }
    fib_rts[free_slot].dst        = dst;
    fib_rts[free_slot].len        = len;
    fib_rts[free_slot].gw         = gw;
    fib_rts[free_slot].programmed = 0;
    fib_rts[free_slot].valid      = 1;
    if (fib_route_program(&fib_rts[free_slot]) < 0) {
        fib_pending++;
    }
}

/* A neighbour appearing may unblock routes noted earlier. */
static void fib_flush_pending(void)
{
    int i;
    for (i = 0; i < FIB_MAX_ROUTES; i++) {
        if (fib_rts[i].valid && !fib_rts[i].programmed) {
            if (fib_route_program(&fib_rts[i]) == 0 && fib_pending > 0) {
                fib_pending--;
            }
        }
    }
}

/* Terminate our own addresses in the chip.
 *
 * ⚠ Needed the moment the my-station MAC is marked BCM_L2_L3LOOKUP. Before
 * that, everything addressed to our router MAC was bridged to the CPU and
 * local traffic just worked. With L3LOOKUP the chip ROUTES it instead -- so
 * unicast sent to our own interface address is looked up in the FIB and does
 * not reach software at all.
 *
 * The symptom was OSPF stuck in ExStart on both neighbours, retransmitting DD
 * forever: Hellos are multicast to 224.0.0.5 and still flooded to the CPU, but
 * Database Description packets are UNICAST to our own address and silently
 * vanished. tcpdump on the far end showed our DD going out with the correct
 * MTU and the neighbour answering -- the answers just never arrived.
 *
 * A host entry per local address, pointing at an egress object that delivers
 * to the CPU unrouted, puts local termination back. */
static bcm_if_t fib_cpu_egress;

static int fib_cpu_egress_get(void)
{
    bcm_l3_egress_t eg;
    bcm_if_t eid = 0;
    int prv;

    if (fib_cpu_egress != 0) {
        return 0;
    }
    if (fib_nintf == 0) {
        return -1;
    }
    bcm_l3_egress_t_init(&eg);
    eg.flags  = BCM_L3_L2TOCPU;         /* deliver to CPU, do not route */
    eg.intf   = fib_intfs[0].l3_intf;
    eg.vlan   = fib_intfs[0].vlan;
    eg.port   = CMIC_PORT(0);
    eg.module = 0;
    prv = bcm_l3_egress_create(fib_unit, 0, &eg, &eid);
    if (prv != BCM_E_NONE) {
        printf("  FIB: cpu egress create rv=%d %s\n", prv, bcm_errmsg(prv));
        return -1;
    }
    fib_cpu_egress = eid;
    printf("  FIB: CPU egress object %d (local termination)\n", eid);
    fflush(stdout);
    return 0;
}

static void fib_local_addr_add(uint32_t addr)
{
    bcm_l3_host_t host;
    int prv;

    if (fib_cpu_egress_get() != 0) {
        return;
    }
    bcm_l3_host_t_init(&host);
    host.l3a_ip_addr = ntohl(addr);
    host.l3a_intf    = fib_cpu_egress;
    prv = bcm_l3_host_add(fib_unit, &host);
    if (prv != BCM_E_NONE && prv != BCM_E_EXISTS) {
        printf("  FIB: local %u.%u.%u.%u rv=%d %s\n",
               (addr) & 0xff, (addr >> 8) & 0xff, (addr >> 16) & 0xff,
               (addr >> 24) & 0xff, prv, bcm_errmsg(prv));
    } else {
        printf("  FIB: local address %u.%u.%u.%u -> CPU\n",
               (addr) & 0xff, (addr >> 8) & 0xff, (addr >> 16) & 0xff,
               (addr >> 24) & 0xff);
        fflush(stdout);
    }
}

static void fib_handle(struct nlmsghdr *nh)
{
    if (nh->nlmsg_type == RTM_NEWADDR) {
        struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nh);
        struct rtattr *rta = (struct rtattr *)((char *)ifa + NLMSG_ALIGN(sizeof *ifa));
        int len = nh->nlmsg_len - NLMSG_LENGTH(sizeof *ifa);

        /* ⚠ This used to accept an address ONLY if it sat on a routed front
         * port, which silently excluded the LOOPBACK -- and a loopback is
         * exactly the address you most want terminated in hardware, because it
         * is the router-id and the one address that survives a link flap.
         *
         * The symptom was subtle: the loopback pinged fine locally (that is the
         * kernel talking to itself) and OSPF advertised it correctly, so
         * neighbours installed a route to it -- and every packet they sent
         * arrived at the chip, found no host entry telling it to punt, and was
         * dropped in silicon. Reachable everywhere except from the network. */
        if (ifa->ifa_family != AF_INET) {
            return;
        }
        if (fib_intf_by_ifindex(ifa->ifa_index) == NULL &&
            (int)ifa->ifa_index != fib_lo_ifindex) {
            return;
        }
        for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
            if (rta->rta_type == IFA_LOCAL || rta->rta_type == IFA_ADDRESS) {
                uint32_t a = 0;
                sal_memcpy(&a, RTA_DATA(rta), 4);
                /* 127.0.0.0/8 is the kernel's own business, not the chip's. */
                if ((ntohl(a) >> 24) == 127) {
                    return;
                }
                fib_local_addr_add(a);
                return;
            }
        }
        return;
    }
    if (nh->nlmsg_type == RTM_NEWNEIGH) {
        struct ndmsg *nd = (struct ndmsg *)NLMSG_DATA(nh);
        struct rtattr *rta = (struct rtattr *)((char *)nd + NLMSG_ALIGN(sizeof *nd));
        int len = nh->nlmsg_len - NLMSG_LENGTH(sizeof *nd);
        uint32_t gw = 0;
        uint8 mac[6];
        int have_mac = 0;
        struct fib_intf *fi;

        if (nd->ndm_family != AF_INET) return;
        /* Only neighbours on a routed interface, and the egress object is
         * built against THAT interface. Without this the MANAGEMENT gateway
         * (10.1.1.1 on eth0) became a chip next hop pointing out a front
         * port -- a next hop to a host not reachable that way at all. */
        fi = fib_intf_by_ifindex(nd->ndm_ifindex);
        if (fi == NULL) {
            return;
        }
        for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
            if (rta->rta_type == NDA_DST) {
                sal_memcpy(&gw, RTA_DATA(rta), 4);
            } else if (rta->rta_type == NDA_LLADDR &&
                       RTA_PAYLOAD(rta) == 6) {
                sal_memcpy(mac, RTA_DATA(rta), 6);
                have_mac = 1;
            }
        }
        /* NUD_FAILED/INCOMPLETE carry no usable MAC. */
        if (gw && have_mac && (nd->ndm_state & (NUD_REACHABLE | NUD_STALE |
                                                NUD_PERMANENT | NUD_DELAY |
                                                NUD_PROBE))) {
            if (fib_nh_add(gw, mac, fi) != NULL) {
                fib_flush_pending();
            }
        }
    } else if (nh->nlmsg_type == RTM_NEWROUTE) {
        struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(nh);
        struct rtattr *rta = (struct rtattr *)((char *)rtm + NLMSG_ALIGN(sizeof *rtm));
        int len = nh->nlmsg_len - NLMSG_LENGTH(sizeof *rtm);
        uint32_t dst = 0, gw = 0;
        int oif = 0;

        if (rtm->rtm_family != AF_INET || rtm->rtm_type != RTN_UNICAST) {
            return;
        }
        for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
            if (rta->rta_type == RTA_DST) {
                sal_memcpy(&dst, RTA_DATA(rta), 4);
            } else if (rta->rta_type == RTA_GATEWAY) {
                sal_memcpy(&gw, RTA_DATA(rta), 4);
            } else if (rta->rta_type == RTA_OIF) {
                sal_memcpy(&oif, RTA_DATA(rta), 4);
            }
        }
        /* Routes leaving by any other interface are not ours to program --
         * the management default via eth0 above all. Any routed front port is
         * fair game, which is what makes two-uplink topologies work. */
        if (fib_intf_by_ifindex(oif) == NULL) {
            return;
        }
        /* Only gatewayed routes: a connected route needs no chip entry beyond
         * the L3 interface that already exists. */
        if (gw != 0) {
            fib_route_note(dst, rtm->rtm_dst_len, gw);
        }
    } else if (nh->nlmsg_type == RTM_DELROUTE) {
        struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(nh);
        struct rtattr *rta = (struct rtattr *)((char *)rtm + NLMSG_ALIGN(sizeof *rtm));
        int len = nh->nlmsg_len - NLMSG_LENGTH(sizeof *rtm);
        uint32_t dst = 0;
        int oif = 0;

        if (rtm->rtm_family != AF_INET || rtm->rtm_type != RTN_UNICAST) {
            return;
        }
        for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
            if (rta->rta_type == RTA_DST) {
                sal_memcpy(&dst, RTA_DATA(rta), 4);
            } else if (rta->rta_type == RTA_OIF) {
                sal_memcpy(&oif, RTA_DATA(rta), 4);
            }
        }
        /* Same interface filter as the add path, so a management route being
         * withdrawn cannot delete a chip entry we never made. */
        if (fib_intf_by_ifindex(oif) == NULL) {
            return;
        }
        fib_route_forget(dst, rtm->rtm_dst_len);
    }
}

static void *fib_reader(void *arg)
{
    int fd = *(int *)arg;
    char buf[16384];

    for (;;) {
        int n = recv(fd, buf, sizeof buf, 0);
        struct nlmsghdr *nh;

        if (n <= 0) {
            sal_sleep(1);
            continue;
        }
        for (nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, (unsigned)n);
             nh = NLMSG_NEXT(nh, n)) {
            if (nh->nlmsg_type == NLMSG_DONE) break;
            fib_handle(nh);
        }
    }
    return NULL;
}

/* Ask the kernel to replay what it already has, then keep listening. */
static int fib_dump(int fd, int type)
{
    struct {
        struct nlmsghdr nh;
        struct rtgenmsg g;
    } req;

    sal_memset(&req, 0, sizeof req);
    req.nh.nlmsg_len   = NLMSG_LENGTH(sizeof req.g);
    req.nh.nlmsg_type  = type;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.g.rtgen_family = AF_INET;
    return send(fd, &req, req.nh.nlmsg_len, 0);
}

static int fib_start(int unit)
{
    static int fd;
    struct sockaddr_nl sa;

    fib_unit = unit;

    /* The loopback. Its addresses are terminated in the chip like a routed
     * port's, because the router-id lives there and neighbours will route to
     * it. Resolved by name rather than assuming ifindex 1. */
    fib_lo_ifindex = (int)if_nametoindex("lo");
    printf("  loopback: ifindex %d (its addresses terminate in hardware)\n",
           fib_lo_ifindex);

    /* Resolve every routed port's tap ifindex so netlink events can be matched
     * to the interface they belong to. */
    {
        int i;
        for (i = 0; i < fib_nintf; i++) {
            char nm[16];
            sal_snprintf(nm, sizeof nm, "xe%d", fib_intfs[i].port - 1);
            fib_intfs[i].ifindex = (int)if_nametoindex(nm);
            printf("  routed %s: ifindex %d, l3 intf %d, vlan %d, port %d\n",
                   nm, fib_intfs[i].ifindex, fib_intfs[i].l3_intf,
                   fib_intfs[i].vlan, fib_intfs[i].port);
        }
    }
    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) {
        printf("  ** netlink socket failed\n");
        return -1;
    }
    sal_memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = RTMGRP_IPV4_ROUTE | RTMGRP_NEIGH | RTMGRP_IPV4_IFADDR;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        printf("  ** netlink bind failed\n");
        close(fd);
        return -1;
    }
    /* Local addresses first -- without them the chip routes traffic aimed at
     * us instead of delivering it, and the control plane never converges.
     * Then neighbours, so routes can be programmed as they arrive. */
    (void)fib_dump(fd, RTM_GETADDR);
    sal_usleep(200000);
    {
        char buf[16384];
        int n;
        struct nlmsghdr *p2;
        while ((n = recv(fd, buf, sizeof buf, MSG_DONTWAIT)) > 0) {
            for (p2 = (struct nlmsghdr *)buf; NLMSG_OK(p2, (unsigned)n);
                 p2 = NLMSG_NEXT(p2, n)) {
                fib_handle(p2);
            }
        }
    }
    (void)fib_dump(fd, RTM_GETNEIGH);
    sal_usleep(200000);
    {
        char buf[16384];
        int n;
        struct nlmsghdr *nh;
        while ((n = recv(fd, buf, sizeof buf, MSG_DONTWAIT)) > 0) {
            for (nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, (unsigned)n);
                 nh = NLMSG_NEXT(nh, n)) {
                fib_handle(nh);
            }
        }
    }
    (void)fib_dump(fd, RTM_GETROUTE);
    if (pthread_create(&fib_thread, NULL, fib_reader, &fd) != 0) {
        printf("  ** cannot start FIB sync thread\n");
        return -1;
    }
    return 0;
}

/* ---- tap interfaces: give Linux a netdev per ASIC port ----------------
 *
 * Everything so far has moved packets between the chip and *this program*.
 * To behave like EOS the box needs ordinary Linux interfaces -- something
 * `ip addr add` can address, ping can use and FRR can run OSPF on. EOS gets
 * that from its own kernel glue; the SDK's equivalent (KNET) needs a kernel
 * module, which is the dependency this user-space BDE exists to avoid.
 *
 * A tap device does the same job entirely from user space: the RX callback
 * writes each received frame into the tap, and a reader thread pulls frames
 * out and hands them to bcm_tx aimed at that port. Linux then sees a normal
 * NIC. CONFIG_TUN=y is already in our kernel.
 *
 *   SDKPOC_TAP=61        one tap for port 61, named xe60
 *   SDKPOC_TAP=48,61     several
 *   SDKPOC_TAP=1-61      a range -- every front-panel port on this board
 *   SDKPOC_TAP=1-48,61   ranges and singles mixed
 *
 * The interface is named for the SDK port (xe<port-1>) so it lines up with
 * `ps` and with EOS's numbering.
 */
/* One per front-panel port. This board presents 61: 48 RJ45 plus the four QSFP
 * cages, three broken out as 4x10G and one as 40G. It was 8, which silently
 * dropped everything after the eighth port. */
#define MAX_TAPS 64

struct tap_port {
    int   fd;
    int   port;
    char  name[16];
    pthread_t thread;
    long  rx_to_linux;
    long  tx_from_linux;
    long  tx_errors;
};

static struct tap_port taps[MAX_TAPS];
static int             ntaps;

static struct tap_port *tap_for_port(int port)
{
    int i;
    for (i = 0; i < ntaps; i++) {
        if (taps[i].port == port) {
            return &taps[i];
        }
    }
    return NULL;
}

static int tap_open(const char *name)
{
    struct ifreq ifr;
    int fd = open("/dev/net/tun", O_RDWR);

    if (fd < 0) {
        printf("  ** open /dev/net/tun failed -- is CONFIG_TUN present?\n");
        return -1;
    }
    sal_memset(&ifr, 0, sizeof ifr);
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;    /* raw ethernet frames, no header */
    sal_strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        printf("  ** TUNSETIFF %s failed\n", name);
        close(fd);
        return -1;
    }
    return fd;
}

/* Linux -> chip. One thread per tap: a blocking read is the whole loop. */
static void *tap_reader(void *arg)
{
    struct tap_port *t = (struct tap_port *)arg;
    unsigned char buf[2048];

    for (;;) {
        int n = read(t->fd, buf, sizeof buf);
        bcm_pkt_t *pkt = NULL;

        if (n <= 0) {
            continue;
        }
        /* Runts would be rejected by bcm_tx's own length check. */
        if (n < 60) {
            sal_memset(buf + n, 0, 60 - n);
            n = 60;
        }
        /* ⚠ BCM_PKT_F_NO_VTAG is essential. A frame from Linux carries no
         * 802.1Q tag, but the port is untagged in its VLAN, so without this
         * the SDK "removes the tag" on egress and eats four real bytes. The
         * frame then arrives at the peer with correct MACs and the ethertype
         * and the next two bytes missing -- an ARP request that tcpdump
         * reports as "ethertype IPv4, IP0 (invalid)", which looks like
         * corruption rather than a tag operation. Diagnosed by capturing the
         * bytes on the far end. */
        if (bcm_pkt_alloc(0, n, BCM_TX_CRC_APPEND, &pkt) != BCM_E_NONE) {
            t->tx_errors++;
            continue;
        }
        sal_memcpy(pkt->pkt_data[0].data, buf, n);
        pkt->pkt_len = n;
        pkt->pkt_data[0].len = n;
        pkt->flags |= BCM_PKT_F_NO_VTAG;
        BCM_PBMP_CLEAR(pkt->tx_pbmp);
        BCM_PBMP_PORT_ADD(pkt->tx_pbmp, t->port);
        BCM_PBMP_CLEAR(pkt->tx_upbmp);
        BCM_PBMP_PORT_ADD(pkt->tx_upbmp, t->port);

        if (bcm_tx(0, pkt, NULL) != BCM_E_NONE) {
            t->tx_errors++;
        } else {
            t->tx_from_linux++;
        }
        bcm_pkt_free(0, pkt);
    }
    return NULL;
}

static int tap_add(int port)
{
    struct tap_port *t;

    if (ntaps >= MAX_TAPS) {
        return -1;
    }
    t = &taps[ntaps];
    sal_memset(t, 0, sizeof *t);
    t->port = port;
    sal_snprintf(t->name, sizeof t->name, "xe%d", port - 1);
    t->fd = tap_open(t->name);
    if (t->fd < 0) {
        return -1;
    }
    ntaps++;
    if (pthread_create(&t->thread, NULL, tap_reader, t) != 0) {
        printf("  ** cannot start reader thread for %s\n", t->name);
        return -1;
    }
    printf("  tap %-6s <-> port %d\n", t->name, port);
    return 0;
}

/* ---- our own RX handler ------------------------------------------------
 *
 * PacketWatcher registers successfully and still shows nothing, while the
 * hardware demonstrably completes descriptors (type-26 word 15 with done=1 and
 * real per-packet byte counts). Counting invocations of our OWN handler splits
 * the remaining possibilities cleanly:
 *
 *   fires  -> the reap path works and pw's own filtering is the problem
 *   silent -> nothing reaps completed descriptors, and the fault is below pw
 *
 * This is instrumentation INSIDE the process. Every previous attempt to judge
 * this from outside -- sampling a self-clearing status register, reading a
 * snapshot counter as a total -- produced a wrong answer, because shared state
 * observed from another process seconds later cannot show a transient.
 */
static volatile long rx_cb_calls;
static volatile long rx_cb_bytes;

static bcm_rx_t sdkpoc_rx_cb(int unit, bcm_pkt_t *pkt, void *cookie)
{
    COMPILER_REFERENCE(unit);
    COMPILER_REFERENCE(cookie);
    rx_cb_calls++;
    if (pkt != NULL) {
        struct tap_port *t = tap_for_port(pkt->rx_port);

        rx_cb_bytes += pkt->pkt_len;
        /* chip -> Linux */
        if (t != NULL && pkt->pkt_data != NULL &&
            pkt->pkt_data[0].data != NULL) {
            uint8 *d = pkt->pkt_data[0].data;
            int    n = pkt->pkt_len;

            /* ⚠ Strip the 802.1Q tag. Frames delivered to the CPU port keep
             * their VLAN tag whatever the VLAN's untagged bitmap says -- the
             * chip tells software which VLAN a frame came from, which is
             * reasonable but not what a tap representing an untagged routed
             * interface wants. Linux silently dropped every tagged frame:
             * the tap's rx_packets climbed while ARP never resolved. Proved by
             * hexdumping here and seeing 8100 00a1 sitting between the MACs
             * and the ethertype. */
            if (n > 16 && d[12] == 0x81 && d[13] == 0x00) {
                sal_memmove(d + 12, d + 16, n - 16);
                n -= 4;
            }
            if (write(t->fd, d, n) > 0) {
                t->rx_to_linux++;
            }
        }
        if (rx_cb_calls <= 12) {
            /* Dump the header bytes. Guessing at what reaches the CPU -- is it
             * VLAN tagged? is it the frame we expect? -- wasted several
             * iterations; 24 bytes settles it every time. */
            int d, dn = pkt->pkt_len < 24 ? pkt->pkt_len : 24;
            printf("  RX[%ld] len=%d port=%d cos=%d  ",
                   rx_cb_calls, pkt->pkt_len, pkt->rx_port, pkt->cos);
            for (d = 0; d < dn; d++) {
                printf("%02x", pkt->pkt_data[0].data[d]);
                if ((d % 2) == 1) printf(" ");
            }
            printf("\n");
            fflush(stdout);
        }
    }
    return BCM_RX_NOT_HANDLED;   /* leave it for anyone else, e.g. pw */
}

/* Run a ';'-separated list of diag shell commands. Each sdkpoc run is a full
 * cold init, so without batching, every command would cost a chip reset and
 * nothing could be sequenced. Used by both SDKPOC_CMD (before port bring-up)
 * and SDKPOC_POSTCMD (after it). */

/*
 * BCM84848 front-panel LEDs.
 *
 * The 48 RJ45 ports on this box light from the PHY, not from anywhere we had
 * been looking. Three mechanisms were candidates and two are now ruled out by
 * measurement rather than by argument:
 *
 *  - NOT the SCD. Arista's own board description creates exactly six LED
 *    blocks -- FlashRate1, Status1 (plus the blue beacon), MultiFan1,
 *    PowerSupply1-2 -- and then 16 link LEDs for the four QSFP ports at
 *    0xA000. There is no block for Ethernet1..48.
 *  - NOT the switch chip's LED processors. CMIC_LEDUP0/1 read LEDUP_EN=0 and
 *    LEDUP_RUNNING=0 on this box, and byte-identical values appear in the
 *    EOS-time register dump -- so they are reset defaults that EOS never
 *    turned on either.
 *
 * That leaves the BCM84848's own LED outputs, which are driven by the PHY's
 * firmware and configured through its top-level command interface. Two things
 * then explain a dark panel exactly:
 *
 *  1. _phy_8481_halt (phy8481.c:6367-6404) parks the LED control block at
 *     0xa82c-0xa83d before halting the ARM core for the MDIO firmware
 *     download, and phy_ext_rom_boot=0 means that download always happens.
 *     Nothing in the SDK ever writes that block again.
 *  2. The SDK's own LED entry points, _phy_848xx_led_type_get/set
 *     (phy8481.c:8910,8931), are declared, defined, and called from nowhere.
 *     They are a hook left for the platform vendor. EOS calls the equivalent
 *     out of Strata; we called nothing at all.
 *
 * So this is the piece the SDK deliberately leaves to us. The handshake
 * itself is the SDK's -- _phy84834_top_level_cmd_{set,get}_v2 are non-static
 * (phy8481.c:616-617) despite the commented-out STATIC, so we drive the
 * vendor's own protocol rather than reimplementing a scratch-register dance.
 */
extern int _phy84834_top_level_cmd_set_v2(int unit, phy_ctrl_t *pc,
                                          uint16 cmd, uint16 arg[], int size);
extern int _phy84834_top_level_cmd_get_v2(int unit, phy_ctrl_t *pc,
                                          uint16 cmd, uint16 arg[], int size);

/* phy8481.h:2129-2130 */
#define PHY848XX_CMD_GET_LED_TYPE  0x8021
#define PHY848XX_CMD_SET_LED_TYPE  0x8022

/* The get handshake waits up to 7 s for the firmware to be ready for a
 * command (phy8481.c:2843). Across 48 ports a wedged PHY would hold the
 * command FIFO for five minutes, so give up on the sweep once the failures
 * stop looking like one bad port. */
#define PHYLED_MAX_FAILS  3

static int phyled_one(int unit, int port, int set, int type, int mode,
                      int *typep, int *modep)
{
    phy_ctrl_t *pc = EXT_PHY_SW_STATE(unit, port);
    uint16 args[5];
    int rv;

    if (pc == NULL) {
        return SOC_E_NOT_FOUND;      /* no external PHY: QSFP or unused */
    }

    sal_memset(args, 0, sizeof args);
    if (set) {
        args[0] = (uint16)type;
        args[1] = (uint16)mode;
        rv = _phy84834_top_level_cmd_set_v2(unit, pc, PHY848XX_CMD_SET_LED_TYPE,
                                            args, 2);
    } else {
        rv = _phy84834_top_level_cmd_get_v2(unit, pc, PHY848XX_CMD_GET_LED_TYPE,
                                            args, 2);
        if (SOC_SUCCESS(rv)) {
            if (typep != NULL) *typep = args[0];
            if (modep != NULL) *modep = args[1];
        }
    }
    return rv;
}

/*
 *   phyled                    read the LED type from every PHY
 *   phyled <port>             read one
 *   phyled set <type> [mode]  write every PHY
 *   phyled set <port> <type> [mode]
 *
 * Read first. Nothing here knows what the type values mean -- that mapping is
 * in the PHY firmware and not in any document we have -- so the honest first
 * move is to ask 48 PHYs what they currently think and see whether they all
 * agree.
 */
static int phyled_fix_one(int unit, int port);
static void phyled_fix_all(int unit);

static void phyled_cmd(int unit, char *args)
{
    int set = 0, port = -1, type = 0, mode = 0;
    int fails = 0, ok = 0, skipped = 0;
    char *tok;

    tok = strtok(args, " \t");
    if (tok != NULL && !strcmp(tok, "fix")) {
        tok = strtok(NULL, " \t");
        if (tok != NULL) {
            int p = (int)strtol(tok, NULL, 0);
            int rv = phyled_fix_one(unit, p);
            printf("  phyled fix: port %d %s\n", p,
                   SOC_SUCCESS(rv) ? "ok" : soc_errmsg(rv));
            fflush(stdout);
        } else {
            phyled_fix_all(unit);
        }
        return;
    }
    if (tok != NULL && !strcmp(tok, "set")) {
        set = 1;
        tok = strtok(NULL, " \t");
    }
    if (tok != NULL) {
        long v = strtol(tok, NULL, 0);
        char *next = strtok(NULL, " \t");
        if (set && next == NULL) {
            type = (int)v;                    /* "set <type>" -- all ports */
        } else {
            port = (int)v;                    /* "<port> ..." */
            if (set) {
                if (next == NULL) {
                    printf("  phyled: set <port> needs a type\n");
                    fflush(stdout);
                    return;
                }
                type = (int)strtol(next, NULL, 0);
                next = strtok(NULL, " \t");
            }
        }
        if (set && next != NULL) {
            mode = (int)strtol(next, NULL, 0);
        }
    } else if (set) {
        printf("  phyled: set needs a type\n");
        fflush(stdout);
        return;
    }

    for (int p = (port >= 0 ? port : 1); p <= (port >= 0 ? port : 64); p++) {
        int t = -1, m = -1, rv;

        rv = phyled_one(unit, p, set, type, mode, &t, &m);
        if (rv == SOC_E_NOT_FOUND) {
            skipped++;
            continue;
        }
        if (SOC_FAILURE(rv)) {
            printf("  port %2d: %s failed (%s)\n", p, set ? "set" : "get",
                   soc_errmsg(rv));
            if (++fails >= PHYLED_MAX_FAILS && port < 0) {
                printf("  ** %d failures, stopping the sweep\n", fails);
                break;
            }
            continue;
        }
        ok++;
        if (set) {
            printf("  port %2d: led type <- %d mode %d\n", p, type, mode);
        } else {
            printf("  port %2d: led type %d mode %d\n", p, t, m);
        }
    }
    printf("  phyled: %d ok, %d failed, %d without an external PHY\n",
           ok, fails, skipped);
    fflush(stdout);
}

/* Clause-45 register access straight through the PHY driver's own accessors.
 *
 * The diag shell's "phy raw c45" cannot see these parts -- it returns 0xffff
 * for everything, including the PMA/PMD ID at 1.2/1.3 that "phy info" reads
 * correctly as 600d/84f9, because raw goes at the switch chip's internal MIIM
 * controller and Arista hangs the copper PHYs off the SCD's three MDIO
 * accelerators instead. pc->read/pc->write are the pointers the driver itself
 * uses, so they land on the same bus our shim serves.
 *
 * Address encoding is the SDK's: devad in bits 21:16, regad in 15:0
 * (shared/phyreg.h:37).
 */
#define C45_ADDR(_devad, _reg)  ((((uint32)(_devad) & 0x3f) << 16) | \
                                 ((uint32)(_reg) & 0xffff))

static int phyreg_rd(int unit, int port, int devad, int reg, uint16 *val)
{
    phy_ctrl_t *pc = EXT_PHY_SW_STATE(unit, port);

    if (pc == NULL || pc->read == NULL) {
        return SOC_E_NOT_FOUND;
    }
    return pc->read(unit, pc->phy_id, C45_ADDR(devad, reg), val);
}

static int phyreg_wr(int unit, int port, int devad, int reg, uint16 val)
{
    phy_ctrl_t *pc = EXT_PHY_SW_STATE(unit, port);

    if (pc == NULL || pc->write == NULL) {
        return SOC_E_NOT_FOUND;
    }
    return pc->write(unit, pc->phy_id, C45_ADDR(devad, reg), val);
}

/*   phyreg <port> <devad> <reg> [value]
 *   phyreg dump <port>            the LED control block, 0xa82c-0xa83d
 */
static void phyreg_cmd(int unit, char *args)
{
    char *tok = strtok(args, " \t");
    int port, devad, reg;
    uint16 val;
    int rv;

    if (tok == NULL) {
        printf("  usage: phyreg <port> <devad> <reg> [value] | phyreg dump <port>\n");
        fflush(stdout);
        return;
    }

    if (!strcmp(tok, "dump")) {
        tok = strtok(NULL, " \t");
        port = tok ? (int)strtol(tok, NULL, 0) : 1;
        printf("  port %d LED control block:\n", port);
        for (reg = 0xa82c; reg <= 0xa83d; reg++) {
            rv = phyreg_rd(unit, port, 1, reg, &val);
            if (SOC_FAILURE(rv)) {
                printf("    0x%04x: read failed (%s)\n", reg, soc_errmsg(rv));
                break;
            }
            printf("    0x%04x: 0x%04x\n", reg, val);
        }
        fflush(stdout);
        return;
    }

    port = (int)strtol(tok, NULL, 0);
    tok = strtok(NULL, " \t");
    if (tok == NULL) { printf("  phyreg: need a devad\n"); fflush(stdout); return; }
    devad = (int)strtol(tok, NULL, 0);
    tok = strtok(NULL, " \t");
    if (tok == NULL) { printf("  phyreg: need a register\n"); fflush(stdout); return; }
    reg = (int)strtol(tok, NULL, 0);
    tok = strtok(NULL, " \t");

    if (tok != NULL) {
        val = (uint16)strtol(tok, NULL, 0);
        rv = phyreg_wr(unit, port, devad, reg, val);
        printf("  port %d %d.0x%04x <- 0x%04x: %s\n", port, devad, reg, val,
               SOC_SUCCESS(rv) ? "ok" : soc_errmsg(rv));
    } else {
        rv = phyreg_rd(unit, port, devad, reg, &val);
        if (SOC_SUCCESS(rv)) {
            printf("  port %d %d.0x%04x = 0x%04x\n", port, devad, reg, val);
        } else {
            printf("  port %d %d.0x%04x: %s\n", port, devad, reg, soc_errmsg(rv));
        }
    }
    fflush(stdout);
}

/* The four registers EOS programs and the SDK does not.
 *
 * Read off this same silicon, under EOS, with the panel lit -- not derived and
 * not guessed. `platform trident diag phy xe45 0x<reg> 1` on this 7050TX-64
 * running EOS 4.14 gives:
 *
 *        reg      EdgeNOS   EOS xe0 (dark)   EOS xe45 (linked)
 *        0xa82c   0x0008    0x0020           0x0020
 *        0xa82f   0x0010    0x0020           0x0020
 *        0xa835   0x0040    0x0020           0x0020
 *        0xa83b   0x0400    0x4924           0x4922
 *
 * The first three read the same on a linked and an unlinked port, so they are
 * configuration. 0xa83b is configuration in its upper bits and state in its
 * low ones -- 0x4924 with no link, 0x4922 with link -- so write the no-link
 * form and let the firmware drive the rest.
 *
 * ⚠ Apply the whole set, never a subset. The SDK default and EOS's config
 * disagree about which register even carries the LED mask: on the default
 * 0xa83c tracks link and 0xa83b is frozen, and under EOS it is the exact
 * reverse. An earlier attempt to force LEDs on by writing masks into 0xa83c
 * lit nothing at all, for exactly that reason.
 */
static const struct { int reg; uint16 val; } LED_CFG[] = {
    { 0xa82c, 0x0020 },
    { 0xa82f, 0x0020 },
    { 0xa835, 0x0020 },
};

/* 0xa83b is five 3-bit LED mode fields, and it is the drive.
 *
 * Established by writing it and looking at the panel, which is the only way
 * any of this got settled:
 *
 *   mode 0  the SDK's default for most fields -- dark
 *   mode 2  LIT. All five fields set to 2 (0x2492) lights the port.
 *   mode 3  what _phy_8481_halt writes to all five (0xb6db) -- the parked state
 *   mode 4  what EOS sets, and dark on our box
 *
 * EOS holds all five at 4 and its firmware rewrites field 0 to 2 when the port
 * gains link: 0x4924 down, 0x4922 up. Ours never rewrites anything, because
 * mode 4 means "let the firmware drive it" and our firmware does not. That is
 * the whole fault -- not the configuration, which we now match exactly, but
 * that nothing was ever going to write this register.
 *
 * So we write it ourselves, from linkscan, using EOS's own two values. The
 * other four fields stay at 4: they are the LEDs EOS's firmware drives for
 * activity and speed, and we have no honest source for those. Leaving them at
 * 4 keeps them dark rather than lighting them with something invented.
 */
#define LED_A83B_LINK_UP    0x4922
#define LED_A83B_LINK_DOWN  0x4924

static void phyled_link_set(int unit, int port, int up)
{
    if (EXT_PHY_SW_STATE(unit, port) == NULL) {
        return;                       /* QSFP: the SCD drives those, via platmon */
    }
    (void)phyreg_wr(unit, port, 1, 0xa83b,
                    up ? LED_A83B_LINK_UP : LED_A83B_LINK_DOWN);
}

/* Runs on the linkscan thread, which already does MDIO through the same shim. */
static void led_linkscan_cb(int unit, bcm_port_t port, bcm_port_info_t *info)
{
    int speed = 0;

    if (info == NULL || info->linkstatus != BCM_PORT_LINK_STATUS_UP) {
        phyled_link_set(unit, port, 0);
        return;
    }
    /* See the sync thread: link without speed is not link on this platform. */
    if (bcm_port_speed_get(unit, port, &speed) != BCM_E_NONE || speed <= 0) {
        phyled_link_set(unit, port, 0);
        return;
    }
    phyled_link_set(unit, port, 1);
}

static int phyled_fix_one(int unit, int port)
{
    size_t i;
    int rv;

    if (EXT_PHY_SW_STATE(unit, port) == NULL) {
        return SOC_E_NOT_FOUND;      /* QSFP or unused: no external PHY */
    }
    for (i = 0; i < sizeof LED_CFG / sizeof LED_CFG[0]; i++) {
        rv = phyreg_wr(unit, port, 1, LED_CFG[i].reg, LED_CFG[i].val);
        if (SOC_FAILURE(rv)) {
            return rv;
        }
    }

    /* Start dark. Seeding from bcm_port_link_status_get here reads UP on ports
     * with nothing plugged in -- this runs before linkscan has settled, and an
     * LED that is wrong is worse than one that is late. The sync thread turns
     * on whatever is genuinely up, within a couple of seconds. */
    phyled_link_set(unit, port, 0);
    return SOC_E_NONE;
}

/* Reconcile every port's LED with its link, forever.
 *
 * A linkscan callback alone is not enough: it reports CHANGES, so a port that
 * is already up when we register never generates one and would stay dark until
 * it flapped. Polling the cached link state costs nothing -- HW linkscan keeps
 * it in software, no MDIO -- and we only write the PHY when our own view
 * changes, so the steady state is silent. It is also self-healing, which a
 * callback is not: one missed event does not leave an LED lying indefinitely. */
static void *phyled_sync_thread(void *arg)
{
    int unit = (int)(intptr_t)arg;
    signed char last[65];
    int p;

    memset(last, -1, sizeof last);
    for (;;) {
        for (p = 1; p <= 64; p++) {
            int link = 0, speed = 0, up;
            if (EXT_PHY_SW_STATE(unit, p) == NULL) continue;
            if (bcm_port_link_status_get(unit, p, &link) != BCM_E_NONE) continue;
            /* ⚠ A link with no speed is not a link -- the same trap the routed
             * port loop documents below. With the BCM84848 driver loaded every
             * unconnected copper port reports "Link Up with Speed 0M!", so
             * link status ALONE lights all 48 LEDs with nothing plugged in.
             * That is exactly what it did on the first attempt. */
            if (bcm_port_speed_get(unit, p, &speed) != BCM_E_NONE) speed = 0;
            up = (link == BCM_PORT_LINK_STATUS_UP && speed > 0) ? 1 : 0;
            if (up != last[p]) {
                phyled_link_set(unit, p, up);
                last[p] = (signed char)up;
            }
        }
        sal_sleep(2);
    }
    return NULL;
}

static void phyled_fix_all(int unit)
{
    int p, ok = 0, failed = 0;

    for (p = 1; p <= 64; p++) {
        int rv = phyled_fix_one(unit, p);
        if (rv == SOC_E_NOT_FOUND) continue;
        if (SOC_FAILURE(rv)) {
            if (failed < 3) {
                printf("  phyled fix: port %d failed (%s)\n", p, soc_errmsg(rv));
            }
            failed++;
            continue;
        }
        ok++;
    }
    printf("  phyled fix: LED config applied to %d PHYs, %d failed\n", ok, failed);
    fflush(stdout);
}

static void run_diag_cmds(int unit, const char *list)
{
    char buf[4096];
    char *p, *next;

    snprintf(buf, sizeof buf, "%s", list);
    for (p = buf; p != NULL; p = next) {
        next = strchr(p, ';');
        if (next != NULL) {
            *next++ = '\0';
        }
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            continue;
        }
        printf("BCM.0> %s\n", p);
        fflush(stdout);
        sh_process_command(unit, p);
        fflush(stdout);
    }
}

int main(int argc, char *argv[])
{
    int phyrd_port = 61;          /* xe60; override with SDKPOC_PHYRD_PORT */
    /* Lane selector lives in the FLAGS argument of bcm_port_phy_get, and
     * LANE0_ACCESS is 1, NOT 0 (wcmod_extra.h:168). Passing flags=0 selects no
     * lane and the read lands on whatever AER page is current -- which is how
     * 0xf010 came back 0x7ac0 instead of 0xa042. */
    int phyrd_lane = 1;           /* LANE0_ACCESS; override SDKPOC_PHYRD_LANE */
    uint16_t dev_id = 0;
    uint8_t rev_id = 0;
    int unit, rv;

    COMPILER_REFERENCE(argc);
    COMPILER_REFERENCE(argv);
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("SDK proof of concept -- BCM56855 via a user-space BDE\n");
    /* Guard the ABI that cost six boots. The SDK libraries are built with
     * INCLUDE_RCPU, which puts a conditional rcpu_tp pointer ahead of
     * config_var_get; building this file without the flag shifts every vector
     * by 8 bytes and the SDK ends up calling interrupt_connect where it wants
     * config_var_get. Fail loudly rather than crash in _shr_ctoi. */
    printf("abi: sizeof(soc_cm_device_vectors_t) = %zu (expect 232 with "
           "INCLUDE_RCPU)\n", sizeof(soc_cm_device_vectors_t));
    if (sizeof(soc_cm_device_vectors_t) != 232) {
        printf("abi: ** REFUSING TO RUN -- built without INCLUDE_RCPU; the "
               "vectors would be misaligned against the SDK libraries **\n");
        return 1;
    }

    STEP("1. BDE: map BAR0, PCI config and the DMA pool");
    if (bde_shim_init() < 0) {
        printf("bde_shim_init failed -- nothing below this can work\n");
        return 1;
    }
    bde_shim_devid(&dev_id, &rev_id);
    printf("PCI device 0x%04x rev 0x%02x %s\n", dev_id, rev_id,
           dev_id == BCM56855_DEVICE_ID ? "(BCM56855, as expected)"
                                        : "** NOT the expected device **");
    if (dev_id != BCM56855_DEVICE_ID) {
        printf("refusing to continue against an unexpected device\n");
        return 1;
    }

    STEP("2. SAL");
    rv = sal_core_init();  report("sal_core_init", rv);
    if (rv < 0) return 1;
    rv = sal_appl_init();  report("sal_appl_init", rv);
    if (rv < 0) return 1;

    /* Backstop: if the SAL already called the hook this is a no-op, and if it
     * did not, the properties still land before soc_cm_device_init needs them. */
    sal_config_init_defaults();

    STEP("3. config manager");
    rv = soc_cm_init();    report("soc_cm_init", rv);
    if (rv < 0) return 1;

    STEP("4. create the device and install our vectors");
    unit = soc_cm_device_create(dev_id, rev_id, NULL);
    printf("soc_cm_device_create -> unit %d\n", unit);
    if (unit < 0) return 1;
    /* Armed BEFORE soc_cm_device_init, because that is what calls soc_attach
     * (cm.c:7027) -- arming after it would trace an already-finished init and
     * capture nothing. SDKPOC_SCHAN_TRACE=<path> turns it on; unset means the
     * binary behaves exactly as every previous run did. */
    bsl_start(getenv("SDKPOC_CMD") != NULL || getenv("SDKPOC_SHELL") != NULL);
    if (getenv("SDKPOC_SCHAN_TRACE")) {
        trace_start(getenv("SDKPOC_SCHAN_TRACE"));
    }

    /* NOTE: soc_cm_device_init calls soc_attach() itself (cm.c:7027 under
     * BCM_ESW_SUPPORT), so the real chip init happens HERE, not at step 5.
     * Every "segfault in soc_cm_device_init" was a segfault inside chip init. */
    rv = soc_cm_device_init(unit, &bde_shim_vectors);
    report("soc_cm_device_init", rv);
    /* Snapshot, NOT a total -- the run has barely started. See step 6. */
    printf("bde DMA after device init (snapshot, not a total):\n  ");
    bde_shim_dma_stats();
    bde_shim_config_stats();
    if (rv < 0) {
        /* Flush the trace on the failure path too -- this is exactly the run
         * whose trace we most want to read. */
        trace_finish();
        return 1;
    }

    /* DO NOT call soc_attach here. soc_cm_device_init already did (cm.c:7027),
     * and a second call is destructive: soc_attach allocates a fresh
     * soc_persist and sal_memset()s it to zero at drv.c:~21273, and only THEN
     * hits the "already attached" guard at drv.c:21425 and returns
     * SOC_E_NONE -- never reaching the memState fill at drv.c:21846.
     *
     * That wiped every memory's index bound to 0, which is why three runs died
     * on "invalid index 1" against a static table that plainly declares 127,
     * 617 and 4095. Measured directly:
     *
     *   MMU_THDU_XPIPE_CONFIG_QGROUP valid=1 info=127  runtime=0
     *   EGR_VLAN_CONTROL_1           valid=1 info=617  runtime=0
     *   VLAN_TAB                     valid=1 info=4095 runtime=0
     *
     * The redundancy was noted when cm.c:7027 was found and then left in
     * place; leaving it in was the bug. */
    STEP("5. attach state (soc_cm_device_init already attached)");
    printf("soc_attached(%d) = %d -- second soc_attach deliberately NOT called\n",
           unit, soc_attached(unit));

    /* THIS is chip initialisation. soc_attach only built software state. */
    STEP("5b. soc_reset_init -- chip reset + soc_do_init");
    if (getenv("SDKPOC_NO_INIT") == NULL) {
        rv = soc_reset_init(unit);
        report("soc_reset_init", rv);
    } else {
        printf("soc_reset_init skipped (SDKPOC_NO_INIT set)\n");
    }
    /* soc_reset_init does chip reset + soc_do_init, and the 2026-08-11 trace
     * showed that is port mapping and DMA setup -- 172 ops, all attributed to
     * soc_trident2_chip_reset and _soc_xgxs_reset_single_tsc. The MMU and the
     * scheduler are NOT in it. soc_mmu_init (drv.c:633) dispatches through the
     * chip driver table to _soc_trident2_mmu_init, which calls soc_td2_lls_init
     * at trident2.c:16083 -- the sequence that is 39% of EOS's cold init. */
    /* soc_misc_init MUST precede soc_mmu_init. The runtime index bounds live
     * in sop->memState[mem].index_max and are populated by
     * _soc_trident2_misc_init (trident2.c:5584+), the sibling entry to
     * _soc_trident2_mmu_init in soc_trident2_drv_funs. Calling mmu_init alone
     * left memState zeroed, so MMU_THDU_XPIPE_CONFIG_QGROUP -- declared
     * index_max 127 in allmems_m.i, matching SOC_TD2_MMU_CFG_QGROUP_MAX 128 --
     * reported index_max 0 and the Q-group loop died on "invalid index 1".
     * drv.c:311 shows the SDK's own ordering: misc_init first. */
    /* Stop theorising about index_max and print it. Three runs have died on
     * "invalid index 1", on three different memories, which means the runtime
     * bound is 0 for everything -- while allmems_m.i declares 127 for
     * MMU_THDU_XPIPE_CONFIG_QGROUP and only one driver table exists
     * (soc_driver_bcm56850_a0 -- 56855 is driven by the 56850 tables, which
     * is what EOS reports too: Chip=BCM56855_A2 Driver=BCM56850_A0). soc_attach fills
     * memState from SOC_MEM_INFO at drv.c:21846 and returned rv=0, so one of
     * these three numbers is not what the source says it should be. */
    STEP("5a. memory index bounds -- measure, do not infer");
    {
        static const struct { const char *name; soc_mem_t mem; } probe[] = {
            { "MMU_THDU_XPIPE_CONFIG_QGROUP", MMU_THDU_XPIPE_CONFIG_QGROUPm },
            { "EGR_VLAN_CONTROL_1",           EGR_VLAN_CONTROL_1m },
            { "VLAN_TAB",                     VLAN_TABm },
        };
        unsigned i;

        printf("NUM_SOC_MEM=%d  SOC_PERSIST=%p\n",
               (int)NUM_SOC_MEM, (void *)SOC_PERSIST(unit));
        for (i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
            soc_mem_t m = probe[i].mem;
            /* SOC_MEM_INFO must NOT be evaluated for a memory this chip
             * does not have. On the BCM56855 EGR_VLAN_CONTROL_1 is absent and
             * dereferencing its info gave SIGSEGV at 0x1c -- a crash in this
             * diagnostic, immediately after a perfectly good attach, which is
             * a maximally misleading place to blow up. Guard it. */
            if (!SOC_MEM_IS_VALID(unit, m)) {
                printf("  %-30s valid=0 (not present on this chip)\n",
                       probe[i].name);
                continue;
            }
            printf("  %-30s valid=1 info.index_max=%d runtime.index_max=%d\n",
                   probe[i].name,
                   SOC_MEM_INFO(unit, m).index_max,
                   soc_mem_index_max(unit, m));
        }
    }

    STEP("5b2. soc_misc_init -- populates memState index bounds");
    if (getenv("SDKPOC_NO_MISC") == NULL) {
        rv = soc_misc_init(unit);
        report("soc_misc_init", rv);
    } else {
        printf("soc_misc_init skipped (SDKPOC_NO_MISC set)\n");
    }

    STEP("5c. soc_mmu_init -- _soc_trident2_mmu_init + soc_td2_lls_init");
    if (getenv("SDKPOC_NO_MMU") == NULL) {
        rv = soc_mmu_init(unit);
        report("soc_mmu_init", rv);
    } else {
        printf("soc_mmu_init skipped (SDKPOC_NO_MMU set)\n");
    }

    /*
     * The BCM layer. Everything above is soc_*, and the 2026-08-11 traces
     * showed that is NOT where the TSC lane window is opened: exactly one
     * captured operation touches real block 18, and both captures contain zero
     * portmod/phymod/bcm_port frames. The port bring-up that lights the lanes
     * lives here, under bcm_port_*, above soc_*.
     *
     * config.bcm already sets polled_irq_mode so this can run without an
     * interrupt, and the recovered known-good binary read SDKPOC_BCM=1, so the
     * lost original almost certainly came this far too.
     *
     * SDKPOC_BCM=1 to enable; unset keeps the previous behaviour exactly.
     */
    if (getenv("SDKPOC_BCM")) {
        /* Install the copper PHY bus BEFORE the port layer: phyctrl binds
     * ext_pc.read/write during probe, so the hook has to be in place first. */
    if (getenv("SDKPOC_PHYBUS")) {
        STEP("5c2. SCD MDIO as the SDK's PHY bus (48 copper ports)");
        if (scd_phy_bus_init() == 0) {
            int prv = phy_i2c_bus_func_hook_set(unit, sdkpoc_phy_bus_rd,
                                                sdkpoc_phy_bus_wr);
            printf("  phy_i2c_bus_func_hook_set rv=%d\n", prv);
            printf("  SCD BAR mapped; ports with phy_bus_i2c_<port>=1 will "
                   "reach their BCM84848 through it\n");
            {   /* prove the path before handing it to the SDK: port 1 is
                 * phy_id 0x001, PMA/PMD (MMD 1) registers 2/3 */
                uint16 hi = 0, lo = 0;
                (void)sdkpoc_phy_bus_rd(unit, 0x001, (1 << 16) | 2, &hi);
                (void)sdkpoc_phy_bus_rd(unit, 0x001, (1 << 16) | 3, &lo);
                printf("  self-test: port 1 PMA/PMD id = 0x%04x 0x%04x%s\n",
                       hi, lo,
                       (hi == 0x600d) ? "  (BCM84848 -- path works)"
                                      : "  ** EXPECTED 0x600d");
            }
        }
    }

    STEP("5d. bcm_attach + bcm_init -- the PORT layer");
        /* type MUST be NULL. bcm_attach auto-selects it from the SOC_IS_*
         * macros (control.c:423-476) and falls through to "esw" for this
         * chip. Passing "bcm56860_a0" got BCM_E_CONFIG (-15) on the 7050SX2 --
         * it is a driver family name, not a chip name. */
        rv = bcm_attach(unit, NULL, NULL, unit);
        report("bcm_attach", rv);
        if (rv >= 0) {
            rv = bcm_init(unit);
            report("bcm_init", rv);
        }
    } else {
        printf("bcm layer skipped (set SDKPOC_BCM=1)\n");
    }

    trace_finish();

    /* ---- the Broadcom diagnostic shell ---------------------------------
     *
     * EOS has this and we did not, until 2026-08-16 -- `platform trident
     * shell`, a hidden CLI mode proxied to the StrataDiags agent
     * (BSHELL-20260816.md). It addresses registers and memories BY NAME,
     * which is the constraint that shaped three days of state-diffing.
     *
     * Nothing here is ported: libdiag.a is already built and already on the
     * link line inside --start-group. It was simply never referenced, so the
     * linker never pulled it in -- the Makefile says as much. Referencing
     * sh_process is the whole change.
     *
     *   SDKPOC_CMD="ps"     run one command and exit -- right for telnet
     *   SDKPOC_SHELL=1      interactive, needs a real tty
     */
    {
        const char *one = getenv("SDKPOC_CMD");
        /* SDKPOC_DAEMON belongs in this list too: the resident agent feeds
         * FIFO lines to the same dispatcher, and without cmdlist_init every
         * one of them answers "No mode command list for unit:0". */
        if (one || getenv("SDKPOC_SHELL") || getenv("SDKPOC_POSTCMD") ||
            getenv("SDKPOC_DAEMON")) {
            /* The SDK's own main reaches the shell via diag_sdk_init() ->
             * diag_init(), which registers the command table. Without it
             * sh_process_command accepts a command and prints nothing. */
            diag_init();
            /* diag_init() does symtab, gvar and background init and does NOT
             * touch the command table -- cmdlist_init() is what binds each
             * attached unit to a command list. The SDK's own main reaches both
             * through diag_sdk_init(); sdkpoc has to call them itself.
             *
             * Without cmdlist_init, cur_mode[] is static-zero, which IS
             * ESW_CMD_MODE, so command_mode_set(unit, ESW_CMD_MODE) hits its
             * `mode == cur_mode[unit]` early return and never assigns
             * cur_cmd_list[]. Every non-global command then answers "No mode
             * command list for unit:0" -- `version` worked because it is a
             * global builtin, `ps` did not. cmdlist_init sets cur_mode[] to
             * ILLEGAL_CMD_MODE first, which is exactly what makes the
             * subsequent bind take. */
            cmdlist_init();
            printf("diag: soc_ndev=%d, unit %d bound to a command list\n",
                   soc_ndev, unit);
        }
        if (one) {
            STEP("5e. diag shell");
            run_diag_cmds(unit, one);
        } else if (getenv("SDKPOC_SHELL")) {
            STEP("5e. diag shell, interactive -- 'exit' to leave");
            sh_process(unit, "BCM", 1);
        }
    }

    /* ---- EOS's per-port bring-up sequence ------------------------------
     *
     * bcm_init brings the CHIP up; it does not configure an individual port
     * for a medium, speed and interface. Static analysis of EOS's own
     * libStrataAgent.so shows Arista issues an explicit per-port sequence
     * after bcm_init -- see docs/EOS-PORT-BRINGUP-STATIC.md. sdkpoc never did
     * any of it, which fits a port with PMD lock on all four lanes, PCS config
     * byte-identical to EOS, and no PCS lock.
     *
     * Every call here appears in EOS's undefined-symbol set; none is invented.
     * Check the result with tools/tscread.sh 0xc137 -- 1 means the PCS locked.
     *
     *   SDKPOC_PORTUP=61
     */
    if (getenv("SDKPOC_PORTUP")) {
        int p = atoi(getenv("SDKPOC_PORTUP"));
        bcm_pbmp_t pbmp, okay;
        int prv;

        STEP("5g. per-port bring-up (EOS's sequence)");
        BCM_PBMP_CLEAR(pbmp);
        BCM_PBMP_PORT_ADD(pbmp, p);

        BCM_PBMP_CLEAR(okay);
        prv = bcm_port_probe(unit, pbmp, &okay);
        printf("  bcm_port_probe            rv=%d\n", prv);

        prv = bcm_port_interface_set(unit, p, BCM_PORT_IF_XGMII);
        printf("  bcm_port_interface_set    rv=%d  (XGMII)\n", prv);

        prv = bcm_port_speed_set(unit, p, 40000);
        printf("  bcm_port_speed_set        rv=%d  (40G)\n", prv);

        prv = bcm_port_duplex_set(unit, p, BCM_PORT_DUPLEX_FULL);
        printf("  bcm_port_duplex_set       rv=%d\n", prv);

        prv = bcm_port_autoneg_set(unit, p, 0);
        printf("  bcm_port_autoneg_set      rv=%d  (off -- SR4 optic)\n", prv);

        prv = bcm_port_enable_set(unit, p, 1);
        printf("  bcm_port_enable_set       rv=%d\n", prv);

        prv = bcm_linkscan_mode_set_pbm(unit, pbmp, BCM_LINKSCAN_MODE_HW);
        printf("  bcm_linkscan_mode_set_pbm rv=%d  (HW)\n", prv);

        /* THIS IS THE CALL THAT MAKES TX WORK, and its absence is why every
         * `tx` returned "Could not send pkt with dv_vcnt = 0" while the port
         * showed link up and STP Forward.
         *
         * mode_set_pbm only says HOW a port would be scanned; it does not
         * start the scan. Without enable_set the linkscan thread never runs,
         * so soc_persist's lc_pbm_link stays empty -- and bcm_tx ANDs the
         * requested port bitmap against it (tx.c:5452-5463):
         *
         *   BCM_PBMP_AND(tx_pbmp, sop->lc_pbm_link);
         *   ...
         *   if (BCM_PBMP_IS_NULL(tx_pbmp) && !BCM_PBMP_IS_NULL(pkt->tx_pbmp))
         *       return BCM_E_NONE;
         *
         * That returns SUCCESS having added no descriptors, which is why the
         * failure was silent and looked like a DMA or DCB problem for hours.
         * `linkscan` in the diag shell printed "Linkscan disabled" and settled
         * it. Interval is the SDK's usual 250 ms.
         */
        prv = bcm_linkscan_enable_set(unit, 250000);
        printf("  bcm_linkscan_enable_set   rv=%d  (250 ms)\n", prv);

        sal_sleep(5);
        {
            int link = -1;
            prv = bcm_port_link_status_get(unit, p, &link);
            printf("  bcm_port_link_status_get  rv=%d  link=%d\n", prv, link);
        }
    }

    /* ---- probe every port's PHY ----------------------------------------
     *
     * `phy info` shows all 61 ports bound to the INTERNAL TSC SerDes
     * (TSC-A2/xx/x at addresses 0x8d, 0x8e, ...) and none to the external
     * BCM84848s that actually terminate the 48 copper ports. Their MDIO
     * addresses ARE configured -- config.bcm carries 61 port_phy_addr_N.0
     * entries (0x1, 0x20, 0x23, ...) taken from EOS -- but nothing has probed
     * them, so every copper port reports IF(XFI) at 10G and stays down.
     *
     * bcm_port_probe is what walks the configured addresses and binds a PHY
     * driver. The 5g block calls it for one port; this calls it for all of
     * them, which is what a switch that is meant to behave like EOS needs.
     *
     *   SDKPOC_PROBE_ALL=1
     */
    if (getenv("SDKPOC_PROBE_ALL")) {
        bcm_pbmp_t pbmp, okay;
        int prv, n = 0, p;

        STEP("5k. probe all ports for external PHYs");
        BCM_PBMP_CLEAR(pbmp);
        BCM_PBMP_ASSIGN(pbmp, PBMP_PORT_ALL(unit));
        BCM_PBMP_CLEAR(okay);
        prv = bcm_port_probe(unit, pbmp, &okay);
        BCM_PBMP_ITER(okay, p) { n++; }
        printf("  bcm_port_probe            rv=%d  %d ports probed OK\n",
               prv, n);
    }

    /* ---- start the RX module -------------------------------------------
     *
     * bcm_init does NOT start packet reception. Without this, PacketWatcher
     * comes up, allocates its buffers and silently receives nothing -- the
     * giveaway is only visible at SDKPOC_LOG_LEVEL=7:
     *
     *   API: bcm_rx_cfg_get(0,...) -> -17 Feature not initialized
     *
     * after which pw frees every buffer it just allocated and gives up. The
     * traffic is genuinely there: the peer's multicast reaches the MAC
     * (RPKT.xe60) and the switch enqueues it to the CPU port
     * (MC_PERQ_PKT(0).cpu0), it just has no software side to land in.
     *
     * NULL cfg asks the SDK for its own defaults, which is what we want here --
     * the point is to prove the path, not to tune it.
     *
     * ⚠ init and start are SEPARATE knobs on purpose. PacketWatcher starts RX
     * itself, so doing both here makes `pw start` fail with
     *
     *   PW rx mode: Cannot start RX: Operation still running.
     *   AbnormalThreadExit:bcmPW.0
     *
     * which is a different failure wearing the same "pw sees nothing" clothes.
     * bcm_rx_init alone is what pw actually needs.
     *
     *   SDKPOC_RX=1        bcm_rx_init only -- use this with pw
     *   SDKPOC_RX=start    also bcm_rx_start, for a handler of our own
     */
    if (getenv("SDKPOC_RX")) {
        const char *mode = getenv("SDKPOC_RX");
        int prv;

        STEP("5j. start the RX module");
        prv = bcm_rx_init(unit);
        printf("  bcm_rx_init               rv=%d\n", prv);
        if (strcmp(mode, "start") == 0) {
            prv = bcm_rx_start(unit, NULL);
            printf("  bcm_rx_start              rv=%d  (default cfg)\n", prv);
        } else {
            printf("  bcm_rx_start              skipped -- pw starts RX itself\n");
        }

        /* Register our own counting handler. BCM_RCO_F_ALL_COS so no CoS
         * binding can hide packets from us, and priority 100 so we sit above
         * anything pw installs. The callback returns BCM_RX_NOT_HANDLED so it
         * observes without consuming. */
        prv = bcm_rx_register(unit, "sdkpoc-count", sdkpoc_rx_cb,
                              100, NULL, BCM_RCO_F_ALL_COS);
        printf("  bcm_rx_register           rv=%d  (ALL_COS, prio 100)\n", prv);

        /* ---- force the hardware IRQ mask ------------------------------
         *
         * polled_irq_mode=1 makes the SDK write ZERO to the hardware mask
         * (intr_cmicm.c:1618, "In polled mode, the hardware IRQ mask is always
         * zero") and keep the real mask in a software shadow. The polled
         * handler then gates on
         *
         *     CMCx_IRQ0_STAT(dev,cmc) & SOC_CMCx_IRQ0_MASK(dev,cmc)
         *
         * (ipoll.c:107) -- hardware status ANDed with that shadow. We have
         * measured a descriptor completing (type-26 word 15 = 0x80030044:
         * done=1, start=1, end=1, count=68) while IRQ_STAT0 stayed 0x43 and
         * never showed DescDone1/ChainDone1. If the status bits are gated by
         * the hardware mask, polled mode can never observe packet-DMA
         * completion on this chip.
         *
         * EOS runs this chip with real interrupts and PCIE_IRQ_MASK0 = 0xB020
         * (ChainDone0|ChainDone1|DescDone1|FifoDma0), measured directly. Write
         * that value and see whether the status bits appear and pw delivers.
         *
         *   SDKPOC_IRQMASK=0xB020
         */
        if (getenv("SDKPOC_IRQMASK")) {
            uint32 want = (uint32)strtoul(getenv("SDKPOC_IRQMASK"), NULL, 0);
            uint32 off = 0x31414;   /* CMIC_CMC0_PCIE_IRQ_MASK0, cmicm.h:3573 */
            uint32 before, after;

            before = soc_pci_read(unit, off);
            (void)soc_pci_write(unit, off, want);
            after = soc_pci_read(unit, off);
            printf("  PCIE_IRQ_MASK0            0x%08x -> wrote 0x%08x -> "
                   "0x%08x%s\n", before, want, after,
                   after == want ? "  (took)" : "  (DID NOT TAKE)");
            printf("  IRQ_STAT0                 0x%08x\n",
                   soc_pci_read(unit, 0x31400));
        }
    }

    /* ---- replay EOS's PCS write sequence -------------------------------
     *
     * The 63 writes below are EOS's own, captured on this board by hooking
     * soc_miim_write / soc_esw_miim_write in tools/stratatrace.c and issuing
     * shutdown / no shutdown on Ethernet52/1. They are transcribed verbatim
     * from /mnt/flash/pcs-writes.txt, in order, repeats included --
     * docs/EOS-PCS-WRITE-SEQUENCE.md has the full listing and the analysis.
     *
     * Nothing here is invented or interpolated. The shape is a
     * hold -> configure -> release that our bcm_port_* bring-up never
     * performs: 0xc010.0x17 = 0xa000 per lane and 0xc010.0x1a = 1 to hold,
     * the 0xc100/0xc110/0xc130/0xc180 config on lane 6, then
     * 0xc010.0x17 = 0x2000, 0xc010.0x1a = 0 and 0xc100.0x10 = 0x4021 to
     * release.
     *
     * We replay through soc_esw_miim_write because that is the exact layer
     * the trace was taken at -- bcm_port_phy_get was tried first and was
     * demonstrably not decoding the register address (it returned identical
     * values for different addresses), so the bcm_port_phy_set counterpart
     * is not trustworthy for this.
     *
     * ⚠ TWO ASSUMPTIONS, both stated rather than hidden:
     *
     *   1. Registers 0x1e (lane/AER) and 0x1f (block) are pure selectors with
     *      no side effects, so re-emitting them before every data write is
     *      equivalent to EOS's behaviour of writing them only when they
     *      change. The capture collapsed them out, so this cannot be read
     *      back off the trace.
     *   2. No inter-phase delay or poll-until-ready is needed. The trace has
     *      1,133 MIIMR reads interleaved with these writes that have NOT been
     *      correlated, so if EOS waits on a status bit somewhere in here, we
     *      do not yet know where. If the replay fails this is the first thing
     *      to go and look at.
     *
     * This writes to one port's SerDes only. Worst case that port stays down;
     * re-running init or a reboot restores it. It is not a blind sweep -- every
     * value is one EOS itself writes to this exact chip.
     *
     *   SDKPOC_PCSSEQ=1                 replay, then read the 0xc137 oracle
     *   SDKPOC_PCSSEQ_PHY=0x1a1         override the phy_id
     *   SDKPOC_PCSSEQ_DRYRUN=1          print the writes, issue none
     */
    if (getenv("SDKPOC_PCSSEQ")) {
        static const struct { uint16 blk; uint8 lane; uint8 reg; uint16 val; }
        pcsseq[] = {
            { 0xc180, 6, 0x10, 0x4000 },
            { 0xc130, 6, 0x18, 0x000d },
            { 0xc100, 6, 0x14, 0x0000 },
            { 0xc100, 6, 0x10, 0x0021 },
            { 0xc100, 6, 0x14, 0x0000 },
            { 0xc100, 6, 0x10, 0x0000 },
            { 0xc110, 6, 0x13, 0xc1c8 },
            { 0xc110, 6, 0x13, 0xc1c8 },
            { 0xc010, 0, 0x17, 0xa000 },
            { 0xc010, 1, 0x17, 0xa000 },
            { 0xc010, 2, 0x17, 0xa000 },
            { 0xc010, 3, 0x17, 0xa000 },
            { 0xc010, 6, 0x1a, 0x0001 },
            { 0xc130, 6, 0x17, 0x0000 },
            { 0xc100, 6, 0x10, 0x0021 },
            { 0xc100, 6, 0x11, 0x9800 },
            { 0xc100, 6, 0x12, 0x0040 },
            { 0xc100, 6, 0x13, 0x0001 },
            { 0xc100, 6, 0x14, 0x0000 },
            { 0xc100, 6, 0x15, 0x0000 },
            { 0xc100, 6, 0x10, 0x0021 },
            { 0xc110, 6, 0x11, 0x0010 },
            { 0xc110, 6, 0x13, 0xc1c8 },
            { 0xc110, 6, 0x14, 0x0000 },
            { 0xc130, 6, 0x10, 0x33c0 },
            { 0xc130, 6, 0x16, 0x0004 },
            { 0xc130, 6, 0x14, 0x2072 },
            { 0xc130, 6, 0x11, 0x0000 },
            { 0xc110, 6, 0x13, 0xc1c8 },
            { 0xc130, 6, 0x14, 0x2072 },
            { 0xc010, 0, 0x17, 0xa000 },
            { 0xc010, 0, 0x17, 0xa000 },
            { 0xc010, 0, 0x17, 0xa000 },
            { 0xc010, 1, 0x17, 0xa000 },
            { 0xc010, 1, 0x17, 0xa000 },
            { 0xc010, 1, 0x17, 0xa000 },
            { 0xc010, 2, 0x17, 0xa000 },
            { 0xc010, 2, 0x17, 0xa000 },
            { 0xc010, 2, 0x17, 0xa000 },
            { 0xc010, 3, 0x17, 0xa000 },
            { 0xc010, 3, 0x17, 0xa000 },
            { 0xc010, 3, 0x17, 0xa000 },
            { 0xc180, 6, 0x18, 0x0000 },
            { 0xc110, 6, 0x13, 0xc1ca },
            { 0xc110, 6, 0x13, 0xc1cb },
            { 0xc100, 6, 0x10, 0x4021 },
            { 0xc010, 0, 0x17, 0xa000 },
            { 0xc010, 1, 0x17, 0xa000 },
            { 0xc010, 2, 0x17, 0xa000 },
            { 0xc010, 3, 0x17, 0xa000 },
            { 0xc010, 0, 0x17, 0xa000 },
            { 0xc010, 1, 0x17, 0xa000 },
            { 0xc010, 2, 0x17, 0xa000 },
            { 0xc010, 3, 0x17, 0xa000 },
            { 0xc130, 6, 0x18, 0x0009 },
            { 0xc010, 6, 0x1a, 0x0000 },
            { 0xc010, 0, 0x17, 0x2000 },
            { 0xc010, 1, 0x17, 0x2000 },
            { 0xc010, 2, 0x17, 0x2000 },
            { 0xc010, 3, 0x17, 0x2000 },
            { 0xc110, 6, 0x13, 0xc1cb },
            { 0xc110, 6, 0x13, 0xc1cb },
            { 0xc100, 6, 0x10, 0x4021 },
        };
        const int nseq = (int)(sizeof pcsseq / sizeof pcsseq[0]);
        uint32 phy = 0x1a1;   /* what the SDK passes for xe60 on this board */
        int dry = getenv("SDKPOC_PCSSEQ_DRYRUN") != NULL;
        uint16 before = 0, after = 0;
        int i, prv, nerr = 0;

        if (getenv("SDKPOC_PCSSEQ_PHY")) {
            phy = (uint32)strtoul(getenv("SDKPOC_PCSSEQ_PHY"), NULL, 0);
        }

        STEP("5h. replay EOS's PCS write sequence");
        printf("  phy_id 0x%x, %d writes%s\n", phy, nseq,
               dry ? "  (DRY RUN -- nothing is issued)" : "");

        /* The oracle. 0xc137 is block 0xc130 offset 0x17; EOS writes 0 to it
         * during configure and never again, so a 1 here is the hardware
         * reporting PCS lock. Read it the same three-step way EOS does. */
        if (!dry) {
            (void)soc_esw_miim_write(unit, phy, 0x1e, 6);
            (void)soc_esw_miim_write(unit, phy, 0x1f, 0xc130);
            prv = soc_esw_miim_read(unit, phy, 0x17, &before);
            printf("  0xc137 before = 0x%04x (rv=%d)\n", before, prv);
        }

        for (i = 0; i < nseq; i++) {
            if (dry) {
                printf("    [%2d] lane %d  blk 0x%04x  reg 0x%02x = 0x%04x\n",
                       i, pcsseq[i].lane, pcsseq[i].blk,
                       pcsseq[i].reg, pcsseq[i].val);
                continue;
            }
            prv = soc_esw_miim_write(unit, phy, 0x1e, pcsseq[i].lane);
            if (prv >= 0) {
                prv = soc_esw_miim_write(unit, phy, 0x1f, pcsseq[i].blk);
            }
            if (prv >= 0) {
                prv = soc_esw_miim_write(unit, phy, pcsseq[i].reg,
                                         pcsseq[i].val);
            }
            if (prv < 0) {
                printf("    [%2d] lane %d blk 0x%04x reg 0x%02x = 0x%04x"
                       "  ERROR rv=%d\n", i, pcsseq[i].lane, pcsseq[i].blk,
                       pcsseq[i].reg, pcsseq[i].val, prv);
                nerr++;
            }
        }

        if (!dry) {
            printf("  %d writes issued, %d errors\n", nseq * 3, nerr);
            sal_sleep(3);
            (void)soc_esw_miim_write(unit, phy, 0x1e, 6);
            (void)soc_esw_miim_write(unit, phy, 0x1f, 0xc130);
            prv = soc_esw_miim_read(unit, phy, 0x17, &after);
            printf("  0xc137 after  = 0x%04x (rv=%d)%s\n", after, prv,
                   (after & 1) ? "   <- PCS LOCK" : "   <- still no PCS lock");

            if (getenv("SDKPOC_PORTUP")) {
                int link = -1, p = atoi(getenv("SDKPOC_PORTUP"));
                prv = bcm_port_link_status_get(unit, p, &link);
                printf("  bcm_port_link_status_get  rv=%d  link=%d\n",
                       prv, link);
            }
        }
    }

    /* ---- TSC register reads -------------------------------------------
     *
     * `phy raw c45` cannot reach this PHY: EOS itself answers
     *   ERROR: MII Addr 417: soc_miim_read failed: Feature unavailable
     * on a port whose link is UP, and our build silently returns 0x0000 for
     * the same read -- which produced a confident wrong conclusion once
     * already (docs/SCD-SMBUS-WORKING.md).
     *
     * bcm_port_phy_get routes through the PHY driver's OWN accessor
     * (phy_tscmod_reg_read -> phy_tscmod_reg_aer_read), which is the path
     * `phy diag dsc` uses successfully every run, rather than the raw MII
     * path that fails. Registers are plain TSC addresses, e.g. 0xf010 is the
     * uC version and must read 0xa042 on this board -- a built-in self-test
     * that says whether the path works before any value is believed.
     *
     *   SDKPOC_PHYRD="0xf010,0xf015,0xc150"
     */
    {
        const char *rd = getenv("SDKPOC_PHYRD");
        if (rd != NULL) {
            char buf[512];
            char *p, *next;
            int prv;

                    if (getenv("SDKPOC_PHYRD_PORT")) {
                phyrd_port = atoi(getenv("SDKPOC_PHYRD_PORT"));
            }
            if (getenv("SDKPOC_PHYRD_LANE")) {
                phyrd_lane = atoi(getenv("SDKPOC_PHYRD_LANE"));
            }
            STEP("5f. TSC register reads via bcm_port_phy_get");
            printf("  port %d, lane flags 0x%x (LANE0_ACCESS=1)\n",
                   phyrd_port, phyrd_lane);
            snprintf(buf, sizeof buf, "%s", rd);
            for (p = buf; p != NULL; p = next) {
                uint32 val = 0;
                unsigned long reg;

                next = strchr(p, ',');
                if (next != NULL) {
                    *next++ = '\0';
                }
                while (*p == ' ') p++;
                if (*p == '\0') continue;
                reg = strtoul(p, NULL, 0);
                prv = bcm_port_phy_get(unit, phyrd_port,
                                       (uint32)phyrd_lane,
                                       (uint32)reg, &val);
                if (prv < 0) {
                    printf("  0x%04lx : ERROR rv=%d\n", reg, prv);
                } else {
                    printf("  0x%04lx = 0x%04x%s\n", reg, val,
                           reg == 0xf010
                             ? (val == 0xa042 ? "   <- uC ver OK, path works"
                                              : "   <- EXPECTED 0xa042, path suspect")
                             : "");
                }
            }
        }
    }

    /* ---- tap interfaces ------------------------------------------------
     * After RX is registered (the callback feeds them) and before port mode,
     * so a routed port already has its netdev when addresses are assigned. */
    if (getenv("SDKPOC_TAP")) {
        /* ⚠ This buffer was 128 bytes, which is smaller than a list of all 61
         * ports (~230 chars). Asking for every port would have been TRUNCATED
         * mid-number and silently given a partial set. Ranges exist so the
         * common case is short, and the buffer is now big enough for the long
         * one either way. */
        char buf[512];
        char *p2, *next;

        STEP("5l. tap interfaces (Linux netdev per ASIC port)");
        sal_snprintf(buf, sizeof buf, "%s", getenv("SDKPOC_TAP"));
        for (p2 = buf; p2 != NULL; p2 = next) {
            char *dash;
            next = strchr(p2, ',');
            if (next != NULL) {
                *next++ = '\0';
            }
            while (*p2 == ' ') p2++;
            if (*p2 == '\0') {
                continue;
            }
            /* "a-b" is an inclusive range; anything else is a single port. */
            dash = strchr(p2, '-');
            if (dash != NULL) {
                int lo, hi, port;
                *dash = '\0';
                lo = atoi(p2);
                hi = atoi(dash + 1);
                if (lo > 0 && hi >= lo) {
                    for (port = lo; port <= hi; port++) {
                        if (tap_add(port) < 0) {
                            printf("  ** stopped at port %d: no tap slots left "
                                   "(MAX_TAPS=%d)\n", port, MAX_TAPS);
                            break;
                        }
                    }
                }
            } else {
                (void)tap_add(atoi(p2));
            }
        }
        printf("  %d tap(s) created -- ip link set <name> up to use them\n",
               ntaps);
    }

    /* ---- port mode: routed (like EOS) or L2 bridged ---------------------
     *
     * EOS runs this box with the front-panel ports ROUTED, not switched --
     * artifacts/golden-20260816-40g/sys-running.txt:
     *
     *     interface Ethernet48          interface Ethernet52/1
     *        no switchport                 no switchport
     *        ip address 10.101.103.1/29    ip address 10.101.101.50/29
     *        ipv6 address 2001:...::1/64   ipv6 address 2001:...::1/64
     *
     * so parity means each port is its own L3 interface, isolated from the
     * others, with routing enabled -- NOT all of them bridged together in
     * VLAN 1, which is what bcm_init leaves behind by default.
     *
     * routed  (default): one VLAN per port, the port moved out of VLAN 1 into
     *                    it, an L3 interface created on that VLAN with a
     *                    per-port router MAC, and IPv4/IPv6 routing enabled on
     *                    ingress. Ports cannot reach each other at L2 -- they
     *                    are separate subnets, exactly as EOS had them.
     * l2:               leave the ports in VLAN 1 together so they bridge.
     *                   This is the OPTION, not the default.
     *
     * The router MAC is the base MAC with the low byte replaced by the port
     * number, so every L3 interface is distinct without needing a MAC pool.
     * Addresses and routes are the control plane's job, not this program's --
     * what is built here is the forwarding state a routing stack then
     * populates.
     *
     *   SDKPOC_PORTMODE=routed   (or l2)
     *   SDKPOC_PORTMODE_VLAN=100 base VLAN for routed mode (default 100)
     */
    if (getenv("SDKPOC_PORTMODE")) {
        const char *mode = getenv("SDKPOC_PORTMODE");
        int is_l2 = (strcmp(mode, "l2") == 0);
        int vbase = getenv("SDKPOC_PORTMODE_VLAN")
                        ? atoi(getenv("SDKPOC_PORTMODE_VLAN")) : 100;
        bcm_pbmp_t all;
        int p, prv, nrouted = 0, nl2 = 0;

        STEP("5m. port mode: %s", is_l2 ? "L2 bridged" : "routed (EOS parity)");

        BCM_PBMP_CLEAR(all);
        BCM_PBMP_ASSIGN(all, PBMP_PORT_ALL(unit));

        /* Bring the ports up FIRST. An earlier version configured mode before
         * anything was enabled, so bcm_port_link_status_get reported every
         * port down and it configured nothing at all -- while still printing
         * a cheerful l3_enable rv=0. */
        BCM_PBMP_ITER(all, p) {
            (void)bcm_port_enable_set(unit, p, 1);
        }
        (void)bcm_linkscan_mode_set_pbm(unit, all, BCM_LINKSCAN_MODE_HW);
        prv = bcm_linkscan_enable_set(unit, 250000);
        printf("  ports enabled, linkscan rv=%d -- settling\n", prv);
        sal_sleep(10);

        if (is_l2) {
            /* Everything shares VLAN 1 -- which is where bcm_init already put
             * them, so this is mostly an assertion that nothing moved them. */
            prv = bcm_vlan_port_add(unit, 1, all, all);
            printf("  bcm_vlan_port_add(vlan 1) rv=%d  -- ports bridge\n", prv);
            BCM_PBMP_ITER(all, p) { nl2++; }
            printf("  %d ports in VLAN 1\n", nl2);
        } else {
            prv = bcm_l3_enable_set(unit, 1);
            printf("  bcm_l3_enable_set         rv=%d\n", prv);
            /* ⚠ Without this, bcm_l3_egress_create returns -12 "Operation
             * disabled". The SDK defaults to the legacy interface-based L3
             * model; egress OBJECTS -- which is what a FIB sync needs, one per
             * next hop, shared by many routes -- require advanced egress
             * management to be switched on explicitly. */
            prv = bcm_switch_control_set(unit, bcmSwitchL3EgressMode, 1);
            printf("  bcmSwitchL3EgressMode     rv=%d\n", prv);

            /* ⚠ TTL-1 CONTROL TRAFFIC MUST BE TRAPPED, OR ROUTING PROTOCOLS
             * NEVER CONVERGE.
             *
             * OSPF sends with TTL 1 by design. Once the my-station MAC is
             * marked L3LOOKUP the chip routes what is addressed to us, and a
             * TTL-1 packet entering the routing path decrements to 0 and is
             * dropped. The result is an adjacency stuck in ExStart forever,
             * retransmitting Database Description, while ICMP to the very same
             * address answers perfectly -- ping carries TTL 64.
             *
             * That asymmetry is the whole tell, and it cost a long detour:
             * local termination was verified working with ping and looked
             * fine. The EdgeNOS project's own notes name this exactly --
             * "CPU_CONTROL_1 TTL1 traps" in core/control-plane/build-quagga.sh. */
            prv = bcm_switch_control_set(unit, bcmSwitchL3UcastTtl1ToCpu, 1);
            printf("  L3UcastTtl1ToCpu          rv=%d\n", prv);
            prv = bcm_switch_control_set(unit, bcmSwitchIpmcTtl1ToCpu, 1);
            printf("  IpmcTtl1ToCpu             rv=%d\n", prv);
            prv = bcm_switch_control_set(unit, bcmSwitchL3UcTtlErrToCpu, 1);
            printf("  L3UcTtlErrToCpu           rv=%d\n", prv);

            BCM_PBMP_ITER(all, p) {
                bcm_l3_intf_t intf;
                bcm_pbmp_t pbm, ubm;
                bcm_vlan_t vid = (bcm_vlan_t)(vbase + p);
                int link = 0;

                /* Only bother with ports that actually have a link; the rest
                 * would just be 61 unused VLANs and L3 interfaces. */
                {
                    int speed = 0;
                    if (bcm_port_link_status_get(unit, p, &link) != BCM_E_NONE
                        || !link) {
                        continue;
                    }
                    /* ⚠ A link with no speed is not a link. With the BCM84848
                     * driver loaded, every unconnected copper port reports
                     * "Link Up with Speed 0M!" -- taking that at face value
                     * configured all 48 of them as routed interfaces. */
                    if (bcm_port_speed_get(unit, p, &speed) != BCM_E_NONE
                        || speed <= 0) {
                        continue;
                    }

                    /* ⚠⚠ THE MAC INTERFACE MUST MATCH THE NEGOTIATED SPEED.
                     *
                     * This is what stopped the copper ports working. A 10GBASE-T
                     * port left at its default IF(XFI) while the copper side
                     * autonegotiates 1G gives a PHY with link on BOTH sides that
                     * bridges nothing: we transmitted, the Nexus transmitted, and
                     * neither received a single frame -- for hours.
                     *
                     * It was invisible from the usual places. `ps` reported the
                     * port up at 1G, the PHY reported link and AN complete, and
                     * the far end agreed the link was up. MAC loopback passed.
                     * PHY loopback passed -- so MAC <-> SerDes <-> PHY system
                     * side was fine all along. Only `port xe47` showing
                     * IF(XFI) next to Medium(Copper) gave it away.
                     *
                     * SGMII for 1G/100M, XFI for 10G. The 40G QSFP ports have no
                     * external PHY and keep XGMII, so only touch ports whose
                     * speed says otherwise. */
                    if (speed <= 2500) {
                        prv = bcm_port_interface_set(unit, p,
                                                     BCM_PORT_IF_SGMII);
                        printf("  port %-3d %d Mb -> IF SGMII rv=%d\n",
                               p, speed, prv);
                    } else if (speed <= 10000) {
                        prv = bcm_port_interface_set(unit, p, BCM_PORT_IF_XFI);
                        printf("  port %-3d %d Mb -> IF XFI rv=%d\n",
                               p, speed, prv);
                    }
                }

                BCM_PBMP_CLEAR(pbm);
                BCM_PBMP_PORT_ADD(pbm, p);
                BCM_PBMP_CLEAR(ubm);
                BCM_PBMP_PORT_ADD(ubm, p);
                /* The CPU port MUST be in the VLAN or nothing arriving on this
                 * port can reach software. Without it the peer's ARP replies
                 * were transmitted, received at the MAC, and then dropped with
                 * nowhere to go -- ping saw 100% loss while a capture on the
                 * far end showed a perfectly good reply on the wire. Tagged
                 * (not in ubm): the CPU keeps the tag, the front port does
                 * not.
                 *
                 * ⚠ And UNTAGGED. Adding the CPU tagged delivered every frame
                 * to Linux with an 802.1Q header on an interface that has no
                 * VLAN configured, so the kernel dropped them all: the tap's
                 * rx_packets climbed while ARP still failed. */
                BCM_PBMP_PORT_ADD(pbm, CMIC_PORT(unit));
                BCM_PBMP_PORT_ADD(ubm, CMIC_PORT(unit));

                /* Report every step: swallowing these hid the fact that
                 * the VLAN moves were not taking effect while the L3
                 * interfaces were being created regardless. */
                prv = bcm_vlan_create(unit, vid);
                if (prv != BCM_E_NONE && prv != BCM_E_EXISTS) {
                    printf("  port %d: vlan_create(%d) rv=%d %s\n",
                           p, vid, prv, bcm_errmsg(prv));
                    continue;
                }
                prv = bcm_vlan_port_remove(unit, 1, pbm);
                if (prv != BCM_E_NONE) {
                    printf("  port %d: vlan_port_remove(1) rv=%d %s\n",
                           p, prv, bcm_errmsg(prv));
                }
                prv = bcm_vlan_port_add(unit, vid, pbm, ubm);
                if (prv != BCM_E_NONE) {
                    printf("  port %d: vlan_port_add(%d) rv=%d %s\n",
                           p, vid, prv, bcm_errmsg(prv));
                    continue;
                }
                /* The port's default VLAN must follow it, or untagged ingress
                 * still lands in VLAN 1. */
                prv = bcm_port_untagged_vlan_set(unit, p, vid);
                if (prv != BCM_E_NONE) {
                    printf("  port %d: untagged_vlan_set(%d) rv=%d %s\n",
                           p, vid, prv, bcm_errmsg(prv));
                }

                bcm_l3_intf_t_init(&intf);
                intf.l3a_vid = vid;
                intf.l3a_mac_addr[0] = 0x02;
                intf.l3a_mac_addr[1] = 0x1c;
                intf.l3a_mac_addr[2] = 0x73;
                intf.l3a_mac_addr[3] = 0x00;
                intf.l3a_mac_addr[4] = (uint8)(vid >> 8);
                intf.l3a_mac_addr[5] = (uint8)p;
                prv = bcm_l3_intf_create(unit, &intf);
                if (prv != BCM_E_NONE) {
                    printf("  port %d: l3_intf_create rv=%d\n", p, prv);
                    continue;
                }

                (void)bcm_port_control_set(unit, p, bcmPortControlIP4, 1);
                (void)bcm_port_control_set(unit, p, bcmPortControlIP6, 1);

                /* ---- control multicast straight to the CPU ------------
                 *
                 * EOS enumerates the control traffic it wants and programs an
                 * L2 multicast entry per address, per internal VLAN, pointing
                 * at a group containing the CPU. Its table on this box holds
                 * exactly six per routed VLAN (docs/EOS-VLAN-STRUCTURE.md):
                 * IPv6 all-nodes, all-routers, OSPFv3 AllSPFRouters and
                 * AllDRouters, and two solicited-node addresses.
                 *
                 * We had been relying on unknown-multicast FLOODING to reach
                 * the CPU. That works -- it is how our OSPF hellos arrived --
                 * but it punts every unknown multicast frame to software and
                 * floods it at every other member of the VLAN. Naming the
                 * addresses is both cheaper and closer to what the hardware is
                 * for.
                 *
                 * IPv4 is included here even though EOS's L2 table has no
                 * 01:00:5e entries: EOS traps IPv4 control traffic with a
                 * field-processor rule on 224/8 instead, which we have not
                 * built. An explicit entry per address gets the same result
                 * with the mechanism we do have. */
                {
                    static const uint8 mcast_macs[][6] = {
                        {0x01,0x00,0x5e,0x00,0x00,0x01}, /* IPv4 all hosts   */
                        {0x01,0x00,0x5e,0x00,0x00,0x02}, /* IPv4 all routers */
                        {0x01,0x00,0x5e,0x00,0x00,0x05}, /* OSPFv2 AllSPF    */
                        {0x01,0x00,0x5e,0x00,0x00,0x06}, /* OSPFv2 AllDR     */
                        {0x33,0x33,0x00,0x00,0x00,0x01}, /* IPv6 all nodes   */
                        {0x33,0x33,0x00,0x00,0x00,0x02}, /* IPv6 all routers */
                        {0x33,0x33,0x00,0x00,0x00,0x05}, /* OSPFv3 AllSPF    */
                        {0x33,0x33,0x00,0x00,0x00,0x06}, /* OSPFv3 AllDR     */
                    };
                    bcm_multicast_t grp = 0;
                    int mi, added = 0;

                    prv = bcm_multicast_create(unit, BCM_MULTICAST_TYPE_L2,
                                               &grp);
                    if (prv != BCM_E_NONE) {
                        printf("  port %d: multicast_create rv=%d %s\n",
                               p, prv, bcm_errmsg(prv));
                    } else {
                        prv = bcm_multicast_egress_add(unit, grp,
                                                       CMIC_PORT(unit), -1);
                        if (prv != BCM_E_NONE) {
                            printf("  port %d: mcast egress_add(cpu) rv=%d %s\n",
                                   p, prv, bcm_errmsg(prv));
                        }
                        for (mi = 0; mi < (int)(sizeof mcast_macs /
                                                sizeof mcast_macs[0]); mi++) {
                            bcm_l2_addr_t m;
                            bcm_l2_addr_t_init(&m, (uint8 *)mcast_macs[mi], vid);
                            m.flags      = BCM_L2_STATIC | BCM_L2_MCAST;
                            m.l2mc_group = grp;
                            if (bcm_l2_addr_add(unit, &m) == BCM_E_NONE) {
                                added++;
                            }
                        }
                        printf("  port %-3d control mcast -> CPU: %d/%d entries "
                               "in vlan %d (group %d)\n",
                               p, added,
                               (int)(sizeof mcast_macs / sizeof mcast_macs[0]),
                               vid, grp);
                    }
                }

                /* Traffic addressed to our own router MAC must be L3
                 * LOOKED UP, not bridged.
                 *
                 * ⚠ This entry was originally BCM_L2_STATIC pointing at the
                 * CPU port, added when there were no routes and the chip was
                 * dropping the peer's ARP replies. It worked -- and then
                 * quietly prevented hardware routing: with 33 routes in the
                 * FIB, 100 transit packets arrived on xe60, all 100 went to
                 * the CPU, and 4 left on xe47. The chip was bridging them up
                 * instead of routing them, because that is exactly what the
                 * entry said to do.
                 *
                 * BCM_L2_L3LOOKUP marks the address as "mine, route it" -- the
                 * my-station entry every router needs. Frames the chip cannot
                 * route still reach the CPU. */
                {
                    bcm_l2_addr_t l2;
                    bcm_l2_addr_t_init(&l2, intf.l3a_mac_addr, vid);
                    l2.port  = CMIC_PORT(unit);
                    l2.flags = BCM_L2_STATIC | BCM_L2_L3LOOKUP;
                    prv = bcm_l2_addr_add(unit, &l2);
                    if (prv != BCM_E_NONE) {
                        printf("  port %d: l2_addr_add(L3LOOKUP) rv=%d %s\n",
                               p, prv, bcm_errmsg(prv));
                    }
                }

                printf("  port %-3d routed: vlan %d, intf %d, mac "
                       "02:1c:73:00:%02x:%02x\n",
                       p, vid, intf.l3a_intf_id, vid >> 8, p);
                /* Remember the last routed interface for FIB sync to build
                 * egress objects against. */
                if (fib_nintf < FIB_MAX_INTF) {
                    fib_intfs[fib_nintf].l3_intf = intf.l3a_intf_id;
                    fib_intfs[fib_nintf].vlan    = vid;
                    fib_intfs[fib_nintf].port    = p;
                    fib_intfs[fib_nintf].ifindex = 0;   /* resolved at fib_start */
                    fib_nintf++;
                }
                nrouted++;
            }
            printf("  %d linked ports made routed interfaces\n", nrouted);
        }
    }

    /* ---- diag commands AFTER port bring-up -----------------------------
     *
     * SDKPOC_CMD runs before 5g/5h, which is right for inspecting a chip
     * before it is touched but useless for anything that needs a configured,
     * linked port -- counters, tx, rx. This runs the same batching after the
     * port is up, which is what a datapath test needs:
     *
     *   SDKPOC_PORTUP=61 SDKPOC_POSTCMD="ps;show counters;tx 10 pbm=xe60"
     *
     * Link status is not a datapath test. A port can hold PCS lock and still
     * forward nothing, so counters before and after a tx are the evidence,
     * not `link=1`.
     */
    /* The front-panel copper LEDs. The SDK never configures them and neither
     * did we, which is the whole reason 48 ports sat dark while forwarding. */
    if (getenv("SDKPOC_PHYBUS")) {
        pthread_t led_tid;
        int lrv = bcm_linkscan_register(unit, led_linkscan_cb);
        printf("  phyled: linkscan handler rv=%d\n", lrv);
        phyled_fix_all(unit);
        if (pthread_create(&led_tid, NULL, phyled_sync_thread,
                           (void *)(intptr_t)unit) == 0) {
            pthread_detach(led_tid);
            printf("  phyled: link sync thread started\n");
        } else {
            printf("  ** phyled: could not start the link sync thread\n");
        }
    }

    if (getenv("SDKPOC_POSTCMD")) {
        STEP("5i. diag shell, after port bring-up");
        /* Bracket the commands with the DMA counters. The totals alone cannot
         * answer "did addtx run during THIS tx" -- 2933 l2p calls across a
         * whole run is dominated by bcm_init. The delta across these two lines
         * is the command's own DMA activity, and a zero l2p delta is the only
         * thing that actually shows dcb19_addtx never ran. */
        printf("bde DMA before POSTCMD:\n  ");
        bde_shim_dma_stats();
        run_diag_cmds(unit, getenv("SDKPOC_POSTCMD"));
        printf("bde DMA after POSTCMD:\n  ");
        bde_shim_dma_stats();
    }

    if (getenv("SDKPOC_RX")) {
        printf("\nRX handler: %ld callbacks, %ld bytes\n",
               rx_cb_calls, rx_cb_bytes);
    }

    /* ---- FIB sync ------------------------------------------------------ */
    if (getenv("SDKPOC_FIBSYNC")) {
        STEP("5n. FIB sync: kernel routes -> chip");
        if (fib_nintf == 0) {
            printf("  ** no routed interface -- run with SDKPOC_PORTMODE=routed\n");
        } else if (fib_start(unit) == 0) {
            printf("  listening on netlink across %d routed interface(s)\n",
                   fib_nintf);
            sal_sleep(3);
            printf("  after initial dump: %ld next hops, %ld routes programmed, "
                   "%ld pending, %ld errors\n",
                   fib_nh_added, fib_routes_added, fib_pending, fib_errors);
        }
    }

    /* ---- platform sensors ----------------------------------------------
     *
     * EOS reports 13 temperature sensors on this box
     * (artifacts/platform-20260816/env-temp.txt): the CPU die, four board
     * sensors on the SCD's SMBus, and eight embedded in the Trident2 itself.
     * The CPU one is already covered by k10temp; the eight inside the ASIC are
     * readable straight from the SDK, which is the cheapest of the three
     * groups to light up and the only one that needs no new bus plumbing.
     *
     *   SDKPOC_SENSORS=1
     */
    if (getenv("SDKPOC_SENSORS")) {
        bcm_switch_temperature_monitor_t temps[16];
        int count = 0, i, prv;

        STEP("5o. ASIC temperature monitors");
        prv = bcm_switch_temperature_monitor_get(unit, 16, temps, &count);
        if (prv != BCM_E_NONE) {
            printf("  bcm_switch_temperature_monitor_get rv=%d %s\n",
                   prv, bcm_errmsg(prv));
        } else {
            printf("  %d monitors (EOS reports 8 Trident sensors)\n", count);
            for (i = 0; i < count; i++) {
                /* The SDK reports in 0.1 degree units. */
                printf("    sensor %-2d  current %3d.%d C   peak %3d.%d C\n",
                       i, temps[i].curr / 10, abs(temps[i].curr % 10),
                       temps[i].peak / 10, abs(temps[i].peak % 10));
            }
        }
    }

    /* ---- stay resident --------------------------------------------------
     *
     * Everything this program has ever proven happened inside one run and then
     * evaporated when it exited: the chip keeps its register state, but
     * linkscan stops, the RX handler is gone, and the next run cold-inits the
     * whole chip again. That makes every result a demo rather than a switch.
     *
     * SDKPOC_DAEMON keeps the process alive with all of it running -- linkscan
     * polling, RX callbacks arriving, ports held up -- and takes commands on a
     * FIFO so the box can be driven without another cold init:
     *
     *     echo 'ps' > /tmp/sdkpoc.cmd
     *     echo 'status' > /tmp/sdkpoc.cmd      built-in: RX counters
     *     echo 'quit' > /tmp/sdkpoc.cmd        exit cleanly
     *
     * A FIFO rather than a socket because the reader is a blocking open/read/
     * close loop in ~20 lines, and the writer is a shell redirect -- no
     * protocol, and it works over the telnet-to-busybox path we already drive
     * the box with. Output goes to this process's stdout, which the caller has
     * redirected to a log.
     *
     *   SDKPOC_DAEMON=1              use /tmp/sdkpoc.cmd
     *   SDKPOC_DAEMON=/path/to/fifo  use that path
     */
    if (getenv("SDKPOC_DAEMON")) {
        const char *d = getenv("SDKPOC_DAEMON");
        const char *fifo = (strcmp(d, "1") == 0) ? "/tmp/sdkpoc.cmd" : d;
        char line[1024];
        int running = 1;

        STEP("7. resident agent");
        (void)unlink(fifo);
        if (mkfifo(fifo, 0666) < 0 && errno != EEXIST) {
            printf("  ** cannot create FIFO %s (errno %d) -- not staying "
                   "resident\n", fifo, errno);
        } else {
            printf("  command FIFO: %s\n", fifo);
            printf("  built-ins: status, quit.  Anything else goes to the "
                   "diag shell.\n");
            printf("  READY\n");
            fflush(stdout);

            while (running) {
                FILE *f = fopen(fifo, "r");   /* blocks until a writer opens */
                if (f == NULL) {
                    sal_sleep(1);
                    continue;
                }
                while (fgets(line, sizeof line, f) != NULL) {
                    char *nl = strchr(line, '\n');
                    if (nl != NULL) {
                        *nl = '\0';
                    }
                    if (line[0] == '\0') {
                        continue;
                    }
                    if (strcmp(line, "quit") == 0) {
                        printf("  quit requested\n");
                        running = 0;
                        break;
                    }
                    if (strncmp(line, "phyreg", 6) == 0 &&
                        (line[6] == '\0' || line[6] == ' ')) {
                        phyreg_cmd(unit, line + 6);
                        continue;
                    }
                    if (strncmp(line, "phyled", 6) == 0 &&
                        (line[6] == '\0' || line[6] == ' ')) {
                        phyled_cmd(unit, line + 6);
                        continue;
                    }
                    if (strcmp(line, "status") == 0) {
                        printf("  RX: %ld callbacks, %ld bytes\n",
                               rx_cb_calls, rx_cb_bytes);
                        printf("  FIB: %ld next hops, %ld hosts, %ld routes, "
                               "%ld pending, %ld errors\n", fib_nh_added,
                               fib_hosts_added, fib_routes_added,
                               fib_pending, fib_errors);
                        bde_shim_dma_stats();
                        fflush(stdout);
                        continue;
                    }
                    run_diag_cmds(unit, line);
                    printf("  --- done: %s\n", line);
                    fflush(stdout);
                }
                fclose(f);
            }
        }
    }

    if (getenv("SDKPOC_PHYBUS")) {
        printf("\nSCD PHY bus: %ld reads, %ld writes, %ld errors\n",
               phybus_rd_calls, phybus_wr_calls, phybus_errs);
    }

    if (ntaps > 0) {
        int i;
        printf("\ntap counters:\n");
        for (i = 0; i < ntaps; i++) {
            printf("  %-6s chip->linux %ld, linux->chip %ld, tx errors %ld\n",
                   taps[i].name, taps[i].rx_to_linux, taps[i].tx_from_linux,
                   taps[i].tx_errors);
        }
    }

    STEP("6. what the SDK thinks it has");
    /* ⚠ The dma_stats call after soc_cm_device_init is a SNAPSHOT of early
     * init, not a total. Reading it as a total is what produced the false
     * "the SDK never allocated packet DMA rings" claim in 9ba1955 -- it said
     * "2 salloc calls, 0 of 64 MB used" while `dma alloc` in the diag shell
     * was handing out addresses 21 MB into the pool. Print it again here so
     * there is a real end-of-run figure to compare against. */
    printf("bde DMA at end of run:\n  ");
    bde_shim_dma_stats();
    printf("soc_attached(%d) = %d\n", unit, soc_attached(unit));
    if (rv == 0) {
        printf("chip: %s\n", soc_dev_name(unit));
        printf("\nIf this reached here, the SDK has run its own cold\n"
               "initialisation on this board. Dump the chip with\n"
               "`scdreset dumpset` now and diff it against our sequence --\n"
               "that difference is the thing six state-copy attempts could\n"
               "not find.\n");
    }
    return rv == 0 ? 0 : 1;
}
