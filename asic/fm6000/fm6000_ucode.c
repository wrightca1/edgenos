/*
 * fm6000_ucode.c - FM6000 microcode load procedures (clean-room)
 *
 * Recovered from libFocalpointSDK.so fmPlatformLoadMicrocode /
 * fm6000LoadSpicoCode (notes/analysis/phase7g-fm6000-bringup-recovered.md §c/d).
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "fm6000_ucode.h"

/* ======================================================================== */
/* 1. Parser/FFU/MAPPER microcode = text CSR replay                          */
/* ======================================================================== */
/*
 * fmPlatformLoadMicrocodeRawImageFile: fgets each line, strtoul the hex
 * word-address and hex value, writeCSR(addr, val). Blank/comment lines skipped.
 * fmPlatformValidateMicrocode then read-back-verifies. We fold verify inline.
 */
long fm6000_load_csr_image(struct fm6000_dev *dev, const char *path, int verify)
{
    FILE *f = fopen(path, "r");
    char line[128];
    long count = 0;

    if (!f) {
        fprintf(stderr, "fm6000: ucode image %s: %s\n", path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        char *p = line, *end;
        unsigned long addr, val;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;

        addr = strtoul(p, &end, 16);
        if (end == p)
            continue;                 /* not a hex line */
        val = strtoul(end, &end, 16);

        fm6000_csr_write(dev, (uint32_t)addr, (uint32_t)val);

        if (verify) {
            uint32_t got = fm6000_csr_read(dev, (uint32_t)addr);
            /* Not every CSR reads back its written value (write-only / status
             * fields exist); only flag a hard mismatch on the microcode RAM
             * windows (PARSER/MAPPER/FFU). */
            int in_ucode_ram =
                (addr >= FM6000_BLK_PARSER && addr < FM6000_BLK_PARSER + 0x40000u) ||
                (addr >= FM6000_BLK_MAPPER && addr < FM6000_BLK_MAPPER + 0x40000u) ||
                (addr >= FM6000_BLK_FFU    && addr < FM6000_BLK_FFU    + 0x40000u);
            if (in_ucode_ram && got != (uint32_t)val) {
                fprintf(stderr,
                        "fm6000: ucode verify FAIL @0x%05lx: wrote 0x%08lx read 0x%08x\n",
                        addr, val, got);
                fclose(f);
                return -1;
            }
        }
        count++;
    }

    fclose(f);
    fprintf(stderr, "fm6000: loaded %ld CSR writes from %s%s\n",
            count, path, verify ? " (verified)" : "");
    return count;
}

/* ======================================================================== */
/* 2. SerDes SPICO microcode = binary blob over SBus into IMEM               */
/* ======================================================================== */
/*
 * SBus write primitive. The SPICO slave-register addresses (0xFDxx) are fully
 * recovered; the SBus *controller* framing at FM6000_BLK_SERDES_WR (0xB0500) —
 * how a (slave_reg, value) pair is packed into cmd/addr/data CSRs and how the
 * ready bit is polled — is the one runtime-computed piece still needing a live
 * register trace (GAPS.md A, phase7g §e/2). Implemented as a documented stub so
 * the SPICO sequence below is complete and reviewable; the trace fills only this
 * function.
 */
/* SBus transaction over the JSS SBus master (0xF001/2/3). `reg` = {Address[15:8], Register[7:0]}
 * (e.g. 0xFD0C = receiver 0xFD, register 0x0C). Op 0x21=write / 0x22=read. Decoded verbatim from
 * libFocalpointSDK.so (fm6000WriteSBus@0x479e09, executor @0x477c54); LIVE-VALIDATED on the 7150.
 * NOTE: 0xF004 (SBUS_SPICO Reset/Enable) is a DIRECT CSR, not routed through here. */
#define FM6000_SBUS_COMMAND_R  0xF001u
#define FM6000_SBUS_REQUEST_R  0xF002u
#define FM6000_SBUS_RESPONSE_R 0xF003u
#define FM6000_SBUS_OP_WRITE   0x21u
#define FM6000_SBUS_OP_READ    0x22u
static int fm6000_sbus_xact(struct fm6000_dev *dev, uint32_t op, uint32_t reg, uint32_t *val)
{
    uint32_t cmd, st = 0; int i;
    fm6000_csr_write(dev, FM6000_SBUS_REQUEST_R, op == FM6000_SBUS_OP_WRITE ? *val : 0);
    fm6000_csr_write(dev, FM6000_SBUS_COMMAND_R, 0);
    cmd = (reg & 0xFFFFu) | (op << 16) | (1u << 24);          /* Execute */
    fm6000_csr_write(dev, FM6000_SBUS_COMMAND_R, cmd);
    for (i = 0; i < 100000; i++) {                            /* poll Busy(bit25) */
        st = fm6000_csr_read(dev, FM6000_SBUS_COMMAND_R);
        if (!(st & (1u << 25))) break;
    }
    if (st & (1u << 25)) { fprintf(stderr, "fm6000: SBus busy timeout reg=0x%04x\n", reg); return -1; }
    if (((st >> 26) & 7u) != (op == FM6000_SBUS_OP_WRITE ? 1u : 4u)) {
        fprintf(stderr, "fm6000: SBus rc=%u reg=0x%04x\n", (st >> 26) & 7u, reg); return -1; }
    if (op == FM6000_SBUS_OP_READ) *val = fm6000_csr_read(dev, FM6000_SBUS_RESPONSE_R);
    return 0;
}
static int fm6000_sbus_write(struct fm6000_dev *dev, uint32_t slave_reg, uint32_t val)
{ return fm6000_sbus_xact(dev, FM6000_SBUS_OP_WRITE, slave_reg, &val); }
static int fm6000_sbus_read(struct fm6000_dev *dev, uint32_t slave_reg, uint32_t *val)
{ return fm6000_sbus_xact(dev, FM6000_SBUS_OP_READ, slave_reg, val); }

int fm6000_load_spico(struct fm6000_dev *dev, const char *path)
{
    FILE *f = fopen(path, "rb");
    uint16_t *code = NULL;
    long nbytes;
    size_t nwords, i;
    int rc = -1;

    if (!f) {
        fprintf(stderr, "fm6000: spico image %s: %s\n", path, strerror(errno));
        return -1;
    }
    fseek(f, 0, SEEK_END);
    nbytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (nbytes <= 0 || (nbytes & 1)) {
        fprintf(stderr, "fm6000: spico image bad size %ld\n", nbytes);
        goto out;
    }
    nwords = (size_t)nbytes / 2;
    code = malloc((size_t)nbytes);
    if (!code || fread(code, 1, (size_t)nbytes, f) != (size_t)nbytes) {
        fprintf(stderr, "fm6000: spico image read failed\n");
        goto out;
    }

    /* Upload loop, exactly as recovered (phase7g §c/2). code[i] is a 16-bit
     * SPICO instruction word (host is little-endian, matching the on-disk blob). */
    fm6000_sbus_write(dev, FM6000_SBUS_SPICO_CTRL,     3);   /* SPICO reset       */
    fm6000_sbus_write(dev, FM6000_SBUS_SPICO_CTRL,     1);   /* SPICO enable      */
    fm6000_sbus_write(dev, FM6000_SBUS_SPICO_IMEM_CTL, 8);   /* IMEM write enable */

    for (i = 0; i < nwords; i++) {
        uint32_t w = code[i];
        fm6000_sbus_write(dev, FM6000_SBUS_SPICO_ADDR_HI, (uint32_t)(i >> 8));
        fm6000_sbus_write(dev, FM6000_SBUS_SPICO_ADDR_LO, (uint32_t)(i & 0xFF));
        fm6000_sbus_write(dev, FM6000_SBUS_SPICO_DATA_LO,  w & 0xFF);
        fm6000_sbus_write(dev, FM6000_SBUS_SPICO_IMEM_CTL, ((w >> 8) & 0xFF) | 0xC); /* data hi + strobe */
        fm6000_sbus_write(dev, FM6000_SBUS_SPICO_IMEM_CTL, ((w >> 8) & 0xFF) | 0x8); /* strobe deassert  */
    }

    fm6000_sbus_write(dev, FM6000_SBUS_SPICO_IMEM_CTL, 0);   /* IMEM write disable */
    fm6000_sbus_write(dev, FM6000_SBUS_SPICO_CTRL,     8);   /* SPICO run          */

    /* TODO(live-trace): fm6000InterruptSpico(sw, 4, ...) CRC-verify handshake,
     * then WriteSBus(0xFD0C, 0). The interrupt command encoding beyond cmd 0x4
     * is runtime-computed (phase7g §e/4). */

    fprintf(stderr, "fm6000: SPICO uploaded %zu words from %s\n", nwords, path);
    rc = 0;
out:
    free(code);
    fclose(f);
    return rc;
}
