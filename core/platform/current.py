"""Resolve the platform to its EdgeNOS platform class (ONL current.py analog).

Detection cascade (first hit wins):
  1. /etc/edgenos/version.json  -> onie_platform     (the EdgeNOS canonical identity)
  2. /etc/os-release            -> EDGENOS_PLATFORM
  3. onie-sysinfo -p            (under ONIE)

In a per-switch image there's exactly one platform/<board>/platform.py, so this
resolves it directly; the cascade still works for sanity and for the (optional)
case of a multi-board image.

CLI:
  current.py show [--root R]    print the resolved platform's info
  current.py init [--root R]    run the platform's baseconfig() (load drivers, init hw)
"""
import os
import sys
import glob
import json
import argparse
import subprocess
import importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))           # edgenos/


def platform_name(root="/"):
    vj = os.path.join(root, "etc/edgenos/version.json")
    if os.path.exists(vj):
        try:
            d = json.load(open(vj))
            if d.get("onie_platform"):
                return d["onie_platform"]
        except Exception:
            pass
    osr = os.path.join(root, "etc/os-release")
    if os.path.exists(osr):
        for line in open(osr, encoding="utf-8"):
            if line.startswith("EDGENOS_PLATFORM="):
                return line.split("=", 1)[1].strip().strip('"')
    try:
        return subprocess.check_output(["onie-sysinfo", "-p"]).decode().strip()
    except Exception:
        return None


def _board_classes():
    """Yield (class, source_path) for every platform/<board>/platform.py."""
    for pf in sorted(glob.glob(os.path.join(ROOT, "platform", "*", "platform.py"))):
        spec = importlib.util.spec_from_file_location("edgenos_board", pf)
        mod = importlib.util.module_from_spec(spec)
        try:
            spec.loader.exec_module(mod)
        except Exception as e:
            print(f"warning: failed to load {pf}: {e}", file=sys.stderr)
            continue
        for obj in vars(mod).values():
            if isinstance(obj, type) and getattr(obj, "PLATFORM", None):
                yield obj, pf


def load_platform(name=None, root="/"):
    name = name or platform_name(root)
    if not name:
        raise RuntimeError("could not determine platform (no version.json / os-release / onie-sysinfo)")
    for cls, _ in _board_classes():
        if cls.PLATFORM == name:
            return cls(root=root)
    raise RuntimeError(f"no EdgeNOS platform class found for '{name}'")


def main(argv):
    ap = argparse.ArgumentParser(prog="edgenos platform")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for c in ("show", "init", "name", "hal"):
        s = sub.add_parser(c)
        s.add_argument("--root", default="/")
        s.add_argument("--platform", default=None, help="override detected platform")
    args = ap.parse_args(argv[1:])

    if args.cmd == "name":
        print(platform_name(args.root) or "(unknown)")
        return 0
    plat = load_platform(args.platform, root=args.root)
    if args.cmd == "show":
        print(json.dumps(plat.info(), indent=2))
        return 0
    if args.cmd == "hal":
        print(json.dumps(plat.hal_report(), indent=2))
        return 0
    if args.cmd == "init":
        print(f"platform: baseconfig for {plat.PLATFORM} ({plat.MODEL})")
        return 0 if plat.baseconfig() else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
