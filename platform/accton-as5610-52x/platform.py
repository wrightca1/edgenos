"""EdgeNOS platform class for the Accton AS5610-52X (ONL OnlPlatform analog).

Encodes the board's hardware bring-up declaratively: the driver load order (with
params) and the post-driver init phases. Mirrors the proven sequence in
services/platform-init.sh (itself a replica of the Cumulus boot order).
"""
import os
import sys

# import the shared framework from core/platform/
_CORE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "core", "platform")
sys.path.insert(0, os.path.abspath(_CORE))
from base import EdgeNOSPlatformBase, PortConfig_52x10_4x40   # noqa: E402

# where the board services/scripts land in the installed rootfs
_SVC = "/usr/lib/edgenos/platform"


class EdgeNOSPlatform_powerpc_accton_as5610_52x_r0(EdgeNOSPlatformBase, PortConfig_52x10_4x40):
    PLATFORM = "powerpc-accton_as5610_52x-r0"     # == switch DB key
    MODEL = "AS5610-52X"
    SYS_OBJECT_ID = ".5610.52"

    # Driver load order (== platform-init.sh; the Cumulus boot order):
    #   ASIC BDE first (owns BAR0 + DMA pool), then user-BDE, die-temp, TUN,
    #   CPLD, EEPROM, GPIO expanders, temp sensors, retimer.
    DRIVERS = [
        ("linux-kernel-bde", "dma_size=64"),       # ASIC PCI driver + DMA pool
        ("linux-user-bde", ""),                    # userspace BDE interface
        ("linux-bde-tmon", ""),                    # chip die-temp sensor
        ("tun", ""),                               # packet I/O (52 ports)
        ("accton_as5610_52x_cpld", ""),            # LEDs/PSU/fan/watchdog
        ("at24", ""),                              # board + SFP/QSFP EEPROMs
        ("gpio-pca953x", ""),                      # SFP/QSFP control GPIOs
        ("max6697", ""),                           # 7-ch temp sensor
        ("adm1021", ""),                           # 2-ch temp sensor
        ("retimer_class", ""),                     # retimer sysfs class
        ("ds100df410", ""),                        # 40G retimer/equalizer (x32)
    ]

    # Post-driver bring-up phases (GPIO/retimer/fan), wrapping the board scripts.
    INIT_SCRIPTS = [
        f"{_SVC}/retimer-init.sh",
        f"{_SVC}/sfp-enable.sh",
        f"{_SVC}/fan-controller.sh",
    ]

    # --- HAL: a couple of board specifics filled in; rest inherit HALUnsupported ---
    def fan_count(self):
        return 4

    def psu_count(self):
        return 2
