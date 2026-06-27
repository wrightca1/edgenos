/*
 * sfpi.c - SFP/QSFP port interface for AS5610-52X
 *
 * Provides presence detection, EEPROM reading, TX enable/disable,
 * and DOM (Digital Optical Monitoring) for all 52 ports.
 *
 * Port mapping:
 *   Ports 1-48:  SFP+ (I2C bus 22-69 via PCA9546/PCA9548 mux tree)
 *   Ports 49-52: QSFP+ (I2C bus 18-21 via PCA9546 @ 0x77)
 *
 * Adapted from ONL (Copyright 2014 Big Switch Networks / Accton Technology)
 * SPDX-License-Identifier: EPL-1.0
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "platform_lib.h"

/* ── SFP presence detection ──────────────────────────────────── */

/*
 * SFP presence is read from PCA9506 GPIO expanders on bus 17:
 *   0x20: MOD_ABS ports 0-39 (active LOW = present)
 *   0x23: Mixed signals for ports 40-47 + QSFP 48-51
 */
int sfp_is_present(int port)
{
    uint8_t val;
    int reg_offset, bit;

    if (port < 1 || port > NUM_TOTAL_PORTS)
        return 0;

    if (port <= 40) {
        /* Ports 1-40: PCA9506 @ 0x20 on bus 17 */
        int idx = port - 1;
        reg_offset = PCA9506_REG_INPUT_BASE + (idx / 8);
        bit = idx % 8;

        if (i2c_read(I2C_BUS_GPIO_CTRL_2, I2C_ADDR_SFP_MOD_ABS,
                     reg_offset, &val) != 0)
            return 0;

        return (val & (1 << bit)) ? 0 : 1;  /* Active LOW */
    } else if (port <= 48) {
        /* Ports 41-48: PCA9506 @ 0x23 on bus 17 */
        int idx = port - 41;
        reg_offset = PCA9506_REG_INPUT_BASE;
        bit = idx;

        if (i2c_read(I2C_BUS_GPIO_CTRL_2, I2C_ADDR_SFP_MIXED,
                     reg_offset, &val) != 0)
            return 0;

        return (val & (1 << bit)) ? 0 : 1;
    } else {
        /* QSFP ports 49-52: bits 0-3 of PCA9506 @ 0x23, register 1 */
        int idx = port - 49;

        if (i2c_read(I2C_BUS_GPIO_CTRL_2, I2C_ADDR_SFP_MIXED,
                     PCA9506_REG_INPUT_BASE + 1, &val) != 0)
            return 0;

        return (val & (1 << idx)) ? 0 : 1;
    }
}

/* ── SFP EEPROM reading ──────────────────────────────────────── */

int sfp_eeprom_read(int port, uint8_t *data, int size)
{
    int bus;

    if (port < 1 || port > NUM_TOTAL_PORTS)
        return -1;

    if (port <= NUM_SFP_PORTS)
        bus = SFP_PORT_TO_I2C_BUS(port);
    else
        bus = QSFP_PORT_TO_I2C_BUS(port);

    /* Read from EEPROM at 0x50 */
    return i2c_nRead(bus, I2C_ADDR_SFP_EEPROM, 0, size, data);
}

/* ── SFP TX disable control ──────────────────────────────────── */

int sfp_tx_disable_set(int port, int disable)
{
    uint8_t val;
    int reg_offset, bit;

    if (port < 1 || port > NUM_SFP_PORTS)
        return -1;

    int idx = port - 1;
    reg_offset = PCA9506_REG_OUTPUT_BASE + (idx / 8);
    bit = idx % 8;

    /* Read current value */
    if (i2c_read(I2C_BUS_GPIO_CTRL_2, I2C_ADDR_SFP_TX_DISABLE,
                 reg_offset, &val) != 0)
        return -1;

    /* Set or clear bit (HIGH = TX enabled due to inverter) */
    if (disable)
        val &= ~(1 << bit);   /* Clear bit = TX disabled */
    else
        val |= (1 << bit);    /* Set bit = TX enabled */

    return i2c_write(I2C_BUS_GPIO_CTRL_2, I2C_ADDR_SFP_TX_DISABLE,
                     reg_offset, &val);
}

/* ── SFP RX LOS status ──────────────────────────────────────── */

int sfp_rx_los_get(int port)
{
    uint8_t val;
    int reg_offset, bit;

    if (port < 1 || port > 40)
        return -1;

    int idx = port - 1;
    reg_offset = PCA9506_REG_INPUT_BASE + (idx / 8);
    bit = idx % 8;

    if (i2c_read(I2C_BUS_GPIO_CTRL_2, I2C_ADDR_SFP_RX_LOS,
                 reg_offset, &val) != 0)
        return -1;

    return (val & (1 << bit)) ? 1 : 0;  /* Active HIGH */
}

/* ── SFP TX fault status ─────────────────────────────────────── */

int sfp_tx_fault_get(int port)
{
    uint8_t val;
    int reg_offset, bit;

    if (port < 1 || port > 40)
        return -1;

    int idx = port - 1;
    reg_offset = PCA9506_REG_INPUT_BASE + (idx / 8);
    bit = idx % 8;

    if (i2c_read(I2C_BUS_GPIO_CTRL_2, I2C_ADDR_SFP_TX_FAULT,
                 reg_offset, &val) != 0)
        return -1;

    return (val & (1 << bit)) ? 1 : 0;  /* Active HIGH */
}

/* ── QSFP control ────────────────────────────────────────────── */

int qsfp_reset_set(int port, int reset)
{
    uint8_t val;
    int bit = port - 49;

    if (port < 49 || port > 52)
        return -1;

    /* PCA9538 @ 0x70 on bus 16, GPIO 160-163 = RST_L */
    if (i2c_read(I2C_BUS_GPIO_CTRL_1, 0x70, 0x01, &val) != 0)
        return -1;

    if (reset)
        val &= ~(1 << bit);   /* LOW = in reset */
    else
        val |= (1 << bit);    /* HIGH = out of reset */

    return i2c_write(I2C_BUS_GPIO_CTRL_1, 0x70, 0x01, &val);
}

int qsfp_lpmode_set(int port, int lpmode)
{
    uint8_t val;
    int bit = port - 49;

    if (port < 49 || port > 52)
        return -1;

    /* PCA9538 @ 0x70 on bus 16, GPIO 168-171 = LPMODE */
    if (i2c_read(I2C_BUS_GPIO_CTRL_1, 0x70, 0x01, &val) != 0)
        return -1;

    if (lpmode)
        val |= (1 << (bit + 4));   /* HIGH = low power mode */
    else
        val &= ~(1 << (bit + 4));  /* LOW = normal power */

    return i2c_write(I2C_BUS_GPIO_CTRL_1, 0x70, 0x01, &val);
}

/* ── SFP DOM (Digital Optical Monitoring) ────────────────────── */

int sfp_dom_read(int port, int *temp_mc, int *vcc_mv,
                 int *tx_power_uw, int *rx_power_uw)
{
    uint8_t dom[10];
    int bus;

    if (port < 1 || port > NUM_SFP_PORTS)
        return -1;

    bus = SFP_PORT_TO_I2C_BUS(port);

    /* DOM data is at address 0x51 (A2), offset 96-105 */
    if (i2c_nRead(bus, 0x51, 96, 10, dom) != 0)
        return -1;

    /* Temperature: 16-bit signed, units of 1/256 C */
    if (temp_mc) {
        int16_t raw = (dom[0] << 8) | dom[1];
        *temp_mc = (raw * 1000) / 256;
    }

    /* VCC: 16-bit unsigned, units of 100 uV */
    if (vcc_mv) {
        uint16_t raw = (dom[2] << 8) | dom[3];
        *vcc_mv = raw / 10;
    }

    /* TX power: 16-bit unsigned, units of 0.1 uW */
    if (tx_power_uw) {
        uint16_t raw = (dom[6] << 8) | dom[7];
        *tx_power_uw = raw / 10;
    }

    /* RX power: 16-bit unsigned, units of 0.1 uW */
    if (rx_power_uw) {
        uint16_t raw = (dom[8] << 8) | dom[9];
        *rx_power_uw = raw / 10;
    }

    return 0;
}
