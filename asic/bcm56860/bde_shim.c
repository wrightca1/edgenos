#define _GNU_SOURCE
/*
 * A user-space BDE for the Broadcom SDK, built on the access layer we already
 * have working on this board.
 *
 * WHY THIS EXISTS
 *
 * OpenBCM contains the real `_soc_trident2_mmu_init` and `soc_td2_lls_init` for
 * the BCM56860 -- the initialisation sequence that six increasingly complete
 * state-copy attempts failed to reproduce (docs/REPLAY.md). Rather than port it
 * by hand, run it. The obstacle was always the SDK's BDE, which is a pair of
 * kernel modules targeting kernel <= 5.10 while EdgeNOS runs 6.12.
 *
 * But the BDE's job is small, and we have every piece of it already:
 *
 *   register access   mmap of the ASIC's BAR0        (scdreset does this)
 *   PCI config        /sys/bus/pci/devices/.../config
 *   DMA memory        a physically contiguous region
 *   virt <-> phys     known, because the region is reserved at a fixed address
 *   interrupts        not needed; the SDK polls when none is connected
 *
 * So this file implements soc_cm_device_vectors_t over those and hands it to
 * soc_cm_device_init(). Everything above it is the unmodified SDK.
 *
 * DMA MEMORY
 *
 * The SDK wants a contiguous physical pool (SAL_BDE_DMA_MEM_DEFAULT is 32 MB).
 * The pagemap trick that serves scdreset gives single 4 KB pages, which is fine
 * for one DCB but not for a pool. The clean answer is to reserve a region on
 * the kernel command line -- and we control it, because EdgeNOS is kexec'd:
 *
 *     memmap=64M$0x100000000   (reserve 64 MB at 4 GB)
 *
 * `$` marks the region reserved, so the kernel never touches it, and we map it
 * through /dev/mem. Physical addresses are then simply base + offset, which
 * makes l2p/p2l exact rather than a pagemap lookup.
 *
 * Override with DMA_BASE / DMA_SIZE if the reservation moves.
 */
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <ucontext.h>
#include <execinfo.h>

#include <sal/types.h>
#include <sal/core/libc.h>
#include <soc/cm.h>
#include <soc/cmext.h>
#include <soc/drv.h>      /* SOC_PCI_DEV_TYPE */
#include <soc/devids.h>
#include <pthread.h>

#define ASIC_PCI  "/sys/bus/pci/devices/0000:01:00.0"
/* Do not guess the BAR size. mmap on a sysfs `resource0` refuses anything
 * larger than the region, with EINVAL and no hint -- which is exactly what a
 * hardcoded 1 MB got on the first run. Read the real extent from `resource`,
 * whose first line is "start end flags" for BAR0. */
#define BAR0_SIZE_FALLBACK 0x40000

/* Defaults match the `memmap=64M$0x100000000` suggested above. */
#define DMA_BASE_DEFAULT  0x100000000ULL
#define DMA_SIZE_DEFAULT  (64 * 1024 * 1024)

static volatile uint8_t *bar0;
static size_t   bar0_len;          /* was computed and thrown away; needed to
                                    * bounds-check every MMIO access */
static int   cfg_fd = -1;
static uint8_t *dma_virt;
static uint64_t dma_phys;
static size_t   dma_size;
static size_t   dma_used;

/* ---- instrumentation ----------------------------------------------------
 *
 * Why this exists: our build SIGSEGVs inside soc_attach (reached via
 * soc_cm_device_init, which calls it -- cm.c:7027) while the recovered
 * known-good binary completes the same call on the same chip in the same boot.
 * Chip state, config.bcm, struct ABI and SDK version have all been tested and
 * refuted. What is left is this shim, and the reconstruction is missing
 * whatever the original did here.
 *
 * These accessors dereferenced bar0 + addr with NO bounds check against the
 * 256 KB BAR. One out-of-range offset is a wild pointer and exactly the crash
 * we are chasing. Report it instead of taking the fault.
 */
static long mmio_bad, mmio_shown;
static int  mmio_trace;            /* BDE_TRACE_MMIO=1 logs every access */

#define MMIO_SHOW_MAX 24

static int mmio_out_of_range(const char *what, uint32 addr, size_t width)
{
    if ((size_t)addr + width <= bar0_len) {
        if (mmio_trace) {
            fprintf(stderr, "bde: %s 0x%06x\n", what, addr);
        }
        return 0;
    }
    mmio_bad++;
    if (mmio_shown++ < MMIO_SHOW_MAX) {
        fprintf(stderr, "bde: ** %s 0x%08x is OUTSIDE BAR0 (%zu KB) -- "
                "this is the wild dereference, refusing it\n",
                what, addr, bar0_len >> 10);
        fflush(stderr);
    }
    return 1;
}

long bde_shim_bad_accesses(void) { return mmio_bad; }

/* ---- MMIO trace ---------------------------------------------------------
 *
 * BISECT-WINDOW-NEGATIVE-20260812.md eliminated the entire S-Channel class:
 * replaying all 70,550 ops the SDK issues before its first successful lane
 * access, reads included, does not open the TSC lane window. What is left is
 * the traffic a `soc_schan_op` hook cannot see, and the first candidate is
 * direct MMIO -- the SDK reaches CMIC registers through BAR0.
 *
 * Two things had to be built before that capture was possible, and both were
 * previously recorded as already done:
 *
 * 1. BDE_TRACE_MMIO logs a NAME and an ADDRESS and nothing else -- no data on
 *    a write, no result on a read. A stream with no values cannot be diffed
 *    against our own bring-up, which is the entire purpose.
 *
 * 2. Far worse, it would have captured almost nothing. CMREAD/CMWRITE
 *    (cm.h:207-215) are a RUNTIME TERNARY on base_address: when it is set,
 *    the SDK indexes BAR0 directly and these vectors are never called. And
 *    bde_shim_init sets it by default. So the flag could have been turned on,
 *    produced a nearly empty trace, and been read as "MMIO is not the answer
 *    either" -- eliminating the last candidate on no evidence.
 *
 * Hence BDE_MMIO_LOG forces base_address to 0. That is not a hack: with none
 * of SOC_CM_MEMORY_BASE / SOC_CM_MEMORY / SOC_CM_FUNCTION defined -- and
 * sdk-defines.mk defines none of them -- cm.c:6967 explicitly supports a zero
 * base_address and requires only read/write, which we supply. It is the
 * documented other half of the ternary, not an unsupported mode.
 *
 * It does change how the SDK reaches the chip, so the run is not identical to
 * an untraced one. Treat "the window still opens with base_address=0" as a
 * control that must be checked, not assumed -- if it does not, the trace
 * describes a different run and proves nothing.
 *
 * Records are fixed 16 bytes, appended to a preallocated buffer with no I/O
 * on the hot path: an fprintf per access would both dominate the run and
 * change its timing, and timing is itself hypothesis 3 in that document.
 */
struct mmio_rec {
    uint32_t op;                   /* MMIO_OP_* below */
    uint32_t addr;
    uint64_t data;                 /* value written, or value read back */
};

#define MMIO_OP_READ    0
#define MMIO_OP_WRITE   1
#define MMIO_OP_READ64  2
#define MMIO_OP_WRITE64 3
#define MMIO_OP_CFG_RD  4
#define MMIO_OP_CFG_WR  5

#define MMIO_LOG_MB_DEFAULT 192

static struct mmio_rec *mtrace;
static volatile size_t mtrace_n;       /* reserved slots; may exceed max */
static size_t mtrace_max;
static volatile long mtrace_dropped;
static int    mtrace_fd = -1;

/*
 * THESE HOOKS ARE CALLED FROM MORE THAN ONE THREAD. config.bcm sets
 * polled_irq_mode=1, so the SDK runs its own polling thread
 * (soc_ipoll_connect) from soc_cm_device_init onward, and bcm_init starts
 * more. Both logs therefore reserve their space with an atomic add before
 * touching it.
 *
 * This was not a theory. The first payload capture (2026-08-13) incremented
 * the offset non-atomically, two threads reserved the same bytes, and the
 * log became unparseable 472 records in -- caught only because the reader
 * checks a magic number on every header instead of trusting the framing.
 * The fixed-size MMIO records degrade less visibly under the same race: a
 * torn record still looks like a record, which is worse.
 */
static void mmio_log_record(uint32_t op, uint32_t addr, uint64_t data)
{
    size_t idx;

    if (mtrace == NULL) {
        return;
    }
    idx = __sync_fetch_and_add(&mtrace_n, 1);
    if (idx >= mtrace_max) {
        /* Counted, never silent. A truncated trace that looks complete is how
         * the port-layer capture lost 1,748,658 messages for a day. */
        __sync_fetch_and_add(&mtrace_dropped, 1);
        return;
    }
    mtrace[idx].op = op;
    mtrace[idx].addr = addr;
    mtrace[idx].data = data;
}

/* Async-signal-safe: a raw write of a buffer that already exists. Called both
 * at the end of a run and from fault_report, so a crash still yields the
 * trace -- the same lesson as sdkpoc_trace_flush. */
void bde_shim_mmio_flush(void)
{
    size_t want, done = 0, n = mtrace_n;

    if (mtrace == NULL || mtrace_fd < 0) {
        return;
    }
    if (n > mtrace_max) {          /* reservations run past the cap */
        n = mtrace_max;
    }
    want = n * sizeof(struct mmio_rec);
    while (done < want) {
        ssize_t n = write(mtrace_fd, (const char *)mtrace + done, want - done);
        if (n <= 0) {
            break;
        }
        done += (size_t)n;
    }
    mtrace_n = 0;                  /* flushed; a second call must not duplicate */
}

void bde_shim_mmio_stats(void)
{
    if (mtrace == NULL) {
        return;
    }
    printf("bde: mmio trace %zu records, %ld dropped (cap %zu)\n",
           mtrace_n, mtrace_dropped, mtrace_max);
    if (mtrace_dropped) {
        printf("bde: ** the MMIO trace is TRUNCATED -- raise BDE_MMIO_LOG_MB\n");
    }
}

/* ---- SBUS DMA payload capture -------------------------------------------
 *
 * The MMIO trace records the DESCRIPTORS of every SBUS DMA transfer but not
 * their CONTENTS -- the payload sits in host memory. Since SBUS DMA writes
 * roughly an order of magnitude more chip state than the S-Channel
 * (MMIO-CAPTURED-20260813.md), that payload is most of what a replay is
 * missing, and it is the input to the precondition experiment: replay the
 * pre-window DMA entries first, THEN the S-Channel bisect.
 *
 * Register layout is the SDK's, not inferred (include/soc/cmicm.h:229-260):
 *
 *   CMCx + 0x600 + ch*0x50   CONTROL   bit0 START, bit1 ABORT, bit2 MODE
 *                    +0x04   REQUEST
 *                    +0x08   COUNT       entries
 *                    +0x0c   OPCODE      SAME layout as an S-Channel header
 *                    +0x10   SBUS_START
 *                    +0x14   HOSTMEM_START
 *                    +0x18   DESC_START  (descriptor mode only)
 *
 * THERE ARE THREE CHANNELS, not two (CMIC_CMCx_SBUSDMA_CHAN_MAX). The first
 * analysis of the 08-13 trace decoded only ch0 and ch1 and silently missed
 * ch2's 1,638 transfers, every one of them in descriptor mode.
 *
 * Two things follow from OPCODE being an S-Channel header:
 *
 *   - Direction is bits [31:26]. Only WRITE_MEMORY/WRITE_REGISTER carry state
 *     INTO the chip; a READ_MEMORY transfer's buffer is filled by the chip
 *     AFTER the start, so snapshotting it here would capture stale bytes.
 *     Reads are therefore recorded as headers with no payload rather than
 *     with a payload that would be quietly wrong.
 *   - Entry width is the dlen field, bits [13:7], so payload bytes =
 *     COUNT * dlen. Checked against the microcode transfers: dlen 16 x 1984
 *     entries = 31,744 bytes, which is the size of the TSC 8051 image.
 */
struct sbd_hdr {
    uint32_t magic;                /* 'S','B','D','1' */
    uint32_t seq;
    uint32_t cmc, ch;
    uint32_t control, opcode, count;
    uint32_t sbus_addr, hostmem, descaddr;
    uint32_t bytes;                /* payload bytes that FOLLOW this header */
    uint32_t flags;                /* SBD_F_* */
};

#define SBD_MAGIC     0x31444253u  /* "SBD1" little-endian */
#define SBD_F_WRITE   0x1          /* carries state into the chip */
#define SBD_F_DESC    0x2          /* descriptor mode; bytes are the CHAIN */
#define SBD_F_TRUNC   0x4          /* payload longer than the per-xfer cap */
#define SBD_F_NOMAP   0x8          /* host address outside our DMA pool */

#define SBD_DESC_DUMP_DEFAULT 8192 /* descriptor-mode chain bytes to grab */

static uint8_t *sbd_buf;
static volatile size_t sbd_n;          /* reserved bytes; may exceed max */
static size_t   sbd_max, sbd_desc_dump;
static volatile long sbd_xfers, sbd_dropped, sbd_seq;
static int      sbd_fd = -1;

/*
 * Header and payload must be reserved as ONE unit, or a concurrent transfer
 * lands between them and the framing is lost -- which is exactly how the
 * first capture corrupted itself. See the note on mmio_log_record.
 */
static void sbd_emit(const struct sbd_hdr *h, const void *payload, size_t len)
{
    size_t total = sizeof(*h) + len, off;

    off = __sync_fetch_and_add(&sbd_n, total);
    if (off + total > sbd_max) {
        __sync_fetch_and_add(&sbd_dropped, 1);
        return;
    }
    memcpy(sbd_buf + off, h, sizeof(*h));
    if (len) {
        memcpy(sbd_buf + off + sizeof(*h), payload, len);
    }
}

/*
 * Called from shim_write when CONTROL is written with START set. `d` holds
 * the descriptor registers as last written to this channel.
 */
static void sbd_record(int cmc, int ch, uint32_t control, const uint32_t *d)
{
    struct sbd_hdr h;
    uint32_t opcode = d[0x0c / 4], count = d[0x08 / 4];
    uint32_t opc = (opcode >> 26) & 0x3f;
    uint32_t dlen = (opcode >> 7) & 0x7f;
    const void *src = NULL;
    size_t bytes = 0;

    if (sbd_buf == NULL) {
        return;
    }
    __sync_fetch_and_add(&sbd_xfers, 1);
    memset(&h, 0, sizeof(h));
    h.magic = SBD_MAGIC;
    h.seq = (uint32_t)__sync_fetch_and_add(&sbd_seq, 1);
    h.cmc = (uint32_t)cmc;
    h.ch = (uint32_t)ch;
    h.control = control;
    h.opcode = opcode;
    h.count = count;
    h.sbus_addr = d[0x10 / 4];
    h.hostmem = d[0x14 / 4];
    h.descaddr = d[0x18 / 4];

    if (control & 0x4) {
        /* Descriptor mode: the transfer is a chain in host memory, so COUNT
         * is meaningless and the real payload is reachable only by walking
         * it. Grab a bounded window and decode offline -- and SAY so, rather
         * than record a zero-length payload that reads like "nothing here". */
        h.flags |= SBD_F_DESC;
        src = (h.descaddr >= dma_phys && h.descaddr < dma_phys + dma_size)
                  ? dma_virt + (h.descaddr - dma_phys) : NULL;
        bytes = sbd_desc_dump;
    } else if (opc == 0x09 || opc == 0x0d) {
        h.flags |= SBD_F_WRITE;
        src = (h.hostmem >= dma_phys && h.hostmem < dma_phys + dma_size)
                  ? dma_virt + (h.hostmem - dma_phys) : NULL;
        bytes = (size_t)count * (dlen ? dlen : 4);
    }

    if (bytes && src == NULL) {
        h.flags |= SBD_F_NOMAP;
        bytes = 0;
    }
    if (src != NULL) {
        size_t avail = dma_size - ((const uint8_t *)src - dma_virt);
        if (bytes > avail) {
            bytes = avail;
            h.flags |= SBD_F_TRUNC;
        }
    }
    h.bytes = (uint32_t)bytes;
    sbd_emit(&h, src, bytes);
}

void bde_shim_sbd_flush(void)
{
    size_t done = 0, n = sbd_n;

    if (sbd_buf == NULL || sbd_fd < 0) {
        return;
    }
    if (n > sbd_max) {
        n = sbd_max;
    }
    while (done < n) {
        ssize_t w = write(sbd_fd, (const char *)sbd_buf + done, n - done);
        if (w <= 0) {
            break;
        }
        done += (size_t)w;
    }
    sbd_n = 0;
}

void bde_shim_sbd_stats(void)
{
    if (sbd_buf == NULL) {
        return;
    }
    printf("bde: sbusdma %ld transfers, %zu bytes buffered, %ld dropped\n",
           sbd_xfers, sbd_n, sbd_dropped);
    if (sbd_dropped) {
        printf("bde: ** the SBUS DMA payload log is TRUNCATED -- raise "
               "BDE_SBUSDMA_LOG_MB\n");
    }
}

static void sbd_setup(void)
{
    const char *path = getenv("BDE_SBUSDMA_LOG");
    const char *mb = getenv("BDE_SBUSDMA_LOG_MB");
    const char *dd = getenv("BDE_SBUSDMA_DESC_BYTES");
    size_t bytes;

    if (path == NULL) {
        return;
    }
    sbd_desc_dump = dd ? (size_t)atol(dd) : SBD_DESC_DUMP_DEFAULT;
    bytes = (size_t)(mb ? atol(mb) : 256) * 1024 * 1024;
    sbd_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sbd_fd < 0) {
        perror("bde: open BDE_SBUSDMA_LOG");
        return;
    }
    sbd_buf = malloc(bytes);
    if (sbd_buf == NULL) {
        printf("bde: ** cannot allocate a %zu MB SBUS DMA buffer\n",
               bytes >> 20);
        close(sbd_fd);
        sbd_fd = -1;
        return;
    }
    sbd_max = bytes;
    printf("bde: SBUS DMA payloads -> %s (%zu MB, %zu-byte descriptor dumps)\n",
           path, bytes >> 20, sbd_desc_dump);
}

/*
 * Shadow of the three channels' descriptor registers per CMC, updated on
 * every write so that a START has the full descriptor available.
 */
#define SBD_CMCS 3
#define SBD_CHS  3
static uint32_t sbd_shadow[SBD_CMCS][SBD_CHS][0x50 / 4];

static void sbd_watch_write(uint32 addr, uint32 data)
{
    int cmc, ch;

    if (sbd_buf == NULL) {
        return;
    }
    for (cmc = 0; cmc < SBD_CMCS; cmc++) {
        uint32_t base = 0x031000 + (uint32_t)cmc * 0x1000 + 0x600;
        if (addr < base || addr >= base + SBD_CHS * 0x50) {
            continue;
        }
        ch = (int)((addr - base) / 0x50);
        {
            uint32_t off = (addr - base) % 0x50;
            if (off == 0 && (data & 0x1)) {
                sbd_record(cmc, ch, data, sbd_shadow[cmc][ch]);
            } else {
                sbd_shadow[cmc][ch][off / 4] = data;
            }
        }
        return;
    }
}

static void mmio_log_setup(void)
{
    const char *path = getenv("BDE_MMIO_LOG");
    const char *mb = getenv("BDE_MMIO_LOG_MB");
    size_t bytes;

    if (path == NULL) {
        return;
    }
    bytes = (size_t)(mb ? atol(mb) : MMIO_LOG_MB_DEFAULT) * 1024 * 1024;
    mtrace_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (mtrace_fd < 0) {
        perror("bde: open BDE_MMIO_LOG");
        return;
    }
    mtrace = malloc(bytes);
    if (mtrace == NULL) {
        printf("bde: ** cannot allocate a %zu MB MMIO trace buffer -- "
               "lower BDE_MMIO_LOG_MB\n", bytes >> 20);
        close(mtrace_fd);
        mtrace_fd = -1;
        return;
    }
    mtrace_max = bytes / sizeof(struct mmio_rec);
    printf("bde: MMIO trace -> %s (%zu MB, %zu records)\n",
           path, bytes >> 20, mtrace_max);
}

/*
 * The SDK is also handed base_address, and where that is set it addresses
 * registers DIRECTLY rather than through these vectors -- so a bad offset
 * faults inside SDK code where no check of ours can see it. This handler is
 * what catches that path: it reports where the faulting address sits relative
 * to the two mappings, which says immediately whether the SDK ran off the end
 * of BAR0 or off the end of the DMA pool.
 */
/* sdkpoc owns the trace FILE*. _exit() in the handler skips stdio flushing, so
 * the B1 run at 03:50 died with an EMPTY trace -- the one run whose trace we
 * most needed. Weak so bde_shim still links without sdkpoc. */
extern void sdkpoc_trace_flush(void) __attribute__((weak));

static void fault_report(int sig, siginfo_t *si, void *ctx)
{
    char buf[320];
    const uint8_t *a = (const uint8_t *)si->si_addr;
    int n;

    if (bar0 && a >= (const uint8_t *)bar0 &&
        a < (const uint8_t *)bar0 + (bar0_len << 4)) {
        n = snprintf(buf, sizeof(buf),
                     "\nbde: FAULT sig %d at %p = BAR0 + 0x%lx  (BAR0 is %zu KB, "
                     "so this is %s the end)\n", sig, si->si_addr,
                     (unsigned long)(a - (const uint8_t *)bar0), bar0_len >> 10,
                     (size_t)(a - (const uint8_t *)bar0) < bar0_len
                         ? "INSIDE" : "PAST");
    } else if (dma_virt && a >= dma_virt && a < dma_virt + (dma_size << 4)) {
        n = snprintf(buf, sizeof(buf),
                     "\nbde: FAULT sig %d at %p = DMA + 0x%lx  (pool is %zu MB, "
                     "so this is %s the end)\n", sig, si->si_addr,
                     (unsigned long)(a - dma_virt), dma_size >> 20,
                     (size_t)(a - dma_virt) < dma_size ? "INSIDE" : "PAST");
    } else {
        n = snprintf(buf, sizeof(buf),
                     "\nbde: FAULT sig %d at %p -- not in BAR0 (%p) nor the DMA "
                     "pool (%p), so it is an ordinary bad pointer\n",
                     sig, si->si_addr, (void *)bar0, (void *)dma_virt);
    }
    if (n > 0) {
        ssize_t ignored = write(2, buf, (size_t)n);
        COMPILER_REFERENCE(ignored);
    }
    /* The faulting ADDRESS was not enough: 0xffffffff says only "a bad value
     * was used as a pointer", not who did it. RIP names the instruction, and
     * addr2line against the unstripped build turns it into file:line. */
    {
        ucontext_t *uc = (ucontext_t *)ctx;
        if (uc != NULL) {
            n = snprintf(buf, sizeof(buf),
                         "bde: FAULT rip=0x%llx  -- resolve with:\n"
                         "bde:   addr2line -fpe tools/sdkshim/sdkpoc.dbg 0x%llx\n",
                         (unsigned long long)uc->uc_mcontext.gregs[REG_RIP],
                         (unsigned long long)uc->uc_mcontext.gregs[REG_RIP]);
            if (n > 0) {
                ssize_t ig2 = write(2, buf, (size_t)n);
                COMPILER_REFERENCE(ig2);
            }
        }
    }
    /* RIP named _shr_ctoi (util.c:823) -- it dereferences its argument
     * immediately, so it was handed 0xffffffff as a char*. Which caller did
     * that is the actual question, so print the return-address chain and
     * resolve it offline. backtrace() is not strictly async-signal-safe, but
     * the process is already dead. */
    {
        void *bt[24];
        int k = backtrace(bt, 24), i;

        n = snprintf(buf, sizeof(buf), "bde: backtrace (%d frames):\n", k);
        if (n > 0) { ssize_t g = write(2, buf, (size_t)n); COMPILER_REFERENCE(g); }
        for (i = 0; i < k; i++) {
            n = snprintf(buf, sizeof(buf), "bde:   #%-2d 0x%llx\n",
                         i, (unsigned long long)(uintptr_t)bt[i]);
            if (n > 0) { ssize_t g = write(2, buf, (size_t)n); COMPILER_REFERENCE(g); }
        }
    }
    if (sdkpoc_trace_flush) {
        sdkpoc_trace_flush();
    }
    bde_shim_mmio_flush();
    bde_shim_sbd_flush();
    _exit(139);
}

static void fault_setup(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fault_report;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

/* ---- register access ---------------------------------------------------- */

static uint32 shim_read(soc_cm_dev_t *dev, uint32 addr)
{
    uint32_t v;

    COMPILER_REFERENCE(dev);
    if (mmio_out_of_range("read", addr, 4)) return 0xffffffff;
    v = *(volatile uint32_t *)(bar0 + addr);
    mmio_log_record(MMIO_OP_READ, addr, v);
    return v;
}

static void shim_write(soc_cm_dev_t *dev, uint32 addr, uint32 data)
{
    COMPILER_REFERENCE(dev);
    if (mmio_out_of_range("write", addr, 4)) return;
    mmio_log_record(MMIO_OP_WRITE, addr, data);
    /* BEFORE the store: on a START this snapshots the host buffer while the
     * chip has certainly not begun consuming it. Doing it after would race
     * the DMA engine for exactly the bytes we came for. */
    sbd_watch_write(addr, data);
    *(volatile uint32_t *)(bar0 + addr) = data;
}

static uint64 shim_read64(soc_cm_dev_t *dev, uint32 addr)
{
    uint64_t v;

    COMPILER_REFERENCE(dev);
    if (mmio_out_of_range("read64", addr, 8)) return (uint64)-1;
    v = *(volatile uint64_t *)(bar0 + addr);
    mmio_log_record(MMIO_OP_READ64, addr, v);
    return v;
}

static void shim_write64(soc_cm_dev_t *dev, uint32 addr, uint64 data)
{
    COMPILER_REFERENCE(dev);
    if (mmio_out_of_range("write64", addr, 8)) return;
    mmio_log_record(MMIO_OP_WRITE64, addr, data);
    *(volatile uint64_t *)(bar0 + addr) = data;
}

/* ---- PCI config --------------------------------------------------------- */

static uint32 shim_pci_conf_read(soc_cm_dev_t *dev, uint32 addr)
{
    uint32_t v = 0xffffffff;
    COMPILER_REFERENCE(dev);
    if (pread(cfg_fd, &v, 4, addr) != 4) v = 0xffffffff;
    mmio_log_record(MMIO_OP_CFG_RD, addr, v);
    return v;
}

static void shim_pci_conf_write(soc_cm_dev_t *dev, uint32 addr, uint32 data)
{
    COMPILER_REFERENCE(dev);
    /* In the same stream as the BAR0 accesses, and deliberately so: config
     * space is the other thing our own bring-up drives (`pcicfg`), and if the
     * enabling action is there we would rather see it than have to guess
     * which of two traces to look in. */
    mmio_log_record(MMIO_OP_CFG_WR, addr, data);
    if (pwrite(cfg_fd, &data, 4, addr) != 4)
        fprintf(stderr, "bde: pci config write 0x%x failed\n", addr);
}

/* ---- DMA memory --------------------------------------------------------- */

/*
 * A bump allocator WITH REUSE.
 *
 * WHY THIS IS NOT STILL A PURE BUMP ALLOCATOR
 * -------------------------------------------
 * It used to be, with `sfree` as a documented no-op, on the reasoning that the
 * SDK "frees very little during init and never in a pattern that would
 * fragment". That is true of init. It is not true of a switch that stays up.
 *
 * Twice now a caller that allocates periodically has walked the pool to zero:
 *
 *   Aug  bcm_pkt_alloc per transmitted frame -- transmit died after ~2400
 *        packets and an OSPF adjacency fell back to Init
 *   now  the SDK's field-processor counter collection, 6144 bytes per cycle --
 *        both adjacencies died after hours, while every process stayed up,
 *        every link stayed up and the LEDs stayed correctly lit
 *
 * Each was fixed by changing the caller: reuse one packet, stop collecting the
 * counter. Both were workarounds for this function, and the second one was
 * foreseeable from the first. Any future caller that allocates on a timer would
 * be the third.
 *
 * So: `sfree` now actually frees, and `salloc` reuses. The pattern that hurts
 * is repeated alloc/free of the SAME sizes, so an exact-size free list reclaims
 * essentially all of it -- no splitting, no coalescing, no fragmentation, and
 * no way for a block to come back at a different address or size than the
 * caller had. Blocks are never returned to the bump region, which keeps every
 * DMA address stable for the life of the process.
 *
 * The pointer handed out is unchanged: these are DMA buffers whose physical
 * address is computed from the offset into the pool, so a header before the
 * block would move the DMA address. Sizes live in a side table instead.
 *
 * Locked, because unlike init this now runs from several SDK threads at once --
 * counter collection, linkscan and rx all allocate.
 */
#define DMA_MAX_BLOCKS 8192

static struct dma_blk {
    void   *p;
    size_t  sz;
    int     freed;
} dma_blk[DMA_MAX_BLOCKS];
static int  dma_nblk;
static long salloc_calls, salloc_reused, sfree_calls, sfree_unknown;
static pthread_mutex_t dma_lock = PTHREAD_MUTEX_INITIALIZER;

static void *shim_salloc(soc_cm_dev_t *dev, int size, const char *name)
{
    size_t need;
    void *p;

    COMPILER_REFERENCE(dev);
    /* A negative or absurd size means the SDK computed it from something we
     * got wrong; rounding it up would hand back a pointer into nowhere. */
    if (size <= 0) {
        fprintf(stderr, "bde: ** salloc(%d, %s) has a non-positive size\n",
                size, name ? name : "?");
        return NULL;
    }
    need = ((size_t)size + 4095) & ~4095UL;

    pthread_mutex_lock(&dma_lock);
    salloc_calls++;

    /* Reuse before growing. Exact size only -- see the note above. */
    {
        int i;
        for (i = 0; i < dma_nblk; i++) {
            if (dma_blk[i].freed && dma_blk[i].sz == need) {
                dma_blk[i].freed = 0;
                p = dma_blk[i].p;
                salloc_reused++;
                pthread_mutex_unlock(&dma_lock);
                sal_memset(p, 0, need);
                if (mmio_trace) {
                    fprintf(stderr, "bde: salloc %8d -> %p (%s) REUSED\n",
                            size, p, name ? name : "?");
                }
                return p;
            }
        }
    }

    if (dma_used + need > dma_size) {
        fprintf(stderr, "bde: salloc(%d, %s) exhausted the %zu MB pool "
                "(used %zu MB, %ld blocks, %ld reused) -- raise the memmap= "
                "reservation\n", size, name ? name : "?", dma_size >> 20,
                dma_used >> 20, (long)dma_nblk, salloc_reused);
        pthread_mutex_unlock(&dma_lock);
        return NULL;
    }
    p = dma_virt + dma_used;
    dma_used += need;

    /* A block we cannot record is a block sfree can never reclaim, so say so
     * once rather than silently reverting to the old leaking behaviour. */
    if (dma_nblk < DMA_MAX_BLOCKS) {
        dma_blk[dma_nblk].p = p;
        dma_blk[dma_nblk].sz = need;
        dma_blk[dma_nblk].freed = 0;
        dma_nblk++;
    } else {
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "bde: ** more than %d DMA blocks; further ones "
                    "cannot be reclaimed\n", DMA_MAX_BLOCKS);
        }
    }
    pthread_mutex_unlock(&dma_lock);
    sal_memset(p, 0, need);
    if (mmio_trace) {
        fprintf(stderr, "bde: salloc %8d -> %p (%s), pool %zu/%zu MB\n",
                size, p, name ? name : "?", dma_used >> 20, dma_size >> 20);
    }
    return p;
}

void bde_shim_dma_stats(void)
{
    int i, live = 0;
    for (i = 0; i < dma_nblk; i++) if (!dma_blk[i].freed) live++;
    printf("bde: %ld salloc (%ld reused), %ld sfree (%ld not ours), "
           "%d blocks %d live, %zu of %zu MB used, %ld out-of-range MMIO\n",
           salloc_calls, salloc_reused, sfree_calls, sfree_unknown,
           dma_nblk, live, dma_used >> 20, dma_size >> 20, mmio_bad);
}

static void shim_sfree(soc_cm_dev_t *dev, void *ptr)
{
    int i;

    COMPILER_REFERENCE(dev);
    if (ptr == NULL) return;

    pthread_mutex_lock(&dma_lock);
    sfree_calls++;
    for (i = 0; i < dma_nblk; i++) {
        if (dma_blk[i].p == ptr) {
            /* A double free is the SDK telling us something we got wrong, and
             * silently tolerating it would let the same block be handed to two
             * owners. Refuse it and say so. */
            if (dma_blk[i].freed) {
                fprintf(stderr, "bde: ** double sfree of %p, ignored\n", ptr);
            } else {
                dma_blk[i].freed = 1;
            }
            pthread_mutex_unlock(&dma_lock);
            return;
        }
    }
    /* Not ours: the SDK frees plain heap pointers through this path too. */
    sfree_unknown++;
    pthread_mutex_unlock(&dma_lock);
}

/* x86 DMA is coherent; there is nothing to flush or invalidate. Returning 0
 * (success) is correct here, not a stub that hides a missing operation. */
static int shim_sflush(soc_cm_dev_t *dev, void *addr, int length)
{
    COMPILER_REFERENCE(dev); COMPILER_REFERENCE(addr); COMPILER_REFERENCE(length);
    return 0;
}

static int shim_sinval(soc_cm_dev_t *dev, void *addr, int length)
{
    COMPILER_REFERENCE(dev); COMPILER_REFERENCE(addr); COMPILER_REFERENCE(length);
    return 0;
}

static sal_paddr_t shim_l2p(soc_cm_dev_t *dev, void *addr)
{
    COMPILER_REFERENCE(dev);
    if ((uint8_t *)addr < dma_virt || (uint8_t *)addr >= dma_virt + dma_size) {
        fprintf(stderr, "bde: l2p(%p) is outside the DMA pool\n", addr);
        return 0;
    }
    return (sal_paddr_t)(dma_phys + ((uint8_t *)addr - dma_virt));
}

static void *shim_p2l(soc_cm_dev_t *dev, sal_paddr_t addr)
{
    COMPILER_REFERENCE(dev);
    if (addr < dma_phys || addr >= dma_phys + dma_size) {
        fprintf(stderr, "bde: p2l(0x%llx) is outside the DMA pool\n",
                (unsigned long long)addr);
        return NULL;
    }
    return dma_virt + (addr - dma_phys);
}

/* ---- config properties --------------------------------------------------
 *
 * THIS is how the SDK reads config.bcm. soc_property_get_str (drv.c:682)
 * consults sal_config_get ONLY for the pseudo-unit SOC_UNIT_NONE_SAL_CONFIG
 * (drv.c:864); for a real unit it ends at soc_cm_config_var_get, which calls
 * CMVEC(dev).config_var_get. We never supplied that vector, so every property
 * lookup for unit 0 -- including polled_irq_mode -- came back empty no matter
 * what sdkpoc.c pushed into the SAL table. The recovered known-good binary
 * prints "bde: 274 config properties from /config.bcm" because its shim
 * implements exactly this.
 *
 * The crash chain that led here: soc_attach (drv.c:21326) ->
 * soc_property_get(unit, spn_RCPU_ONLY, 0) -> _shr_ctoi (util.c:823), which
 * dereferences its argument immediately.
 */
typedef struct cfg_ent_s {
    char *name;
    char *value;
    struct cfg_ent_s *next;
} cfg_ent_t;

static cfg_ent_t *cfg_list;
static int        cfg_count;
static int        cfg_trace;
static long       cfg_hits, cfg_misses;

static void cfg_load(const char *path)
{
    char line[512];
    FILE *f = fopen(path, "r");

    if (f == NULL) {
        fprintf(stderr, "bde: cannot open %s\n", path);
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *key = line, *val, *end;
        cfg_ent_t *e;

        while (*key == ' ' || *key == '\t') key++;
        if (*key == '#' || *key == '\n' || *key == '\r' || *key == '\0') continue;
        if ((val = strchr(key, '=')) == NULL) continue;
        *val++ = '\0';
        for (end = val - 2; end >= key && (*end == ' ' || *end == '\t'); end--)
            *end = '\0';
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
                            *end == ' '  || *end == '\t'); end--)
            *end = '\0';
        if (*key == '\0') continue;
        if ((e = malloc(sizeof(*e))) == NULL) break;
        /* Own the strings: `line` is reused every iteration, so storing
         * pointers into it would hand the SDK dangling memory. */
        e->name = strdup(key);
        e->value = strdup(val);
        if (e->name == NULL || e->value == NULL) { free(e); break; }
        e->next = cfg_list;
        cfg_list = e;
        cfg_count++;
    }
    fclose(f);
    printf("bde: %d config properties from %s\n", cfg_count, path);
}

static char *shim_config_var_get(soc_cm_dev_t *dev, const char *name)
{
    cfg_ent_t *e;

    COMPILER_REFERENCE(dev);
    if (name == NULL) return NULL;
    for (e = cfg_list; e != NULL; e = e->next) {
        if (strcmp(e->name, name) == 0) {
            cfg_hits++;
            if (cfg_trace) fprintf(stderr, "bde: cfg %s = %s\n", name, e->value);
            return e->value;
        }
    }
    cfg_misses++;
    if (cfg_trace) fprintf(stderr, "bde: cfg %s = <unset>\n", name);
    /* NULL is the documented "not set"; soc_property_get then uses its
     * default. Returning anything else is what put 0xffffffff into
     * _shr_ctoi. */
    return NULL;
}

void bde_shim_config_stats(void)
{
    printf("bde: config lookups: %ld hit, %ld unset, %d loaded\n",
           cfg_hits, cfg_misses, cfg_count);
}

/* ---- interrupts --------------------------------------------------------- */

/*
 * Not connected. The SDK falls back to polling for the operations we need
 * (S-Channel, table DMA, packet DMA), and wiring MSI from user space would add
 * a kernel dependency this whole exercise exists to avoid. Returning failure
 * rather than pretending to succeed keeps the SDK honest about it.
 */
static int shim_interrupt_connect(soc_cm_dev_t *dev,
                                  soc_cm_isr_func_t handler, void *data)
{
    COMPILER_REFERENCE(dev); COMPILER_REFERENCE(handler); COMPILER_REFERENCE(data);
    return -1;
}

static int shim_interrupt_disconnect(soc_cm_dev_t *dev)
{
    COMPILER_REFERENCE(dev);
    return -1;
}

/* ---- setup -------------------------------------------------------------- */

static size_t bar0_size(void)
{
    unsigned long long start = 0, end = 0, flags = 0;
    size_t sz = BAR0_SIZE_FALLBACK;
    FILE *f = fopen(ASIC_PCI "/resource", "r");

    if (f) {
        if (fscanf(f, "%llx %llx %llx", &start, &end, &flags) == 3 && end > start)
            sz = (size_t)(end - start + 1);
        fclose(f);
    }
    return sz;
}

static int map_bar0(void)
{
    size_t sz = bar0_size();
    int fd = open(ASIC_PCI "/resource0", O_RDWR | O_SYNC);

    if (fd < 0) { perror("open resource0"); return -1; }
    bar0 = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (bar0 == MAP_FAILED) {
        fprintf(stderr, "bde: mmap of %zu bytes of BAR0 failed: %s\n",
                sz, strerror(errno));
        return -1;
    }
    bar0_len = sz;
    printf("bde: BAR0 = %zu KB\n", sz >> 10);
    return 0;
}

static int map_dma(void)
{
    const char *e;
    int fd;

    dma_phys = (e = getenv("DMA_BASE")) ? strtoull(e, NULL, 0) : DMA_BASE_DEFAULT;
    dma_size = (e = getenv("DMA_SIZE")) ? strtoull(e, NULL, 0) : DMA_SIZE_DEFAULT;

    if ((fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0) {
        perror("open /dev/mem");
        return -1;
    }
    dma_virt = mmap(NULL, dma_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    fd, (off_t)dma_phys);
    close(fd);
    if (dma_virt == MAP_FAILED) {
        perror("mmap DMA pool");
        fprintf(stderr, "bde: is the region reserved? add "
                "memmap=%zuM$0x%llx to the kernel command line\n",
                dma_size >> 20, (unsigned long long)dma_phys);
        return -1;
    }
    /* Prove the mapping before the chip is told to DMA into it. A silent bad
     * mapping here looks exactly like a dead chip later. */
    *(volatile uint32_t *)dma_virt = 0xa5a5a5a5;
    if (*(volatile uint32_t *)dma_virt != 0xa5a5a5a5) {
        fprintf(stderr, "bde: DMA pool at 0x%llx does not read back\n",
                (unsigned long long)dma_phys);
        return -1;
    }
    memset(dma_virt, 0, 4096);
    return 0;
}

/* ---- SAL DMA hooks ------------------------------------------------------
 *
 * The platform builds with -DLINUX_SAL_DMA_ALLOC_OVERRIDE, which renames the
 * SAL's own sal_dma_alloc/free to sal_sim_dma_* and leaves the real names for
 * the BDE to supply (src/sal/core/unix/alloc.c:11, and the reference
 * implementation in systems/bde/linux/user/linux-user-bde.c:2812). Without
 * these the link fails, or worse, resolves to a weak stub and DMA silently
 * lands in ordinary heap memory the chip cannot reach.
 */
void *sal_dma_alloc(unsigned int sz, char *name)
{
    return shim_salloc(NULL, (int)sz, name);
}

void sal_dma_free(void *ptr)
{
    shim_sfree(NULL, ptr);
}

/* sal_dma_flush/inval/vtop/ptov are NOT defined here: LINUX_SAL_DMA_ALLOC_OVERRIDE
 * renames only alloc and free, and libsal_core_plat.a still exports the other
 * four. Defining them again is a multiple-definition link error. */

soc_cm_device_vectors_t bde_shim_vectors = {
    /* bus_type tells the SDK this is a PCI device, which is what makes it use
     * pci_conf_read/write for config space rather than assuming memory-mapped
     * config. Leaving it 0 was the first run's `soc_cm_device_init rv=-4`.
     *
     * base_address is filled in at init time with the mapped BAR0 -- it cannot
     * be a static initialiser because mmap has not run yet. Some builds
     * REQUIRE it; where it is set, the SDK can address registers directly
     * instead of through read/write, and both remain wired either way.
     */
    .bus_type = SOC_PCI_DEV_TYPE,
    .interrupt_connect    = shim_interrupt_connect,
    .interrupt_disconnect = shim_interrupt_disconnect,
    .read                 = shim_read,
    .write                = shim_write,
    .config_var_get       = shim_config_var_get,
    .pci_conf_read        = shim_pci_conf_read,
    .pci_conf_write       = shim_pci_conf_write,
    .salloc               = shim_salloc,
    .sfree                = shim_sfree,
    .sflush               = shim_sflush,
    .sinval               = shim_sinval,
    .l2p                  = shim_l2p,
    .p2l                  = shim_p2l,
    .read64               = shim_read64,
    .write64              = shim_write64,
};

int bde_shim_init(void)
{
    if ((cfg_fd = open(ASIC_PCI "/config", O_RDWR)) < 0) {
        perror("open pci config");
        return -1;
    }
    mmio_trace = getenv("BDE_TRACE_MMIO") != NULL;
    cfg_trace  = getenv("BDE_TRACE_CONFIG") != NULL;
    if (getenv("SDKPOC_CONFIG")) cfg_load(getenv("SDKPOC_CONFIG"));
    mmio_log_setup();
    sbd_setup();
    fault_setup();
    if (map_bar0() < 0) return -1;
    if (map_dma() < 0) return -1;

    /* base_address lets the SDK bypass shim_read/shim_write and address BAR0
     * directly -- which also bypasses the bounds check that would name a bad
     * offset. BDE_NO_BASE_ADDRESS=1 forces every access back through the
     * vectors, turning the segfault into a printed address. */
    /* BDE_SBUSDMA_LOG needs this as much as BDE_MMIO_LOG does: the capture
     * hangs off shim_write, which the SDK only calls when base_address is 0. */
    if (getenv("BDE_NO_BASE_ADDRESS") || mtrace != NULL || sbd_buf != NULL) {
        bde_shim_vectors.base_address = 0;
        printf("bde: base_address left 0 -- all MMIO forced through the "
               "bounds-checked vectors%s\n",
               (mtrace != NULL || sbd_buf != NULL)
                   ? " (REQUIRED by BDE_MMIO_LOG/BDE_SBUSDMA_LOG: with"
                     " base_address set, CMREAD/CMWRITE index BAR0"
                     " directly and the trace would be empty)" : "");
    } else {
        bde_shim_vectors.base_address = (sal_vaddr_t)bar0;
    }

    printf("bde: DMA pool %zu MB at phys 0x%llx\n",
           dma_size >> 20, (unsigned long long)dma_phys);
    return 0;
}

uint32_t bde_shim_devid(uint16_t *dev_id, uint8_t *rev_id)
{
    uint32_t v = shim_pci_conf_read(NULL, 0);
    if (dev_id) *dev_id = (uint16_t)(v >> 16);
    if (rev_id) *rev_id = (uint8_t)(shim_pci_conf_read(NULL, 8) & 0xff);
    return v;
}
