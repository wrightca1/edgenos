/*
 * platform_lib.c - Platform library for AS5610-52X
 *
 * CPLD memory-mapped I/O, I2C access, and PMBus parsing.
 * Adapted from ONL (Copyright 2014 Big Switch Networks / Accton Technology)
 *
 * SPDX-License-Identifier: EPL-1.0
 */

#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

#include "platform_lib.h"

#define I2C_BUFFER_MAXSIZE 32
#define PMBUS_LITERAL_DATA_MULTIPLIER 1000

/* ── Memory-mapped I/O for CPLD ──────────────────────────────── */

static int mem_read(unsigned int base, unsigned int offset, uint8_t *val)
{
    int fd;
    void *mem;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
        return -1;

    mem = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE,
               MAP_SHARED, fd, base);
    if (mem == MAP_FAILED) {
        close(fd);
        return -1;
    }

    *val = *((volatile uint8_t *)(mem + offset));

    munmap(mem, getpagesize());
    close(fd);
    return 0;
}

static int mem_write(unsigned int base, unsigned int offset, uint8_t val)
{
    int fd;
    void *mem;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
        return -1;

    mem = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE,
               MAP_SHARED, fd, base);
    if (mem == MAP_FAILED) {
        close(fd);
        return -1;
    }

    *((volatile uint8_t *)(mem + offset)) = val;

    munmap(mem, getpagesize());
    close(fd);
    return 0;
}

int cpld_read(unsigned int reg_offset, uint8_t *val)
{
    return mem_read(CPLD_BASE_ADDRESS, reg_offset, val);
}

int cpld_write(unsigned int reg_offset, uint8_t val)
{
    return mem_write(CPLD_BASE_ADDRESS, reg_offset, val);
}

/* ── I2C access ──────────────────────────────────────────────── */

static int i2c_open(unsigned int bus_id, uint8_t i2c_addr)
{
    char fname[32];
    int fd;

    snprintf(fname, sizeof(fname), "/dev/i2c-%u", bus_id);
    fd = open(fname, O_RDWR);
    if (fd < 0)
        return -1;

    if (ioctl(fd, I2C_SLAVE_FORCE, i2c_addr) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int i2c_read(unsigned int bus_id, uint8_t i2c_addr,
             uint8_t offset, uint8_t *buf)
{
    int fd = i2c_open(bus_id, i2c_addr);
    if (fd < 0)
        return -1;

    if (write(fd, &offset, 1) != 1) {
        close(fd);
        return -1;
    }

    if (read(fd, buf, 1) != 1) {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int i2c_write(unsigned int bus_id, uint8_t i2c_addr,
              uint8_t offset, uint8_t *buf)
{
    uint8_t sendbuf[2];
    int fd = i2c_open(bus_id, i2c_addr);
    if (fd < 0)
        return -1;

    sendbuf[0] = offset;
    sendbuf[1] = *buf;

    if (write(fd, sendbuf, 2) != 2) {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int i2c_nRead(unsigned int bus_id, uint8_t i2c_addr,
              uint8_t offset, unsigned int size, uint8_t *buf)
{
    int fd = i2c_open(bus_id, i2c_addr);
    if (fd < 0)
        return -1;

    if (write(fd, &offset, 1) != 1) {
        close(fd);
        return -1;
    }

    if (read(fd, buf, size) != (ssize_t)size) {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int i2c_nWrite(unsigned int bus_id, uint8_t i2c_addr,
               uint8_t offset, unsigned int size, uint8_t *buf)
{
    uint8_t sendbuf[I2C_BUFFER_MAXSIZE + 1];
    int fd;

    if (size > I2C_BUFFER_MAXSIZE)
        return -1;

    fd = i2c_open(bus_id, i2c_addr);
    if (fd < 0)
        return -1;

    sendbuf[0] = offset;
    memcpy(sendbuf + 1, buf, size);

    if (write(fd, sendbuf, size + 1) != (ssize_t)(size + 1)) {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int as5610_52x_i2c0_pca9548_channel_set(uint8_t channel)
{
    /* Force-write to PCA9548 mux at 0x70 on bus 0 */
    int fd = i2c_open(0, 0x70);
    if (fd < 0)
        return -1;

    if (write(fd, &channel, 1) != 1) {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

/* ── PMBus parsing ───────────────────────────────────────────── */

int two_complement_to_int(uint16_t data, uint8_t valid_bit, uint16_t mask)
{
    uint16_t valid_data = data & mask;
    int is_negative = valid_data >> (valid_bit - 1);

    return is_negative ? (-(((~valid_data) & mask) + 1)) : valid_data;
}

int pmbus_parse_literal_format(uint16_t val)
{
    int exponent = two_complement_to_int(val >> 11, 5, 0x1f);
    int mantissa = two_complement_to_int(val & 0x7ff, 11, 0x7ff);

    if (exponent >= 0)
        return (mantissa << exponent) * PMBUS_LITERAL_DATA_MULTIPLIER;
    else
        return (mantissa * PMBUS_LITERAL_DATA_MULTIPLIER) / (1 << -exponent);
}

int pmbus_read_literal_data(uint8_t bus, uint8_t i2c_addr,
                            uint8_t reg, int *val)
{
    uint8_t buf[2] = {0};
    *val = 0;

    if (i2c_nRead(bus, i2c_addr, reg, sizeof(buf), buf) != 0)
        return -1;

    *val = pmbus_parse_literal_format((uint16_t)buf[1] << 8 | buf[0]);
    return 0;
}

int pmbus_read_vout_data(uint8_t bus, uint8_t i2c_addr, int *val)
{
    uint8_t vout_mode = 0;
    uint8_t vout_val[2] = {0};
    *val = 0;

    if (i2c_nRead(bus, i2c_addr, 0x20, 1, &vout_mode) != 0)
        return -1;

    if (i2c_nRead(bus, i2c_addr, 0x8B, 2, vout_val) != 0)
        return -1;

    int exponent = two_complement_to_int(vout_mode, 5, 0x1f);
    int mantissa = (uint16_t)vout_val[1] << 8 | vout_val[0];

    if (exponent >= 0)
        *val = (mantissa << exponent) * PMBUS_LITERAL_DATA_MULTIPLIER;
    else
        *val = (mantissa * PMBUS_LITERAL_DATA_MULTIPLIER) / (1 << -exponent);

    return 0;
}

/* ── PSU type detection ──────────────────────────────────────── */

#define I2C_PSU_MODEL_NAME_LEN 13
#define I2C_AC_PSU1_EEPROM     0x3A
#define I2C_AC_PSU2_EEPROM     0x39
#define I2C_CPR_4011_MODEL_REG 0x26

as5610_52x_psu_type_t as5610_52x_get_psu_type(int id, char *modelname,
                                               int modelname_len)
{
    as5610_52x_psu_type_t ret = PSU_TYPE_UNKNOWN;
    uint8_t mux_ch[] = {0x02, 0x04};  /* PSU1=ch1, PSU2=ch2 */
    char model_name[I2C_PSU_MODEL_NAME_LEN + 1] = {0};
    uint8_t ac_eeprom[] = {I2C_AC_PSU1_EEPROM, I2C_AC_PSU2_EEPROM};

    /* Select PSU mux channel */
    if (as5610_52x_i2c0_pca9548_channel_set(mux_ch[id - 1]) != 0)
        return PSU_TYPE_UNKNOWN;

    /* Try AC PSU EEPROM */
    if (i2c_nRead(0, ac_eeprom[id - 1], I2C_CPR_4011_MODEL_REG,
                  I2C_PSU_MODEL_NAME_LEN, (uint8_t *)model_name) == 0) {
        model_name[I2C_PSU_MODEL_NAME_LEN] = '\0';

        if (strncmp(model_name, "CPR-4011-4M11", 13) == 0)
            ret = PSU_TYPE_AC_F2B;
        else if (strncmp(model_name, "CPR-4011-4M21", 13) == 0)
            ret = PSU_TYPE_AC_B2F;
    }

    /* Close mux channel */
    as5610_52x_i2c0_pca9548_channel_set(0);

    if (modelname && modelname_len > 0)
        snprintf(modelname, modelname_len, "%s", model_name);

    return ret;
}
