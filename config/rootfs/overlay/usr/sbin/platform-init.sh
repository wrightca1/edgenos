#!/bin/bash
# platform-init.sh - Load platform modules and initialize hardware
set -e

log() { echo "platform-init: $*"; logger -t platform-init "$*"; }

log "Starting platform initialization..."

# Load CPLD driver
if [ -f /lib/modules/extra/accton_as5610_52x_cpld.ko ]; then
    log "Loading CPLD driver..."
    insmod /lib/modules/extra/accton_as5610_52x_cpld.ko
fi

# Load retimer class and driver
if [ -f /lib/modules/extra/retimer_class.ko ]; then
    log "Loading retimer drivers..."
    insmod /lib/modules/extra/retimer_class.ko
    insmod /lib/modules/extra/ds100df410.ko
fi

# Load I2C mux drivers (should be built-in, but just in case)
modprobe i2c-mux-pca954x 2>/dev/null || true
modprobe at24 2>/dev/null || true
modprobe max6697 2>/dev/null || true

# Initialize QSFP GPIOs (reset high, modsel low, lpmode low)
# GPIOs 160-171 on PCA9506 expanders
for gpio in $(seq 160 171); do
    if [ -d "/sys/class/gpio/gpio${gpio}" ]; then
        echo out > "/sys/class/gpio/gpio${gpio}/direction"
        echo 1 > "/sys/class/gpio/gpio${gpio}/value"  # RST_L high (not reset)
    fi
done

# Enable SFP TX (via PCA9506 at 0x24 on i2c-1)
# TX_DISABLE pins are active-high, set low to enable
if [ -d "/sys/class/gpio/gpiochip144" ]; then
    for gpio in $(seq 144 159); do
        echo out > "/sys/class/gpio/gpio${gpio}/direction" 2>/dev/null || true
        echo 0 > "/sys/class/gpio/gpio${gpio}/value" 2>/dev/null || true
    done
fi

# Load BDE kernel modules
if [ -f /lib/modules/extra/linux-kernel-bde.ko ]; then
    log "Loading BDE kernel modules..."
    insmod /lib/modules/extra/linux-kernel-bde.ko maxpayload=128
    insmod /lib/modules/extra/linux-user-bde.ko
fi

log "Platform initialization complete."
