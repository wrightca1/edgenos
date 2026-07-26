/*
 * fm6000_l2.h - FM6000 L2-forwarding bring-up for CPU punt (RX)
 *
 * The RX half of "CPU inject -> pipeline -> CPU receive". TX already transmits
 * (fpdma.c + PCI_TX_POST); the return path needs the switch to actually FORWARD a
 * frame to the CPU port. There is no special-delivery bypass: a frame reaches the
 * CPU port only after
 *     GLORT_CAM -> GLORT_RAM(DMaskBaseIdx) -> DMASK_TABLE(CPU-port bit)
 *                -> L2F 13-stage (VLAN & STP & srcport, must admit the CPU port).
 * All three are unconfigured on M1. This module programs the minimal subset for a
 * CPU loopback, then (later) the general L2 primitives behind core/datapath/l2.c.
 *
 * Reference state: reference/live-captures/7150-fm6000/eos-golden-2026-07-26-l2
 * (running-EOS golden capture) — the values marked TODO(golden) below are pinned
 * from that capture's analysis + an M1 zero-baseline diff. Register offsets are
 * RE'd facts (the FM6000_BLK_ and FM6000_L2 macros in fm6000_regs.h), never the
 * vendor header.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef __FM6000_L2_H__
#define __FM6000_L2_H__

#include "fm6000_hw.h"

/* CPU loopback config. Defaults (fm6000_l2_cpu_cfg_default) are the pinned
 * values: CPU port 0, CPU GLORT 0xFF00. cam_idx/dmask_gid pick free table slots
 * to program (any unused entries — the lab EOS uses almost none). src_port_first/
 * last bound which source physical ports get CPU-port L2F membership. */
struct fm6000_l2_cpu_cfg {
    int      cpu_port;      /* physical/logical CPU port (0 on the PCIe path)   */
    uint16_t cpu_glort;     /* DGLORT that resolves to the CPU port (0xFF00)    */
    unsigned cam_idx;       /* GLORT_CAM/RAM entry to program                   */
    unsigned dmask_gid;     /* MCAST_DEST_TABLE group id to program (the DMASK) */
    unsigned src_port_first;/* first source physical port to admit in L2F       */
    unsigned src_port_last; /* last  source physical port to admit in L2F       */
};

/* Fill cfg with the pinned defaults (CPU port 0, GLORT 0xFF00, slots cam=8,
 * gid=16, admit source ports 0..75). Override fields as needed before calling. */
void fm6000_l2_cpu_cfg_default(struct fm6000_l2_cpu_cfg *cfg);

/* Program a minimal GLORT catch-all -> DMASK(CPU port) -> L2F(admit CPU port) so
 * that an injected frame is forwarded to the CPU port and returns up the fpdma RX
 * ring. Returns 0 on success. Idempotent. This is the "moves a byte" milestone. */
int fm6000_l2_configure_cpu_loopback(struct fm6000_dev *dev,
                                     const struct fm6000_l2_cpu_cfg *cfg);

/* Read back the GLORT/DMASK/L2F words this module drives, to stderr — for diffing
 * an M1 bring-up against the golden EOS capture. Non-destructive. */
void fm6000_l2_dump_state(struct fm6000_dev *dev);

#endif /* __FM6000_L2_H__ */
