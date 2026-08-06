/*
 * fm6000_ucode.h - FM6000 microcode load (parser/FFU + SerDes SPICO)
 *
 * Two independent mechanisms (phase7g §c):
 *   1. PARSER/FFU/MAPPER microcode == a TEXT list of "<word-addr> <value>" CSR
 *      writes. Loading = replay ~44K writes, then read-back verify.
 *   2. SerDes SPICO == a BINARY uint16[] blob pushed over SBus into the SPICO
 *      IMEM, then CRC-verified via an SPICO interrupt.
 *
 * PROVENANCE BOUNDARY: the firmware payloads (fm6000Microcode.raw, the 12000-byte
 * SPICO blob) are Arista/Intel proprietary and are NOT vendored. They are staged
 * on the box (extracted from the running EOS image by the operator) and loaded by
 * path at runtime. This file implements only the *load procedure*.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __FM6000_UCODE_H__
#define __FM6000_UCODE_H__

#include "fm6000_hw.h"

/* Default on-box firmware locations (mirror EOS /usr/share/firmware layout). */
#define FM6000_FW_PARSER_FFU   "/usr/share/firmware/fm6000/fm6000Microcode.raw"
#define FM6000_FW_SPICO        "/usr/share/firmware/fm6000/fm6000_spico.bin"

/* Replay a text CSR image ("<hex word-addr> <hex value>" per line) onto the
 * switch. verify!=0 read-back-checks every write. Returns #writes, or -1. */
long fm6000_load_csr_image(struct fm6000_dev *dev, const char *path, int verify);

/* Upload a binary SPICO image (little-endian uint16 words) over SBus into the
 * SerDes SPICO IMEM, then run + CRC-verify. Returns 0 on success. */
int  fm6000_load_spico(struct fm6000_dev *dev, const char *path);

#endif /* __FM6000_UCODE_H__ */
