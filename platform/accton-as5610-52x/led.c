/*
 * led.c - Front-panel port link/activity LEDs for AS5610-52X (BCM56846).
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The BCM56846 drives the front-panel LED serial chain from two LED
 * microprocessors (LEDUP0, LEDUP1). There are two ways to render link state:
 *
 *  1. Fully-autonomous (Cumulus "led auto on"): load the factory microcode and
 *     let the chip's hardware scan per-port MAC status into PORTSTATUS. We tried
 *     this first, but on EdgeNOS the chip-side link bits the scan reads are
 *     seeded stale at init (cumulus_replicate's EPC_LINK_BMAP) and edged's
 *     link-down transitions don't clear them, so the panel showed far more ports
 *     lit than have carrier, in the wrong places. Making that path correct needs
 *     the exact board PORT_ORDER_REMAP (only obtainable from a live Cumulus
 *     capture) plus perfect chip link-state sync.
 *
 *  2. Software-driven (this implementation): edged is the authoritative source
 *     of link state (port_state.link_up, derived from PCS block_lock), so we
 *     drive the LEDs from that directly. We load a tiny "passthrough" microcode
 *     that shifts 64 chain bits straight from data-RAM 0xA0..0xA7, and edged
 *     writes those bits each link poll. This is a real link/activity driver
 *     (solid green on link, blink on traffic) using the SDK's standard SW-LED
 *     mechanism — it cannot be fooled by stale chip state or an unknown remap.
 *
 * Output mapping (panel port -> chain bit) is PANEL_PORTS below, sourced from
 * Cumulus accton.py and EMPIRICALLY VERIFIED on this exact board (the leddance
 * tool lit the correct physical ports with it). Each port owns two adjacent
 * chain bits: amber (unused, kept off) then green.
 *
 * Register access uses bde_reg_*32() — the kernel BDE REG ioctl with raw
 * BAR0-relative offsets, auto-routed through PAXB sub-window 7 for offsets
 * >= 0x1000 (the proven leddance path).
 *
 * Register map (from bcm56840_a0_defs.h):
 *   LEDUP0: ctrl 0x1000  data 0x1400  prog 0x1800
 *   LEDUP1: ctrl 0x2000  data 0x2400  prog 0x2800
 * Each RAM entry is one byte in a 32-bit word; arrays step by 4. CTRL bit0=EN.
 */

#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <stdint.h>

#include "edged.h"
#include "led.h"

/* One LED microprocessor's register block. */
struct ledup {
    const char *name;
    uint32_t ctrl;        /* CMIC_LEDUPx_CTRL */
    uint32_t data_ram;    /* CMIC_LEDUPx_DATA_RAM (256 bytes) */
    uint32_t prog_ram;    /* CMIC_LEDUPx_PROGRAM_RAM (256 bytes) */
};

static const struct ledup ledups[2] = {
    { "LEDUP0", 0x1000, 0x1400, 0x1800 },
    { "LEDUP1", 0x2000, 0x2400, 0x2800 },
};

#define LED_CTRL_EN          0x1
#define LED_PROGRAM_RAM_SIZE 0x100   /* 256 instruction bytes */
#define LED_DATA_RAM_SIZE    0x100   /* 256 data bytes */
#define LED_CHAIN_BASE       0xA0    /* data-RAM offset the passthrough ucode reads */
#define LED_CHAIN_BYTES      8       /* 64 chain bits per processor */

/*
 * Passthrough LED microcode: shifts 64 bits from data-RAM 0xA0..0xA7 out to the
 * LED chain (bit layout: byte 0xA0+N bit i -> chain bit N*8+i). Assembled from
 * utils/leddance/passthrough.asm with Cumulus's ledasm. 43 meaningful bytes,
 * zero-padded to the 256-byte program RAM.
 */
static const uint8_t led_passthrough[LED_PROGRAM_RAM_SIZE] = {
    0x02, 0xA0, 0x60, 0xFE, 0x06, 0xFE, 0x10, 0x15,
    0x1A, 0x00, 0x27, 0x87, 0x1A, 0x01, 0x27, 0x87,
    0x1A, 0x02, 0x27, 0x87, 0x1A, 0x03, 0x27, 0x87,
    0x1A, 0x04, 0x27, 0x87, 0x1A, 0x05, 0x27, 0x87,
    0x1A, 0x06, 0x27, 0x87, 0x1A, 0x07, 0x27, 0x87,
    0x06, 0xFE, 0x80, 0x60, 0xFE, 0xD2, 0xA8, 0x74,
    0x04, 0x3A, 0x40, 0x00,
    /* remainder zero */
};

/*
 * Front-panel port (swpN, 1..52) -> (processor, amber chain-bit). green = amber+1.
 * Source: cumulus accton.py; verified on this board via leddance. Ports not in
 * the table (none here) are simply not driven.
 */
struct panel_led { uint8_t proc; uint8_t amber_bit; };
static const struct panel_led panel_ports[EDGED_MAX_PORTS + 1] = {
    [1]={1,34},[2]={1,32},[3]={1,38},[4]={1,36},
    [5]={1,62},[6]={1,60},[7]={1,58},[8]={1,56},
    [9]={0,2},[10]={0,0},[11]={0,6},[12]={0,4},
    [13]={0,50},[14]={0,48},[15]={0,54},[16]={0,52},
    [17]={0,46},[18]={0,44},[19]={0,42},[20]={0,40},
    [21]={0,62},[22]={0,60},[23]={0,58},[24]={0,56},
    [25]={0,38},[26]={0,36},[27]={0,34},[28]={0,32},
    [29]={0,30},[30]={0,28},[31]={0,26},[32]={0,24},
    [33]={0,14},[34]={0,8},[35]={0,10},[36]={0,12},
    [37]={0,22},[38]={0,20},[39]={0,18},[40]={0,16},
    [41]={1,44},[42]={1,42},[43]={1,40},[44]={1,46},
    [45]={1,52},[46]={1,50},[47]={1,48},[48]={1,54},
    [49]={1,26},[50]={1,24},[51]={1,30},[52]={1,28},
};

/* Blink: toggle ~every LED_BLINK_POLLS link-poll cycles (~30ms each). */
#define LED_BLINK_POLLS  8           /* ~240ms half-period */
static uint64_t last_pkts[EDGED_MAX_PORTS + 1];
static int led_ready;

static void led_ctrl_set(const struct ledup *lp, int enable)
{
    uint32_t ctrl = 0;
    bde_reg_read32(lp->ctrl, &ctrl);
    if (enable) ctrl |= LED_CTRL_EN; else ctrl &= ~LED_CTRL_EN;
    bde_reg_write32(lp->ctrl, ctrl);
}

/* Load the passthrough microcode into one processor and start it. */
static void led_load(const struct ledup *lp)
{
    led_ctrl_set(lp, 0);
    for (int i = 0; i < LED_PROGRAM_RAM_SIZE; i++)
        bde_reg_write32(lp->prog_ram + i * 4, led_passthrough[i]);
    /* Clear the chain bytes so we start dark. */
    for (int i = 0; i < LED_CHAIN_BYTES; i++)
        bde_reg_write32(lp->data_ram + (LED_CHAIN_BASE + i) * 4, 0);
    led_ctrl_set(lp, 1);
}

/*
 * Initialize both LED processors with the passthrough microcode. After this,
 * led_update() owns the front-panel LEDs. Called once from edged init.
 */
int led_init(void)
{
    for (int i = 0; i < 2; i++)
        led_load(&ledups[i]);

    memset(last_pkts, 0, sizeof(last_pkts));
    led_ready = 1;

    uint32_t c0 = 0, c1 = 0;
    bde_reg_read32(ledups[0].ctrl, &c0);
    bde_reg_read32(ledups[1].ctrl, &c1);
    syslog(LOG_INFO, "LED: passthrough loaded, LEDUP0 ctrl=0x%08x LEDUP1 ctrl=0x%08x "
           "(en=%d/%d); software link/activity driver active",
           c0, c1, c0 & LED_CTRL_EN, c1 & LED_CTRL_EN);
    led_update();
    return 0;
}

/*
 * Render current per-port link/activity to the LED chain. Called from the link
 * poll. Green is lit for ports with link_up, and blinks off briefly when the
 * port has packet activity (matching the familiar solid=link / blink=traffic
 * behavior). Amber is left off (as Cumulus does on this board).
 */
void led_update(void)
{
    static uint32_t poll;
    uint8_t bits[16];   /* [0..7]=LEDUP0 chain, [8..15]=LEDUP1 chain */

    if (!led_ready)
        return;

    memset(bits, 0, sizeof(bits));
    int blink_off = ((poll++ / LED_BLINK_POLLS) & 1);

    for (int i = 0; i < edged.num_ports; i++) {
        struct port_state *p = &edged.ports[i];
        if (!p->valid)
            continue;
        int sw = p->logical_port;          /* swpN == panel port number (1..52) */
        if (sw < 1 || sw > EDGED_MAX_PORTS)
            continue;
        if (!p->link_up)
            continue;

        /* Activity since last poll -> blink. */
        uint64_t pkts = p->rx_packets + p->tx_packets;
        int active = (pkts != last_pkts[sw]);
        last_pkts[sw] = pkts;

        int green = (active && blink_off) ? 0 : 1;   /* solid, blink off on traffic */
        if (!green)
            continue;

        int green_bit = panel_ports[sw].amber_bit + 1;
        int byte_idx = panel_ports[sw].proc * 8 + green_bit / 8;
        bits[byte_idx] |= (uint8_t)(1u << (green_bit & 7));
    }

    for (int pr = 0; pr < 2; pr++)
        for (int i = 0; i < LED_CHAIN_BYTES; i++)
            bde_reg_write32(ledups[pr].data_ram + (LED_CHAIN_BASE + i) * 4,
                            bits[pr * 8 + i]);
}

/* Dump LED processor state to syslog for diagnosis (SIGUSR2). */
void led_diag(void)
{
    for (int i = 0; i < 2; i++) {
        const struct ledup *lp = &ledups[i];
        uint32_t ctrl = 0;
        bde_reg_read32(lp->ctrl, &ctrl);
        char line[64];
        int p = 0;
        for (int b = 0; b < LED_CHAIN_BYTES; b++) {
            uint32_t v = 0;
            bde_reg_read32(lp->data_ram + (LED_CHAIN_BASE + b) * 4, &v);
            p += snprintf(line + p, sizeof(line) - p, "%02x ", v & 0xff);
        }
        syslog(LOG_INFO, "LED %s: ctrl=0x%08x en=%d chain[0xA0..A7]=%s",
               lp->name, ctrl, ctrl & LED_CTRL_EN, line);
    }
}
