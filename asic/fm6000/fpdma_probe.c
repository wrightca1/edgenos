/*
 * fpdma_probe.c - minimal live probe/harness for the FM6000 packet-DMA engine.
 *
 * Standalone byte-mover bring-up tool (edged has the logic but no CLI). Opens the
 * fm6000dma kmod, attaches its BAR0 mmap, brings up the TX/RX rings via fpdma_init,
 * and (with "tx") injects one F64 special-delivery frame and polls RX. Every phase
 * emits a /dev/kmsg marker so a CPU wedge is localized in the serial log; the SCD
 * watchdog is armed externally before running.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

#include "fpdma.h"
#include "fpdma_kmod.h"
#include "fm6000_regs.h"

#define FM6000_DMA_CFG 0x5060u   /* dma_cfg (w0x1418); reset reads 0x35 on 7150 */

static int kf = -1;
static void mark(const char *s)
{
    char b[160];
    int n = snprintf(b, sizeof b, "[PROBE] %s\n", s);
    if (kf >= 0) { if (write(kf, b, n) < 0) {} }
    fputs(b, stderr); fflush(stderr);
}

static void dumpdma(struct fm6000_dev *dev, const char *when)
{
    fprintf(stderr, "[PROBE] DMA %s: cmd=0x%08x status=0x%08x dma_cfg=0x%08x ip=0x%08x im=0x%08x\n",
        when,
        fm6000_dma_read(dev, FM6000_DMA_COMMAND),
        fm6000_dma_read(dev, FM6000_DMA_STATUS),
        fm6000_dma_read(dev, FM6000_DMA_CFG),
        fm6000_dma_read(dev, FM6000_DMA_IP),
        fm6000_dma_read(dev, FM6000_DMA_IM));
    fprintf(stderr, "[PROBE]   tx_base=%08x:%08x tx_end=%08x  rx_base=%08x:%08x rx_end=%08x\n",
        fm6000_dma_read(dev, FM6000_DMA_TX_BD_BASE_HI), fm6000_dma_read(dev, FM6000_DMA_TX_BD_BASE_LO),
        fm6000_dma_read(dev, FM6000_DMA_TX_BD_END_LO),
        fm6000_dma_read(dev, FM6000_DMA_RX_BD_BASE_HI), fm6000_dma_read(dev, FM6000_DMA_RX_BD_BASE_LO),
        fm6000_dma_read(dev, FM6000_DMA_RX_BD_END_LO));
    fflush(stderr);
}

static void rxcb(void *ctx, const void *data, uint16_t len)
{
    const uint8_t *p = data;
    fprintf(stderr, "[PROBE] *** RX %u bytes:", len);
    for (int i = 0; i < len && i < 40; i++) fprintf(stderr, " %02x", p[i]);
    fprintf(stderr, " ***\n"); fflush(stderr);
    (void)ctx;
}

int main(int argc, char **argv)
{
    kf = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    int do_tx = (argc > 1 && !strcmp(argv[1], "tx"));
    unsigned dglort = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 0x0000;
    /* argv[3] = SGLORT (source glort in the F64 tag). The L2F LBS stage suppresses
     * loopback to the *source* port; a CPU-injected frame with SGLORT=0 can be
     * pruned if 0 maps to the CPU/dest port, so set a non-CPU SGLORT to test punt. */
    unsigned sglort = (argc > 3) ? (unsigned)strtoul(argv[3], NULL, 0) : 0x0000;
    /* argv[4]=="swap": store the F64 tag words host-endian (byte-swapped) in the BD,
     * in case the DMA reads the field as words rather than a wire byte stream. */
    int swap = (argc > 4 && !strcmp(argv[4], "swap"));

    mark("open kmod /dev/fm6000dma");
    struct fpdma_kmod *k = NULL;
    if (fpdma_kmod_open(&k) < 0) { mark("kmod open FAILED (insmod fm6000dma.ko?)"); return 1; }

    size_t bsz = 0;
    volatile void *bar0 = fpdma_kmod_bar0(k, &bsz);
    struct fm6000_dev dev;
    fm6000_hw_attach(&dev, bar0, bsz, "0000:02:00.0");
    mark("BAR0 attached");
    dumpdma(&dev, "before");

    struct fpdma_backing back = fpdma_kmod_backing(k);
    struct fpdma fp;
    mark("fpdma_init: alloc rings + program regs + command  <-- RISKY");
    if (fpdma_init(&fp, &dev, &back, 4, 4) < 0) { mark("fpdma_init FAILED"); return 1; }
    mark("fpdma_init returned OK (rings up, engine kicked)");
    dumpdma(&dev, "after-init");

    if (do_tx) {
        /* Frame body = DMAC(6) SMAC(6) then payload. The F64 tag is NOT inline:
         * the DMA reads it from the BD's F64 field and inserts it at offset 12 on
         * the way to the fabric (datasheet §7.11.1.4). So we build the 8-byte
         * offset-12 tag (Table 7-8: word0=FTYPE, w1=VLAN, w2=SGLORT, w3=DGLORT,
         * big-endian) separately and hand it to fpdma_tx_f64.
         *
         * With the golden catch-all GLORT (CAM[0]=0x007fffff -> DMaskBaseIdx=1 ->
         * L2F_256[1]={CPU bit0, Et1 bit40}) ANY dglort resolves to the CPU port,
         * so this special-delivery frame should return on the RX ring. Inject is
         * non-destructive (no table writes) — safe to sweep several dglorts. */
        uint8_t f[64];
        memset(f, 0, sizeof f);
        memset(f, 0xff, 6);                                   /* DMAC broadcast */
        f[6]=0x02; f[7]=0; f[8]=0; f[9]=0; f[10]=0; f[11]=0x01;/* SMAC */
        f[12]=0xDE; f[13]=0xAD; f[14]=0xBE; f[15]=0xEF;       /* payload marker */

        /* FTYPE/VLAN overridable via env (FTYPE=0 = normal delivery vs 0x1000 =
         * special; VLAN for the membership-tagged path) so we can sweep the punt
         * frame type without recompiling. */
        const char *fe = getenv("FTYPE"), *ve = getenv("VLAN");
        uint16_t ftype = fe ? (uint16_t)strtoul(fe, NULL, 0) : 0x1000;
        uint16_t vlan  = ve ? (uint16_t)strtoul(ve, NULL, 0) : 0x0000;
        uint16_t tw[4] = { ftype, vlan, (uint16_t)sglort, (uint16_t)dglort };/* w0 FTYPE, w1 VLAN, w2 SGLORT, w3 DGLORT */
        uint8_t tag[8];
        for (int w = 0; w < 4; w++) {
            if (swap) { tag[2*w] = tw[w] & 0xff; tag[2*w+1] = tw[w] >> 8; }   /* host-endian */
            else      { tag[2*w] = tw[w] >> 8;   tag[2*w+1] = tw[w] & 0xff; } /* wire big-endian */
        }

        char m[160]; snprintf(m, sizeof m,
            "fpdma_tx_f64 FTYPE=0x%04x VLAN=0x%04x DGLORT=0x%04x SGLORT=0x%04x endian=%s",
            ftype, vlan, dglort, sglort, swap ? "host" : "wire");
        mark(m);
        if (fpdma_tx_f64(&fp, f, 60, tag, sizeof tag) < 0) mark("fpdma_tx_f64 ring full/FAILED");
        else mark("fpdma_tx_f64 queued + engine kicked");
        usleep(100000);
        dumpdma(&dev, "after-tx");
        fprintf(stderr, "[PROBE] tx desc[0] status=0x%02x (0x09 = still HW-owned)\n",
                *(volatile uint8_t *)fp.tx.desc);
        fprintf(stderr, "[PROBE] tx_reclaim=%d\n", fpdma_tx_reclaim(&fp));
        fflush(stderr);
        mark("rx_poll x10");
        int got = 0;
        for (int i = 0; i < 10; i++) { got += fpdma_rx_poll(&fp, 4, rxcb, NULL); usleep(100000); }
        fprintf(stderr, "[PROBE] total RX frames=%d\n", got);

        /* RX-capture diagnosis (phase50): dump the raw RX BD status bytes + ring
         * addr. If HW wrote a non-0x09 status (e.g. DONE/EOP/ERR) that rx_poll's
         * bit-check missed, the RX status-bit ENCODING is the bug, not forwarding.
         * Also show the DMA STATUS RxState[5:3] and the RX current-BD pointer. */
        fprintf(stderr, "[PROBE] rx.desc_dma=%08llx rx.size=%u  RX BD status:",
                (unsigned long long)fp.rx.desc_dma, fp.rx.size);
        for (uint32_t i = 0; i < fp.rx.size && i < 8; i++)
            fprintf(stderr, " bd[%u]=0x%02x", i,
                    *(volatile uint8_t *)(fp.rx.desc + (size_t)i * FM6000_DESC_STRIDE));
        fprintf(stderr, "\n[PROBE] DMA STATUS=0x%08x (RxState[5:3], TxState[2:0])  cur_rx_ptr=0x%08x\n",
                fm6000_dma_read(&dev, FM6000_DMA_STATUS),
                fm6000_dma_read(&dev, 0x5038));   /* cur_rx/bd ptr region */
    }
    mark("DONE (engine left running)");
    return 0;
}
