"""EdgeNOS platform class for the Accton AS5610-52X (ONL OnlPlatform analog).

Encodes the board's hardware bring-up declaratively: the driver load order (with
params) and the post-driver init phases. Mirrors the proven sequence in
services/platform-init.sh (itself a replica of the Cumulus boot order).
"""
import os
import re
import sys

# import the shared framework — works both in-tree (core/platform/) and installed
# (base.py lands alongside this file under /usr/lib/edgenos/platform/).
_HERE = os.path.dirname(os.path.abspath(__file__))
for _p in (_HERE, os.path.abspath(os.path.join(_HERE, "..", "..", "core", "platform"))):
    if _p not in sys.path:
        sys.path.insert(0, _p)
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

    # --- HAL: real board reads via the CPLD driver's sysfs (never devmem) ---
    CPLD = "sys/devices/platform/as5610_52x_cpld"   # driver sysfs base (root-relative)
    SFP_EEPROMS = "sys/bus/i2c/devices/*-0050/eeprom"   # 48 SFP + 4 QSFP via at24 on muxed buses

    # --- SFP/QSFP i2c-bus -> front-panel port map (verified live on the DTS mux tree,
    #     2026-07-20; see docs/hal-transceivers.md). The kernel enumerates the mux
    #     children contiguously, so the bus number is a pure function of the port:
    #       SFP  1-48 : PCA9546@0x75 (ch0-3 = ports 1-32) / @0x76 (ch0-1 = ports 33-48)
    #                   -> PCA9548@0x74 (ch0-7) => bus = 11 + 9*((p-1)//8) + ((p-1)%8)
    #       QSFP 49-52: PCA9546@0x77 (ch0-3)             => bus = 66 + (p-49)
    #     e.g. swp1=bus11, swp6=bus16, swp9=bus20, swp48=bus63, swp49(QSFP)=bus66.
    #     NB: the legacy onlp/sfpi.c "21+port" macro is STALE and does NOT match this.
    def _sfp_port_for_bus(self, bus):
        for p in range(1, 49):
            if 11 + 9 * ((p - 1) // 8) + ((p - 1) % 8) == bus:
                return "swp%d" % p
        if 66 <= bus <= 69:
            return "swp%d" % (49 + (bus - 66))
        return None

    def fan_count(self):
        return 4

    def psu_count(self):
        return 2

    def fans(self):
        # CPLD exposes board-wide fan status + a single PWM (no per-fan tach).
        status = self._read_int("/%s/fan_status" % self.CPLD)
        pwm = self._read_int("/%s/fan_pwm" % self.CPLD)
        return [{"id": i + 1, "fan_status_raw": status, "pwm": pwm} for i in range(self.fan_count())]

    def fan_set(self, pct):
        # fan_pwm is 0..255 on this CPLD
        return self._write("/%s/fan_pwm" % self.CPLD, max(0, min(255, int(pct * 255 / 100))))

    def _cpld_regs(self):
        """Parse the CPLD driver's `reg` dump node -> {offset: value}."""
        regs = {}
        for line in (self._read("/%s/reg" % self.CPLD) or "").splitlines():
            m = re.match(r"\s*([0-9a-fA-F]+):\s*(.*)", line)
            if not m:
                continue
            base = int(m.group(1), 16)
            for i, tok in enumerate(m.group(2).split()):
                try:
                    regs[base + i] = int(tok, 16)
                except ValueError:
                    break
        return regs

    def psus(self):
        # The migrated cpld driver mis-decodes PSU (all bits in 0x01, active-high).
        # Use the Cumulus-proven map instead: PSU1 status in reg 0x02, PSU2 in 0x01;
        # present is ACTIVE-LOW (bit0=0 => present), power-good = bit1.
        r = self._cpld_regs()
        out = []
        for pid, reg in ((1, 0x02), (2, 0x01)):
            v = r.get(reg)
            if v is None:
                out.append({"id": pid, "present": None, "ok": None})
            else:
                out.append({"id": pid, "present": int((v & 0x01) == 0), "ok": int((v & 0x02) != 0)})
        return out

    def leds(self):
        return {n: self._read("/%s/%s" % (self.CPLD, n)) for n in ("led_sys", "led_loc")}

    def led_set(self, name, state):
        if name not in ("led_sys", "led_loc"):
            raise ValueError("unknown LED %r (have led_sys, led_loc)" % name)
        return self._write("/%s/%s" % (self.CPLD, name), state)

    def cpld_version(self):
        return self._read("/%s/version" % self.CPLD)
