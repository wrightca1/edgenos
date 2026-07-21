/*
 * fm6000_edged.h - FM6000 backend for the EdgeNOS ASIC-ops seam
 *
 * Wraps the clean-room FM6000 driver (fpdma_vfio + fm6000_boot + fpdma) as a
 * struct asic_ops the 7150 datapath daemon binds to.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __FM6000_EDGED_H__
#define __FM6000_EDGED_H__

#include "asic_ops.h"

/* The FM6000 backend. init() reads the PCI slot from $EDGENOS_FM6000_SLOT
 * (default 0000:02:00.0). Single-instance (one ASIC per pizza box). */
const struct asic_ops *fm6000_asic_ops(void);

#endif /* __FM6000_EDGED_H__ */
