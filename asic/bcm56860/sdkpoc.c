/*
 * Proof of concept: bring the BCM56860 up with the real Broadcom SDK, driven
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
#include <soc/drv.h>
#include <soc/devids.h>
#include <soc/schanmsg.h>
#include <soc/mem.h>
#include <bcm/init.h>
#include <bcm/port.h>
#include <execinfo.h>
#include <soc/error.h>   /* soc_errmsg() is a macro over _SHR_ERRMSG */
#include <sal/appl/config.h>

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

/* tapbridge.c -- switch port <-> Linux tap device */
extern int  tapbridge_start(int unit, const char *ifname, int port);
extern void tapbridge_run(void);

int main(int argc, char *argv[])
{
    uint16_t dev_id = 0;
    uint8_t rev_id = 0;
    int unit, rv;

    COMPILER_REFERENCE(argc);
    COMPILER_REFERENCE(argv);
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("SDK proof of concept -- BCM56860 via a user-space BDE\n");
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
           dev_id == BCM56860_DEVICE_ID ? "(BCM56860, as expected)"
                                        : "** NOT the expected device **");
    if (dev_id != BCM56860_DEVICE_ID) {
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
     * (soc_driver_bcm56860_a0, used for A0 and A1 alike). soc_attach fills
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
            printf("  %-30s valid=%d info.index_max=%d runtime.index_max=%d\n",
                   probe[i].name, SOC_MEM_IS_VALID(unit, m),
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
        STEP("5d. bcm_attach + bcm_init -- the PORT layer");
        /* type MUST be NULL. bcm_attach auto-selects it from the SOC_IS_*
         * macros (control.c:423-476) and falls through to "esw" for this
         * chip. Passing "bcm56860_a0" got BCM_E_CONFIG (-15) on 2026-08-11 --
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
     *   SDKPOC_TAP=<if>     bridge a port to Linux and pump it (never returns)
     */
    if (getenv("SDKPOC_TAP")) {
        /* Put a hardware port on the Linux stack so a routing daemon can use
         * it. Checked BEFORE the shell: this takes over the process. Needs
         * linkscan running or every transmit is silently dropped -- see the
         * note in tapbridge.c. */
        const char *p = getenv("SDKPOC_TAP_PORT");
        printf("\n=== 5f. TAP bridge ===\n");
        if (tapbridge_start(unit, getenv("SDKPOC_TAP"), p ? atoi(p) : 1) == 0) {
            tapbridge_run();
        }
        return 1;
    }
    {
        const char *one = getenv("SDKPOC_CMD");
        if (one || getenv("SDKPOC_SHELL")) {
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
            char buf[512];
            STEP("5e. diag shell, one command");
            snprintf(buf, sizeof buf, "%s", one);
            printf("BCM.0> %s\n", buf);
            sh_process_command(unit, buf);
        } else if (getenv("SDKPOC_SHELL")) {
            STEP("5e. diag shell, interactive -- 'exit' to leave");
            sh_process(unit, "BCM", 1);
        }
    }

    STEP("6. what the SDK thinks it has");
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
