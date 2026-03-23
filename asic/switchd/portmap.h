/*
 * portmap.h - Port mapping between front-panel, logical, and SerDes
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __PORTMAP_H__
#define __PORTMAP_H__

/* Parse portmap_N.0=lane:speed from config */
void portmap_parse_config(const char *key, const char *val);

/* Configure all ports on the ASIC based on parsed port map */
int portmap_configure_ports(void);

/* Poll all ports for link state changes.
 * Returns number of ports that changed state. */
int portmap_link_poll(void);

/* Lookup functions */
int portmap_swp_to_logical(int swp);
int portmap_logical_to_swp(int logical);
int portmap_phys_to_swp(int phys_port);
int portmap_swp_to_i2c_bus(int swp);

#endif /* __PORTMAP_H__ */
