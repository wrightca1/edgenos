/*
 * packet_io.h - Packet I/O between kernel TUN interfaces and ASIC
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __PACKET_IO_H__
#define __PACKET_IO_H__

/* Initialize TUN interfaces and packet I/O */
int packet_io_init(void);

/* Poll for RX packets from ASIC and forward to TUN */
void packet_io_rx_poll(void);

/* Cleanup TUN interfaces */
void packet_io_cleanup(void);

#endif /* __PACKET_IO_H__ */
