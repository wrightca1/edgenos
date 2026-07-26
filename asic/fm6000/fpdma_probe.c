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
        /* Minimal F64 special-delivery frame: DMAC(6) SMAC(6), then the 4x16b F64
         * tag at offset 12 (FTYPE=0x1000 special, VLAN=0, SGLORT=0, DGLORT=arg),
         * then a DEADBEEF payload marker. Big-endian 16-bit words. */
        uint8_t f[64];
        memset(f, 0, sizeof f);
        memset(f, 0xff, 6);                                   /* DMAC broadcast */
        f[6]=0x02; f[7]=0; f[8]=0; f[9]=0; f[10]=0; f[11]=0x01;/* SMAC */
        f[12]=0x10; f[13]=0x00;                               /* F64 word0 FTYPE=special (0x1000) */
        f[14]=0x00; f[15]=0x00;                               /* VLAN word */
        f[16]=0x00; f[17]=0x00;                               /* SGLORT */
        f[18]=(uint8_t)(dglort >> 8); f[19]=(uint8_t)(dglort & 0xff); /* DGLORT */
        f[20]=0xDE; f[21]=0xAD; f[22]=0xBE; f[23]=0xEF;       /* payload marker */

        char m[96]; snprintf(m, sizeof m, "fpdma_tx one F64 frame DGLORT=0x%04x  <-- RISKY", dglort);
        mark(m);
        if (fpdma_tx(&fp, f, 60) < 0) mark("fpdma_tx ring full/FAILED");
        else mark("fpdma_tx queued + engine kicked");
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
    }
    mark("DONE (engine left running)");
    return 0;
}
