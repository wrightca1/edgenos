#!/bin/bash
# i2c_init.sh - I2C device instantiation helper
#
# Creates I2C device entries for devices that don't have
# device tree entries or need manual instantiation.
# Run after kernel I2C mux drivers are loaded.

log() { logger -t i2c-init "$*"; echo "i2c-init: $*"; }

log "Starting I2C device initialization..."

# Instantiate SFP EEPROMs on all port buses (22-69)
for bus in $(seq 22 69); do
    if [ -d "/sys/bus/i2c/devices/i2c-${bus}" ]; then
        # Only instantiate if not already present
        if [ ! -d "/sys/bus/i2c/devices/${bus}-0050" ]; then
            echo "24c04 0x50" > "/sys/bus/i2c/devices/i2c-${bus}/new_device" 2>/dev/null || true
        fi
    fi
done

# Instantiate QSFP EEPROMs on buses 18-21
for bus in 18 19 20 21; do
    if [ -d "/sys/bus/i2c/devices/i2c-${bus}" ]; then
        if [ ! -d "/sys/bus/i2c/devices/${bus}-0050" ]; then
            echo "sff8436 0x50" > "/sys/bus/i2c/devices/i2c-${bus}/new_device" 2>/dev/null || true
        fi
    fi
done

# Instantiate thermal sensors on bus 9
if [ -d "/sys/bus/i2c/devices/i2c-9" ]; then
    if [ ! -d "/sys/bus/i2c/devices/9-004d" ]; then
        echo "max6697 0x4d" > "/sys/bus/i2c/devices/i2c-9/new_device" 2>/dev/null || true
    fi
    if [ ! -d "/sys/bus/i2c/devices/9-0018" ]; then
        echo "adm1021 0x18" > "/sys/bus/i2c/devices/i2c-9/new_device" 2>/dev/null || true
    fi
fi

# Instantiate PSU EEPROMs on buses 3 and 4
for entry in "3 0x3a" "4 0x39"; do
    bus=$(echo "$entry" | cut -d' ' -f1)
    addr=$(echo "$entry" | cut -d' ' -f2)
    if [ -d "/sys/bus/i2c/devices/i2c-${bus}" ]; then
        if [ ! -d "/sys/bus/i2c/devices/${bus}-00$(echo $addr | cut -c3-4)" ]; then
            echo "24c02 $addr" > "/sys/bus/i2c/devices/i2c-${bus}/new_device" 2>/dev/null || true
        fi
    fi
done

log "I2C device initialization complete."
