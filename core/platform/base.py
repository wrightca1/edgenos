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
import re
import glob
import math
import subprocess


def _optic_sortkey(e):
    """Order optics by front-panel name (swp1<swp2<…, xe0<xe1), else by bus."""
    name = e.get("name") or ""
    m = re.search(r"(\d+)$", name)
    return (re.sub(r"\d+$", "", name) or "~", int(m.group(1)) if m else 0, int(e.get("bus") or 0))


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

    def _read_bytes(self, relpath, nbytes, timeout=0.6):
        """Read up to nbytes from a file with a hard timeout (via dd subprocess) —
        so a hung i2c eeprom on a broken mux bus can't block the caller. Threads
        can't use SIGALRM, hence subprocess. Returns bytes or None."""
        path = os.path.join(self.root, relpath.lstrip("/"))
        try:
            p = subprocess.run(["dd", "if=" + path, "bs=%d" % nbytes, "count=1", "status=none"],
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=timeout)
            return p.stdout or None
        except Exception:
            return None

    def _i2c_read(self, bus, addr, reg, force=False):
        """Read one byte via the `i2cget` CLI (for CPLDs / optic DDM with no eeprom
        sysfs). force=True adds -f to read past a no-op `dummy` driver that merely
        reserves the address (e.g. the SFP A2 diagnostics page @0x51). Returns int or
        None. Only meaningful on the box; None off-hardware."""
        cmd = ["i2cget"] + (["-f"] if force else []) + ["-y", str(bus), hex(addr), hex(reg)]
        try:
            out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 timeout=5).stdout.decode().strip()
            return int(out, 16) if out else None
        except Exception:
            return None

    @staticmethod
    def _dbm(u):
        """SFF power word (unit 0.1 uW) -> dBm, or None for no light (<=0)."""
        return round(10 * math.log10(u * 0.1 / 1000.0), 2) if u and u > 0 else None

    def sfp_diagnostics(self, bus, kind="SFP"):
        """Live optic DDM/DOM (light levels + temp/vcc/bias).
        SFP  : SFF-8472 A2 real-time @0x51 bytes 96..105 — forced past the `dummy`
               driver that reserves 0x51 (no eeprom sysfs there).
        QSFP : SFF-8636 monitor fields live in the 0x50 image (lower page 22..57),
               reported per-lane (x4).
        Returns {temp_c, vcc_v, tx_bias_ma, tx_power_dbm, rx_power_dbm} for SFP,
        {temp_c, vcc_v, lanes:[{tx_power_dbm, rx_power_dbm, tx_bias_ma}]} for QSFP,
        or None if unreadable / DDM not populated."""
        if kind and kind.startswith("QSFP"):
            data = self._optic_a0_image(bus, 128) or b""
            if len(data) < 58:
                return None
            w = lambda i: (data[i] << 8) | data[i + 1]
            t = w(22); t = t - 65536 if t > 32767 else t
            lanes = [{"rx_power_dbm": self._dbm(w(34 + 2 * n)),
                      "tx_bias_ma": round(w(42 + 2 * n) * 0.002, 2),
                      "tx_power_dbm": self._dbm(w(50 + 2 * n))} for n in range(4)]
            if t == 0 and w(26) == 0 and all(l["rx_power_dbm"] is None for l in lanes):
                return None                                   # module doesn't populate DDM
            return {"temp_c": round(t / 256.0, 1), "vcc_v": round(w(26) * 0.0001, 2), "lanes": lanes}
        raw = [self._i2c_read(bus, 0x51, 96 + i, force=True) for i in range(10)]
        if any(v is None for v in raw):
            return None
        w = lambda i: (raw[i] << 8) | raw[i + 1]
        t = w(0); t = t - 65536 if t > 32767 else t
        return {"temp_c": round(t / 256.0, 1), "vcc_v": round(w(2) * 0.0001, 2),
                "tx_bias_ma": round(w(4) * 0.002, 2),
                "tx_power_dbm": self._dbm(w(6)), "rx_power_dbm": self._dbm(w(8))}

    # SFP/QSFP optic decode (SFF-8472 / SFF-8636). Two board-provided sources:
    #  - SFP_EEPROMS: glob of bound `eeprom` sysfs nodes (at24/optoe drivers). Present-only.
    #  - SFP_I2C_PORTS: {bus:int -> name:str} read via raw i2c, for boards whose optic
    #    driver isn't loaded (e.g. 4610 optoe absent) so no eeprom sysfs exists.
    SFP_EEPROMS = None      # e.g. "sys/bus/i2c/devices/*-0050/eeprom"
    SFP_I2C_PORTS = None    # e.g. {2: "xe0", 3: "xe1", ...}
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

    def _sfp_port_for_bus(self, bus):
        """Board hook: map an i2c bus -> front-panel interface name (e.g. 'swp6' on the
        5610, 'xe0' on the 4610), or None if the board doesn't know its mux topology.
        Used for the SFP_EEPROMS sysfs path; the SFP_I2C_PORTS path carries names
        directly. Default: unlabeled (the i2c bus is still a stable per-port handle)."""
        return None

    def _i2c_dump(self, bus, addr, first, last, force=False):
        """Block-read absolute bytes [first..last] from an i2c/SMBus device via the
        `i2cdump` CLI (ONE process — works on SMBus adapters where i2ctransfer's raw
        mode is unsupported, and ~4x faster than per-byte i2cget which re-spawns and
        re-selects the mux each byte). For optic EEPROMs with no bound eeprom sysfs.
        Returns a bytes buffer indexed 0..last (bytes below `first` are 0), or None."""
        cmd = ["i2cdump"] + (["-f"] if force else []) + \
            ["-y", "-r", "%d-%d" % (first, last), str(bus), hex(addr), "b"]
        try:
            out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 timeout=30).stdout.decode()
        except Exception:
            return None
        vals = {}
        for line in out.splitlines():
            m = re.match(r"^([0-9a-f]+):\s+(.*)", line)
            if not m:
                continue
            base = int(m.group(1), 16)
            for i, tok in enumerate(m.group(2).split()[:16]):
                if re.fullmatch(r"[0-9a-fA-F]{2}", tok):
                    vals[base + i] = int(tok, 16)
        return bytes(vals.get(i, 0) for i in range(last + 1)) if vals else None

    def _optic_a0_image(self, bus, n=128):
        """The optic A0 (0x50) image — from the bound eeprom sysfs node if present,
        else raw i2c (i2cdump). Returns bytes (indexed from 0) or None."""
        rel = "sys/bus/i2c/devices/%d-0050/eeprom" % int(bus)
        if os.path.exists(os.path.join(self.root, rel)):
            return self._read_bytes(rel, n)
        return self._i2c_dump(bus, 0x50, 0, n - 1)

    def _i2c_optic_read(self, bus):
        """Read an optic's identity over raw i2c. A single-byte i2cget of byte 0 is a
        fast presence probe (empty bus NAKs in ~50ms) that also tells SFP from QSFP;
        only then do we pay for the (slow, per-byte) block read — and only as far as the
        identity fields reach (SFP serial ends @84, QSFP @212). Returns bytes or None."""
        id0 = self._i2c_read(bus, 0x50, 0)
        if id0 is None:
            return None                                  # no module / NAK
        last = 211 if id0 in (0x0c, 0x0d, 0x11, 0x18) else 83
        return self._i2c_dump(bus, 0x50, 0, last)

    def sfps(self):
        """Inventory present optics -> [{bus, name, type, vendor, part, serial}].
        Sources: bound eeprom sysfs nodes (SFP_EEPROMS, at24/optoe) and/or raw-i2c
        buses (SFP_I2C_PORTS, driver-absent boards). A module is reported only if its
        EEPROM decodes as an SFF optic; `name` is the front-panel port when the board
        maps it. Same output shape regardless of how the bytes were obtained."""
        if not (self.SFP_EEPROMS or self.SFP_I2C_PORTS):
            raise HALUnsupported
        out, seen = [], set()
        for path in sorted(glob.glob(os.path.join(self.root, self.SFP_EEPROMS or "\0")),
                           key=lambda p: int((p.split("/")[-2].split("-")[0]) or 0)
                           if p.split("/")[-2].split("-")[0].isdigit() else 0):
            bus = path.split("/")[-2].split("-")[0]
            data = self._read_bytes("/" + os.path.relpath(path, self.root), 256)
            opt = self._decode_optic(data) if data else None
            if not opt:
                continue
            seen.add(bus)
            entry = {"bus": bus, "present": True, **opt}
            name = self._sfp_port_for_bus(int(bus)) if bus.isdigit() else None
            if name:
                entry["name"] = name
            out.append(entry)
        for bus, name in sorted((self.SFP_I2C_PORTS or {}).items()):
            if str(bus) in seen:
                continue
            opt = self._decode_optic(self._i2c_optic_read(bus))
            if opt:
                out.append({"bus": str(bus), "name": name, "present": True, **opt})
        return sorted(out, key=_optic_sortkey)

    def _bus_for_name(self, name):
        """Front-panel port name -> i2c bus, or None. Inverse of the board's map."""
        for b, n in (self.SFP_I2C_PORTS or {}).items():
            if n == name:
                return b
        for path in glob.glob(os.path.join(self.root, self.SFP_EEPROMS or "\0")):
            b = path.split("/")[-2].split("-")[0]
            if b.isdigit() and self._sfp_port_for_bus(int(b)) == name:
                return int(b)
        return None

    def optic_for_port(self, name):
        """One optic by front-panel port name -> dict (bus/name/type/vendor/…) or None.
        The cheap path for a single-port query: reads at most one module (vs sfps()
        which sweeps every bus). Returns None for a copper/empty/unmapped port."""
        bus = self._bus_for_name(name)
        if bus is None:
            return None
        rel = "sys/bus/i2c/devices/%d-0050/eeprom" % int(bus)
        if os.path.exists(os.path.join(self.root, rel)):
            opt = self._decode_optic(self._read_bytes(rel, 256))
        else:
            opt = self._decode_optic(self._i2c_optic_read(bus))
        if not opt:
            return None
        return {"bus": str(bus), "name": name, "present": True, **opt}

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
