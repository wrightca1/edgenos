"""EdgeNOS platform class for the Accton AS4610-54T (ONL OnlPlatform analog).

Helix4 (BCM56340) / ARM iProc. Datapath = bcmd (OpenBCM). The 6.1 own-build keeps
the datapath under /opt/edgenos (modules included), loaded with the params from
bcmd-prep.sh; this class encodes that declaratively.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
for _p in (_HERE, os.path.abspath(os.path.join(_HERE, "..", "..", "core", "platform"))):
    if _p not in sys.path:
        sys.path.insert(0, _p)
from base import EdgeNOSPlatformBase, PortConfig_48x1_4x10   # noqa: E402


class EdgeNOSPlatform_arm_accton_as4610_54_r0(EdgeNOSPlatformBase, PortConfig_48x1_4x10):
    PLATFORM = "arm-accton-as4610-54-r0"          # == switch DB key
    MODEL = "AS4610-54"
    SYS_OBJECT_ID = ".4610.54"

    # 6.1 own-build keeps datapath .ko under /opt/edgenos (not /lib/modules).
    MODULE_DIRS = ["opt/edgenos"]
    DRIVERS = [
        ("linux-kernel-bde", "dmasize=8M"),       # iProc CMICd; smaller DMA pool than the 5610
        ("linux-user-bde", ""),
        ("linux-bcm-knet", "default_mtu=1600"),   # KNET RX/TX punt (match far-side MTU)
    ]
    # ONLP platform layer for this board comes from ONL (onlp component).
    INIT_SCRIPTS = []                             # bcmd.service runs bcmd-prep.sh at start

    # SFP+/QSFP optics via bound eeprom sysfs (SFF decode in the base HAL).
    SFP_EEPROMS = "sys/bus/i2c/devices/*-0050/eeprom"

    # Native fan/PSU via the board CPLD over i2c (no kernel driver on 6.1 Buildroot;
    # the 4.14 ONL accton_as4610_cpld.ko isn't loaded). Register map from ONL's
    # accton_as4610_{fan,psu}.c: CPLD at bus 0 / 0x30.
    CPLD_BUS, CPLD_ADDR = 0, 0x30
    _FAN_RPM_REG = {1: 0x2d, 2: 0x2c}    # raw tach
    _FAN_PWM_REG = 0x2b                   # duty (all fans)
    _PSU_STATUS_REG = 0x11               # present/pg bits, per PSU at (i*2, i*2+1)

    def fan_count(self):
        return 2

    def psu_count(self):
        return 2

    @staticmethod
    def _rpm(raw):                       # ONL: rpm = raw * 379 * 60 / 2 / 100
        return None if raw is None else raw * 379 * 60 // 2 // 100

    def fans(self):
        pwm = self._i2c_read(self.CPLD_BUS, self.CPLD_ADDR, self._FAN_PWM_REG)
        duty = ((pwm & 0xF) * 125 + 5) // 10 if pwm is not None else None
        return [{"id": i, "rpm": self._rpm(self._i2c_read(self.CPLD_BUS, self.CPLD_ADDR, r)),
                 "duty_pct": duty} for i, r in sorted(self._FAN_RPM_REG.items())]

    def psus(self):
        st = self._i2c_read(self.CPLD_BUS, self.CPLD_ADDR, self._PSU_STATUS_REG)
        return [{"id": i + 1,
                 "present": None if st is None else (st >> (i * 2)) & 1,
                 "ok": None if st is None else (st >> (i * 2 + 1)) & 1}
                for i in range(self.psu_count())]

    # thermals() comes from the generic hwmon base implementation.
