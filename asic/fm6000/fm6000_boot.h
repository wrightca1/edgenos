/*
 * fm6000_boot.h - FM6000 bring-up orchestration
 *
 * Mirrors the fm6000BootSwitch call order recovered in phase7g §b. The public
 * entry is fm6000_boot_switch(); the staged helpers are exposed for unit-by-unit
 * bring-up and diagnostics.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __FM6000_BOOT_H__
#define __FM6000_BOOT_H__

#include "fm6000_hw.h"

/* Full bring-up: preboot(+BIST) -> sbus -> caches -> microcode -> SPICO ->
 * ports/MAC/CPU -> forwarding-table init -> enable forwarding. Returns 0 on a
 * chip that reached the CPU-punt-capable state. */
int fm6000_boot_switch(struct fm6000_dev *dev);

/* Stage 1: chip/PLL bring-up + built-in memory BIST/repair. */
int fm6000_preboot(struct fm6000_dev *dev);

/* BIST / built-in memory init + repair (register skeleton recovered; march
 * pattern *values* are TODO(live-trace)). Returns 0 if BIST_DONE_PASS. */
int fm6000_bist_memory_init(struct fm6000_dev *dev);

#endif /* __FM6000_BOOT_H__ */
