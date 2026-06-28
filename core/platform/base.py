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
import subprocess


class HALUnsupported(NotImplementedError):
    """Raised by HAL methods a board hasn't implemented yet."""


class PlatformHAL:
    """ONLP-analog hardware abstraction. Boards override what they support."""
    def sfp_count(self):            raise HALUnsupported
    def sfp_present(self, port):    raise HALUnsupported
    def sfp_eeprom(self, port):     raise HALUnsupported
    def fan_count(self):            raise HALUnsupported
    def fan_rpm(self, idx):         raise HALUnsupported
    def fan_set(self, idx, pct):    raise HALUnsupported
    def psu_count(self):            raise HALUnsupported
    def psu_present(self, idx):     raise HALUnsupported
    def thermal_count(self):        raise HALUnsupported
    def thermal_celsius(self, idx): raise HALUnsupported
    def led_set(self, name, state): raise HALUnsupported


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
