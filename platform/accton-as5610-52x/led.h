/*
 * led.h - Front-panel port LED control for AS5610-52X.
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __EDGED_LED_H__
#define __EDGED_LED_H__

/* Load the passthrough microcode into both LED microprocessors and start them.
 * After this, led_update() owns the front-panel LEDs. Call once at init. */
int led_init(void);

/* Render current per-port link/activity (from edged.ports) to the LED chain.
 * Call from the link poll loop. */
void led_update(void);

/* Dump LED processor / chain state to syslog for diagnosis (SIGUSR2). */
void led_diag(void);

#endif /* __EDGED_LED_H__ */
