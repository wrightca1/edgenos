/*
 * platform_lib.h - Platform library for AS5610-52X
 *
 * Hardware access functions for CPLD, I2C, and PMBus devices.
 * Adapted from ONL (Copyright 2014 Big Switch Networks / Accton Technology)
 *
 * SPDX-License-Identifier: EPL-1.0
 */

#ifndef __PLATFORM_LIB_H__
#define __PLATFORM_LIB_H__

#include <stdint.h>

/* ── CPLD access (memory-mapped at 0xEA000000) ─────────────── */
#define CPLD_BASE_ADDRESS       0xEA000000

/* CPLD register offsets */
#define CPLD_REG_VERSION        0x00
#define CPLD_REG_PSU_STATUS_1   0x01
#define CPLD_REG_PSU_STATUS_2   0x02
#define CPLD_REG_SYS_STATUS     0x03
#define CPLD_REG_FAN_SPEED_CTL  0x0D
#define CPLD_REG_LED_1          0x13
#define CPLD_REG_LED_LOC        0x15

int cpld_read(unsigned int reg_offset, uint8_t *val);
int cpld_write(unsigned int reg_offset, uint8_t val);

/* ── I2C access ────────────────────────────────────────────── */

int i2c_read(unsigned int bus_id, uint8_t i2c_addr,
             uint8_t offset, uint8_t *buf);
int i2c_write(unsigned int bus_id, uint8_t i2c_addr,
              uint8_t offset, uint8_t *buf);
int i2c_nRead(unsigned int bus_id, uint8_t i2c_addr,
              uint8_t offset, unsigned int size, uint8_t *buf);
int i2c_nWrite(unsigned int bus_id, uint8_t i2c_addr,
               uint8_t offset, unsigned int size, uint8_t *buf);

int as5610_52x_i2c0_pca9548_channel_set(uint8_t channel);

/* ── PMBus ─────────────────────────────────────────────────── */

int two_complement_to_int(uint16_t data, uint8_t valid_bit, uint16_t mask);
int pmbus_parse_literal_format(uint16_t val);
int pmbus_read_literal_data(uint8_t bus, uint8_t i2c_addr,
                            uint8_t reg, int *val);
int pmbus_read_vout_data(uint8_t bus, uint8_t i2c_addr, int *val);

/* ── PSU types ─────────────────────────────────────────────── */

typedef enum {
    PSU_TYPE_UNKNOWN,
    PSU_TYPE_AC_F2B,
    PSU_TYPE_AC_B2F,
    PSU_TYPE_DC_48V_F2B,
    PSU_TYPE_DC_48V_B2F
} as5610_52x_psu_type_t;

as5610_52x_psu_type_t as5610_52x_get_psu_type(int id, char *modelname, int size);

/* ── Thermal sensor IDs ────────────────────────────────────── */

enum thermal_sensor_id {
    THERMAL_RESERVED = 0,
    NE1617A_LOCAL_SENSOR,
    NE1617A_REMOTE_SENSOR,
    MAX6581_LOCAL_SENSOR,
    MAX6581_REMOTE_SENSOR_1,
    MAX6581_REMOTE_SENSOR_2,
    MAX6581_REMOTE_SENSOR_3,
    MAX6581_REMOTE_SENSOR_4,
    MAX6581_REMOTE_SENSOR_5,
    MAX6581_REMOTE_SENSOR_6,
    MAX6581_REMOTE_SENSOR_7,
    BCM56846_LOCAL_SENSOR,
    PSU1_THERMAL_SENSOR_1,
    PSU2_THERMAL_SENSOR_1,
    NUM_OF_CHASSIS_THERMAL_SENSOR = BCM56846_LOCAL_SENSOR,
};

/* ── Fan ───────────────────────────────────────────────────── */

enum fan_duty_cycle {
    FAN_PERCENTAGE_MIN = 40,
    FAN_PERCENTAGE_MID = 70,
    FAN_PERCENTAGE_MAX = 100,
};

/* CPLD fan speed register values */
#define FAN_CPLD_VAL_MIN   4   /* ~40% */
#define FAN_CPLD_VAL_MID   7   /* ~70% */
#define FAN_CPLD_VAL_MAX   10  /* ~100% */

/* ── LED IDs ───────────────────────────────────────────────── */

enum led_id {
    LED_DIAG = 1,
    LED_FAN,
    LED_LOC,
    LED_PSU1,
    LED_PSU2,
};

#define TEMPERATURE_MULTIPLIER 1000

/* ── Port constants ────────────────────────────────────────── */
#define NUM_SFP_PORTS   48
#define NUM_QSFP_PORTS  4
#define NUM_TOTAL_PORTS 52

/* I2C bus for port N (1-based): SFP = 21+N, QSFP 49-52 = 18-21 */
#define SFP_PORT_TO_I2C_BUS(port)  (21 + (port))
#define QSFP_PORT_TO_I2C_BUS(port) ((port) - 49 + 18)

/* I2C addresses */
#define I2C_ADDR_SFP_EEPROM    0x50
#define I2C_ADDR_QSFP_EEPROM   0x50
#define I2C_ADDR_RETIMER       0x27

/* GPIO expander buses and addresses */
#define I2C_BUS_GPIO_CTRL_1     16
#define I2C_BUS_GPIO_CTRL_2     17
#define I2C_ADDR_SFP_MOD_ABS    0x20
#define I2C_ADDR_SFP_TX_FAULT   0x21
#define I2C_ADDR_SFP_RX_LOS     0x22
#define I2C_ADDR_SFP_MIXED      0x23
#define I2C_ADDR_SFP_TX_DISABLE 0x24

/* PCA9506 register offsets */
#define PCA9506_REG_INPUT_BASE  0x00
#define PCA9506_REG_OUTPUT_BASE 0x08
#define PCA9506_REG_IOC_BASE    0x18

#endif /* __PLATFORM_LIB_H__ */
