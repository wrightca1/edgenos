"""EdgeNOS platform class for the Arista DCS-7150S-52 (FM6000 / "raven"/Santa Rosa).

First Arista board and first **SCD-FPGA** platform (the Broadcom boards use a
CPLD). Board control is the SCD FPGA (PCI 3475:0001): LEDs, resets, GPIO, the
SMBus/i2c masters, and the per-cage SFP transceiver controllers all hang off it,
driven by the GPL `scd` + `scd-hwmon` drivers (our wrightca1/sonic fork). The
mgmt NIC is tg3; sensors/PSU/fans are mainline hwmon chips on the SCD i2c buses.

Provenance: arista RE repo edgenos/PLATFORM.md + edgenos/SCD.md + Phase-3i/Phase-11
live captures. Confirmed SCD register blocks are hard-coded below; the per-cage
i2c/xcvr addresses are written by services/scd-setup.sh (filled from a live
i2cdetect on first bring-up — the per-SKU raven config is data-driven in EOS).
"""
import glob
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
for _p in (_HERE, os.path.abspath(os.path.join(_HERE, "..", "..", "core", "platform"))):
    if _p not in sys.path:
        sys.path.insert(0, _p)
from base import EdgeNOSPlatformBase, make_port_config   # noqa: E402

_SVC = "/usr/lib/edgenos/platform"

# 7150S-52 = 52x SFP+ (10G), no 40G. Front-panel Ethernet1..52.
PortConfig_52x10 = make_port_config("PortConfig_52x10", [("et", 52, 10)])


class EdgeNOSPlatform_x86_64_arista_7150s_52_r0(EdgeNOSPlatformBase, PortConfig_52x10):
    PLATFORM = "x86_64-arista_7150s_52-r0"       # == switch DB key
    MODEL = "DCS-7150S-52"
    SYS_OBJECT_ID = ".7150.52"

    # Driver load order: mgmt NIC first, then the SCD (owns board control +
    # spawns the i2c adapters), then hwmon chips on those buses, then TUN.
    DRIVERS = [
        ("tg3", ""),                    # BCM5719 mgmt NIC (ma1)
        ("scd", ""),                    # SCD FPGA (PCI 3475:0001): BAR0/IRQ/PTP/wdog
        ("scd-hwmon", ""),              # component driver: the new_<object> sysfs API
        ("raven-fan-driver", ""),       # SB800 fan glue (raven)
        ("max31790", ""),               # fan controller/tach (hwmon)
        ("ucd9000", ""),                # UCD9012 PSU/rail monitor (PMBus hwmon)
        ("at24", ""),                   # board prefdl + SFP EEPROMs
        ("lm90", ""),                   # max6658-family temp
        ("lm75", ""),                   # temp
        ("tun", ""),                    # CPU-port punt netdev (edged-7150)
    ]

    # Post-driver bring-up: declare the SCD board blocks (LEDs/resets/i2c/xcvrs),
    # then enable SFP lasers.
    INIT_SCRIPTS = [
        f"{_SVC}/scd-setup.sh",
        f"{_SVC}/sfp-enable.sh",
    ]

    # --- confirmed SCD register blocks (Phase-3i / Bodega.py; edgenos/SCD.md) ---
    SCD_LED_BLOCK = (0x5010, 0x5340, 0x10)     # port LEDs, step 0x10
    SCD_LED_SYS   = 0x6940
    SCD_RESET_GPO = 0x4000                     # set +0x00 / clear +0x10
    SCD_INTR      = (0x3000, 0x3030, 0x3060)   # maskSet+0 / maskClear+0x10 / status+0x20

    # scd-led registers standard Linux LED-class devices; scd-xcvr exposes per-cage
    # {present,txdisable,rxlos,txfault} nodes; at24 gives the SFF eeprom at 0x50.
    LED_DIR      = "sys/class/leds"                       # <name>/brightness
    XCVR_GLOB    = "sys/bus/platform/devices/scd-xcvr*/sfp*"   # per-cage dir (confirm on bring-up)
    SFP_EEPROMS  = "sys/bus/i2c/devices/*-0050/eeprom"    # 52 SFP EEPROMs on scd i2c muxes

    def fan_count(self):
        return 4

    def psu_count(self):
        return 2

    # ---- LEDs: scd-led standard LED class ---------------------------------
    def leds(self):
        out = {}
        for path in glob.glob(os.path.join(self.root, self.LED_DIR, "*")):
            name = os.path.basename(path)
            out[name] = self._read_int(os.path.join(self.LED_DIR, name, "brightness"))
        return out

    def led_set(self, name, state):
        return self._write(os.path.join(self.LED_DIR, name, "brightness"), int(state))

    # ---- SFP: present + tx-disable via scd-xcvr (base gives EEPROM/DOM) ----
    def _xcvr_node(self, port):
        """Return the scd-xcvr sysfs dir for front-panel Ethernet<port> (1..52).
        Exact node naming is confirmed on first bring-up; degrade to None."""
        hits = sorted(glob.glob(os.path.join(self.root, self.XCVR_GLOB)))
        idx = port - 1
        return hits[idx] if 0 <= idx < len(hits) else None

    def sfps(self):
        """Per-port presence + tx state from scd-xcvr, merged with base optics."""
        out = []
        for port in range(1, self.port_count() + 1):
            node = self._xcvr_node(port)
            present = txdis = None
            if node:
                present = self._read_int(os.path.relpath(os.path.join(node, "present"), self.root))
                txdis   = self._read_int(os.path.relpath(os.path.join(node, "txdisable"), self.root))
            out.append({"port": port, "present": present, "txdisable": txdis})
        return out

    def sfp_set_tx(self, port, on):
        """Enable/disable a laser. No unsupported-transceiver gate under our own
        scd-xcvr — any SFP's TX turns on (the whole enable3px saga was EOS policy)."""
        node = self._xcvr_node(port)
        if not node:
            return False
        rel = os.path.relpath(os.path.join(node, "txdisable"), self.root)
        return self._write(rel, 0 if on else 1)   # txdisable=0 => laser on

    # ---- PSU / fans: mainline hwmon (ucd9000 / max31790) ------------------
    def _hwmon_by_name(self, want):
        for h in glob.glob(os.path.join(self.root, "sys/class/hwmon/hwmon*")):
            nm = self._read(os.path.relpath(os.path.join(h, "name"), self.root))
            if nm and nm.strip() == want:
                return h
        return None

    def psus(self):
        h = self._hwmon_by_name("ucd9012")
        out = []
        for pid in range(1, self.psu_count() + 1):
            if not h:
                out.append({"id": pid, "present": None, "ok": None})
                continue
            # UCD9012 monitors PSU rails; presence/OK derive from a rail's input.
            v = self._read_int(os.path.relpath(
                os.path.join(h, "in%d_input" % pid), self.root))
            out.append({"id": pid, "present": int(v is not None), "ok": int(bool(v))})
        return out

    def fans(self):
        h = self._hwmon_by_name("max31790")
        out = []
        for fid in range(1, self.fan_count() + 1):
            rpm = None
            if h:
                rpm = self._read_int(os.path.relpath(
                    os.path.join(h, "fan%d_input" % fid), self.root))
            out.append({"id": fid, "present": int(rpm is not None), "rpm": rpm})
        return out
