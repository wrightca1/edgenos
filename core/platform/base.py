"""EdgeNOS platform framework — ONL-inspired, per-switch.

Mirrors ONL's OnlPlatformBase: each board subclasses EdgeNOSPlatformBase, sets
PLATFORM/MODEL, mixes in a port profile, and implements baseconfig() to bring the
hardware up (load drivers in order, then board init phases). An ONLP-style HAL
(sfp/fan/psu/led/thermal) is the seam a board implements for hardware queries.

Unlike ONL (one per-arch image, board detected among many at boot), EdgeNOS ships
one image PER SWITCH — so an image has exactly one platform class — but the structure
is identical, which keeps every board uniform and the HAL board-agnostic.
"""
import os
import glob
import subprocess


class HALUnsupported(NotImplementedError):
    """Raised by HAL methods a board hasn't implemented yet."""


class PlatformHAL:
    """ONLP-analog hardware abstraction (sensors/fans/PSUs/SFPs/LEDs).

    Boards implement what they support; unimplemented queries raise HALUnsupported.
    thermals() has a generic hwmon-based default (works on any board with hwmon).
    All reads are sysfs-based (never devmem) and degrade gracefully off-hardware.
    """
    def thermals(self):  raise HALUnsupported   # [{name, celsius}]
    def fans(self):      raise HALUnsupported   # [{id, present, pwm, ...}]
    def psus(self):      raise HALUnsupported   # [{id, present, ok}]
    def sfps(self):      raise HALUnsupported   # [{port, present}]
    def leds(self):      raise HALUnsupported   # {name: state}
    def led_set(self, name, state): raise HALUnsupported
    # simple counts (boards may set as constants)
    def fan_count(self): raise HALUnsupported
    def psu_count(self): raise HALUnsupported


# --- port profiles (ONL OnlPlatformPortConfig analog) -----------------------
class PortConfig:
    PORTS = []                      # list of (name, speed_gbps)

    @classmethod
    def port_count(cls):
        return len(cls.PORTS)


def make_port_config(name, specs):
    """specs: [(prefix, count, speed_gbps), ...] -> a PortConfig subclass."""
    ports = []
    for prefix, count, speed in specs:
        ports += [(f"{prefix}{i}", speed) for i in range(count)]
    return type(name, (PortConfig,), {"PORTS": ports})


PortConfig_52x10_4x40 = make_port_config("PortConfig_52x10_4x40",
                                         [("xe", 52, 10), ("ce", 4, 40)])
PortConfig_48x1_4x10 = make_port_config("PortConfig_48x1_4x10",
                                        [("ge", 48, 1), ("xe", 4, 10)])


# --- the platform base (ONL OnlPlatformBase analog) -------------------------
class EdgeNOSPlatformBase(PlatformHAL):
    PLATFORM = None                 # ONIE platform string (matches switch DB key)
    MODEL = None
    SYS_OBJECT_ID = None
    DRIVERS = []                    # [(module, params), ...] loaded in order by baseconfig()
    INIT_SCRIPTS = []              # board bring-up scripts run after drivers
    MODULE_DIRS = []               # extra dirs (rel to root) to find .ko, e.g. ["opt/edgenos"]

    def __init__(self, root="/"):
        self.root = root

    # -- helpers (ONL insmod() analog) --
    def _modpath(self, mod):
        dirs = [f"lib/modules/{os.uname().release}/extra", "usr/lib/modules/extra"]
        dirs += list(self.MODULE_DIRS)
        for d in dirs:
            p = os.path.join(self.root, d, mod + ".ko")
            if os.path.exists(p):
                return p
        return None

    def insmod(self, mod, params=""):
        p = self._modpath(mod)
        if p is None and self.root != "/":
            print(f"platform: skip {mod} {params} (staging — no module present)".rstrip())
            return True
        cmd = (["insmod", p] if p else ["modprobe", mod]) + params.split()
        rc = subprocess.call(cmd)
        print(f"platform: {'loaded' if rc == 0 else 'WARN failed'} {mod} {params}".rstrip())
        return rc == 0

    def run_script(self, relpath):
        full = os.path.join(self.root, relpath.lstrip("/"))
        if not os.path.exists(full):
            print(f"platform: missing init script {relpath}")
            return False
        return subprocess.call(["sh", full]) == 0

    # -- the baseconfig() contract --
    def baseconfig(self):
        """Load drivers in order, then run board init scripts. Override to extend."""
        for mod, params in self.DRIVERS:
            self.insmod(mod, params)
        ok = True
        for s in self.INIT_SCRIPTS:
            ok = self.run_script(s) and ok
        return ok

    def info(self):
        return {
            "platform": self.PLATFORM,
            "model": self.MODEL,
            "sys_object_id": self.SYS_OBJECT_ID,
            "ports": self.port_count() if hasattr(self, "PORTS") else None,
            "drivers": [m for m, _ in self.DRIVERS],
            "init_scripts": list(self.INIT_SCRIPTS),
        }

    # -- HAL helpers (sysfs, root-relative, graceful off-hardware) --
    def _read(self, relpath):
        try:
            with open(os.path.join(self.root, relpath.lstrip("/")), encoding="utf-8") as f:
                return f.read().strip()
        except OSError:
            return None

    def _read_int(self, relpath):
        v = self._read(relpath)
        if v is None:
            return None
        try:
            return int(v, 0)
        except ValueError:
            return None

    def _write(self, relpath, value):
        try:
            with open(os.path.join(self.root, relpath.lstrip("/")), "w", encoding="utf-8") as f:
                f.write(str(value))
            return True
        except OSError:
            return False

    def _i2c_read(self, bus, addr, reg):
        """Read one byte via the `i2cget` CLI (for CPLDs with no kernel sysfs driver).
        Returns int or None. Only meaningful on the box; None off-hardware."""
        try:
            out = subprocess.run(["i2cget", "-y", str(bus), hex(addr), hex(reg)],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 timeout=5).stdout.decode().strip()
            return int(out, 16) if out else None
        except Exception:
            return None

    # SFP/QSFP optic decode (SFF-8472 / SFF-8636) from bound i2c `eeprom` sysfs nodes.
    SFP_EEPROMS = None      # board sets a glob (root-relative), e.g. "sys/bus/i2c/devices/*-0050/eeprom"
    _SFF_ID = {0x03: "SFP", 0x0b: "DWDM-SFP", 0x0c: "QSFP", 0x0d: "QSFP+",
               0x11: "QSFP28", 0x18: "QSFP-DD"}

    @staticmethod
    def _sff_str(buf, lo, hi):
        return buf[lo:hi].decode("ascii", "replace").strip().strip("\x00").strip() or None

    def _decode_optic(self, data):
        """Decode an SFP(A0)/QSFP eeprom image -> dict, or None if not an optic."""
        if not data:
            return None
        idt = data[0]
        kind = self._SFF_ID.get(idt)
        if kind is None:
            return None                         # not an SFF module (e.g. board-id eeprom)
        if idt in (0x0c, 0x0d, 0x11, 0x18) and len(data) >= 212:   # QSFP (SFF-8636)
            v, p, s = (148, 164), (168, 184), (196, 212)
        elif len(data) >= 84:                                       # SFP (SFF-8472)
            v, p, s = (20, 36), (40, 56), (68, 84)
        else:
            return {"type": kind}
        return {"type": kind, "vendor": self._sff_str(data, *v),
                "part": self._sff_str(data, *p), "serial": self._sff_str(data, *s)}

    def sfps(self):
        """Inventory present optics by reading bound eeprom sysfs nodes + SFF decode.
        Keyed by i2c bus (a real per-port handle; bus->front-port labeling is future)."""
        if not self.SFP_EEPROMS:
            raise HALUnsupported
        out = []
        for path in sorted(glob.glob(os.path.join(self.root, self.SFP_EEPROMS)),
                           key=lambda p: int((p.split("/")[-2].split("-")[0]) or 0)
                           if p.split("/")[-2].split("-")[0].isdigit() else 0):
            try:
                with open(path, "rb") as f:
                    data = f.read(256)
            except OSError:
                continue                        # absent module -> i2c NAK -> skip
            opt = self._decode_optic(data)
            if opt:
                bus = path.split("/")[-2].split("-")[0]
                out.append({"bus": bus, "present": True, **opt})
        return out

    def thermals(self):
        """Generic: every hwmon temp*_input under /sys/class/hwmon (milli-C -> C)."""
        out = []
        for inp in sorted(glob.glob(os.path.join(self.root, "sys/class/hwmon/hwmon*/temp*_input"))):
            v = self._read_int("/" + os.path.relpath(inp, self.root))
            if v is None:
                continue
            label = self._read("/" + os.path.relpath(inp.replace("_input", "_label"), self.root))
            name = label or "%s/%s" % (os.path.basename(os.path.dirname(inp)),
                                       os.path.basename(inp).replace("_input", ""))
            out.append({"name": name, "celsius": round(v / 1000.0, 1)})
        return out

    def hal_report(self):
        """Collect whatever the board's HAL implements; mark the rest unsupported."""
        rep = {"platform": self.PLATFORM, "model": self.MODEL}
        for key, fn in (("thermals", self.thermals), ("fans", self.fans),
                        ("psus", self.psus), ("sfps", self.sfps), ("leds", self.leds)):
            try:
                rep[key] = fn()
            except HALUnsupported:
                rep[key] = "unsupported"
            except Exception as e:                       # noqa: BLE001 — never let one sensor break the report
                rep[key] = "error: %s" % e
        return rep
