#!/usr/bin/env python3
"""
catalog — the "pick your switch" index, generated from the switch DB.

Lists every supported switch with the facts a downloader needs: model, CPU arch,
switch ASIC, kernel, installer type, and the installer artifact name they'd download.

    catalog            human-readable table
    catalog --json     machine-readable (for a download page / CI matrix)
"""
import os, sys, json, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from switchdb import SwitchDB                            # noqa: E402


def version():
    return open(os.path.join(ROOT, "VERSION"), encoding="utf-8").read().strip()


def artifact_name(p, ver):
    ext = "bin" if p.get("installer") == "onie-sfx" else "swi"
    return f"EdgeNOS-{ver}-{p['onie_platform']}.{ext}"


def entries():
    db = SwitchDB()
    ver = version()
    out = []
    for key in db.list_platforms():
        p = db.platforms[key]
        a, s = db.arch[p["arch"]], db.asic[p["asic"]]
        out.append({
            "onie_platform": key,
            "model": f"{p['vendor']} {p['model']}",
            "arch": p["arch"], "cpu": a.get("cpu"),
            "asic": p["asic"], "asic_name": s.get("name"),
            "kernel": p["kernel"], "installer": p.get("installer"),
            "status": p.get("status", "?"),
            "download": artifact_name(p, ver),
        })
    return out


def main(argv):
    ap = argparse.ArgumentParser(description="EdgeNOS switch catalog")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv[1:])
    rows = entries()
    if args.json:
        print(json.dumps({"version": version(), "switches": rows}, indent=2))
        return 0
    print(f"EdgeNOS {version()} — supported switches\n")
    print(f"{'MODEL':22} {'ARCH':8} {'ASIC':10} {'KERNEL':7} {'STATUS':11} DOWNLOAD")
    print("-" * 96)
    for r in rows:
        print(f"{r['model']:22} {r['arch']:8} {r['asic']:10} {r['kernel']:7} "
              f"{r['status']:11} {r['download']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
