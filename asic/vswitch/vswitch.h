/*
 * vswitch.h - EdgeNOS software "ASIC": an L2 learning switch over Linux netdevs.
 *
 * Copyright (C) 2026 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef __VSWITCH_H__
#define __VSWITCH_H__

#include <stdint.h>
#include "asic_ops.h"

#define VSW_MAX_PORTS   64
#define VSW_CPU_PORT    VSW_MAX_PORTS          /* the CPU is "port 64" in the MAC table */
#define VSW_MAC_ENTRIES 4096
#define VSW_MAC_AGE_S   300
#define VSW_MAX_FRAME   2048
#define VSW_PUNT_RING   256

/* The asic_ops table for this backend (vswitch_edged.c). */
const struct asic_ops *vswitch_asic_ops(void);

/* Optional extras a board main may use (not part of the seam). */
int  vswitch_port_count(void);
const char *vswitch_port_name(int port);
void vswitch_dump(void);                        /* ports + MAC table to stderr */

#endif /* __VSWITCH_H__ */
