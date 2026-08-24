"""EdgeNOS platform class for the QEMU/KVM x86_64 virtual switch (ONL OnlPlatform analog).

No switch ASIC: every NIC after the first is a front-panel port (ge0..geN-1, named by
PCI order by services/edgenos-ports.sh), the first NIC is the management port ma1, and
the Linux kernel forwards. Nothing to insmod (the x86_64 base kernel has virtio/e1000/
vmxnet3 + the networking stack built in), so baseconfig() just (re)applies the port
naming. The HAL degrades gracefully: thermals() is the generic hwmon reader (empty on a
VM), fans/PSUs/LEDs/SFPs are HALUnsupported.
"""
import os
import sys
import glob
import subprocess

_HERE = os.path.dirname(os.path.abspath(__file__))
for _p in (_HERE, os.path.abspath(os.path.join(_HERE, "..", "..", "core", "platform"))):
    if _p not in sys.path:
        sys.path.insert(0, _p)
from base import EdgeNOSPlatformBase, PortConfig, HALUnsupported   # noqa: E402


class PortConfig_vm(PortConfig):
    """Ports are whatever the hypervisor attached: enumerate ge* at query time."""
    PREFIX = "ge"
    SPEED_GBPS = 1

    @classmethod
    def discover(cls, root="/"):
        names = [os.path.basename(p) for p in glob.glob(os.path.join(root, "sys/class/net", cls.PREFIX + "*"))]
        names.sort(key=lambda n: int(n[len(cls.PREFIX):]) if n[len(cls.PREFIX):].isdigit() else 0)
        return [(n, cls.SPEED_GBPS) for n in names]


class EdgeNOSPlatform_x86_64_kvm_x86_64_r0(EdgeNOSPlatformBase, PortConfig_vm):
    PLATFORM = "x86_64-kvm_x86_64-r0"            # == switch DB key (ONIE's kvm_x86_64 machine)
    MODEL = "QEMU/KVM x86_64 virtual switch"
    SYS_OBJECT_ID = ".86.64"

    DRIVERS = []                                 # everything needed is built into the kernel
    INIT_SCRIPTS = ["/opt/edgenos/edgenos-ports.sh"]   # idempotent port naming (ma1 + ge*)
    PORTS_SCRIPT = "/opt/edgenos/edgenos-ports.sh"

    # --- ports (dynamic) ---
    @property
    def PORTS(self):                             # noqa: N802 — PortConfig contract
        return PortConfig_vm.discover(self.root)

    def port_count(self):
        return len(self.PORTS)

    # --- identity helpers ---
    def hypervisor(self):
        """Best-effort: DMI sys_vendor/product_name + /sys/hypervisor."""
        vendor = self._read("/sys/class/dmi/id/sys_vendor") or ""
        product = self._read("/sys/class/dmi/id/product_name") or ""
        hv = self._read("/sys/hypervisor/type")
        return {"sys_vendor": vendor, "product_name": product, "hypervisor": hv,
                "virtual": bool(hv) or any(k in (vendor + product).lower()
                                           for k in ("qemu", "kvm", "vmware", "bochs", "virtualbox", "xen"))}

    def nics(self):
        """All PCI NICs in the VM with their PCI address and current name (PCI order)."""
        out = []
        for d in glob.glob(os.path.join(self.root, "sys/class/net/*")):
            dev = os.path.join(d, "device")
            if not os.path.exists(dev):
                continue                          # lo, bridges, tunnels, veth…
            real = os.path.realpath(dev)
            pci = None
            for comp in reversed(real.split("/")):
                if len(comp) == 12 and comp[4] == ":" and comp[7] == ":" and comp[10] == ".":
                    pci = comp
                    break
            out.append({"pci": pci or "?", "name": os.path.basename(d),
                        "driver": os.path.basename(os.path.realpath(os.path.join(dev, "driver")))
                        if os.path.exists(os.path.join(dev, "driver")) else None})
        return sorted(out, key=lambda n: n["pci"])

    def info(self):
        d = super().info()
        d["ports"] = self.port_count()
        d["port_names"] = [n for n, _ in self.PORTS]
        d["mgmt"] = "ma1" if os.path.exists(os.path.join(self.root, "sys/class/net/ma1")) else None
        d["nics"] = self.nics()
        d["hypervisor"] = self.hypervisor()
        return d

    # --- HAL: what a VM can answer ---
    def fan_count(self):
        return 0

    def psu_count(self):
        return 0

    def fans(self):      raise HALUnsupported
    def psus(self):      raise HALUnsupported
    def leds(self):      raise HALUnsupported
    def sfps(self):      raise HALUnsupported
    # thermals(): generic hwmon reader from the base (usually empty under QEMU)

    # --- bring-up ---
    def baseconfig(self):
        """No drivers to load; (re)apply the port naming so ma1/ge* exist. Idempotent."""
        script = os.path.join(self.root, self.PORTS_SCRIPT.lstrip("/"))
        if self.root == "/" and os.path.exists(script):
            rc = subprocess.call(["sh", script])
            print(f"platform: port naming {'ok' if rc == 0 else 'FAILED'} "
                  f"({self.port_count()} front-panel ports)")
            return rc == 0
        print("platform: staging root — skip port naming")
        return True
