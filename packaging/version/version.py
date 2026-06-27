#!/usr/bin/env python3
"""
EdgeNOS version stamper.

One release version (semver in edgenos/VERSION) resolves, per platform, to a
build identity carrying arch/asic/kernel + git SHA + build timestamp. Generalizes
ONL's make-versions.py model, but platform-aware and driven by the switch DB.

Emits:
  - version.json    machine-readable, shipped to /etc/edgenos/version.json
  - os-release      the subset that belongs in /etc/os-release

The build date is NOT taken from the wall clock by default (reproducible builds):
pass --epoch <unix> or set SOURCE_DATE_EPOCH. Falls back to 0 (1970) if neither
is given, so an unstamped build is obvious rather than silently nondeterministic.
"""
import os, sys, json, argparse, subprocess, datetime

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))           # edgenos/
sys.path.insert(0, os.path.join(ROOT, "tools"))
from switchdb import SwitchDB, DBError                   # noqa: E402

NAME = "EdgeNOS"


def read_base_version():
    with open(os.path.join(ROOT, "VERSION"), encoding="utf-8") as f:
        return f.read().strip()


def git_sha(short=False):
    try:
        args = ["git", "-C", ROOT, "rev-parse"] + (["--short"] if short else []) + ["HEAD"]
        return subprocess.check_output(args, stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return "nogit"


def git_dirty():
    try:
        out = subprocess.check_output(["git", "-C", ROOT, "status", "--porcelain"],
                                      stderr=subprocess.DEVNULL).decode().strip()
        return bool(out)
    except Exception:
        return False


def build_identity(onie_platform, epoch=None):
    db = SwitchDB()
    view = db.resolve(onie_platform)
    p, arch, asic = view["platform"], view["arch"], view["asic"]

    if epoch is None:
        epoch = os.environ.get("SOURCE_DATE_EPOCH")
    epoch = int(epoch) if epoch is not None else 0
    ts = datetime.datetime.utcfromtimestamp(epoch)

    base = read_base_version()
    short = git_sha(short=True)
    dirty = git_dirty()
    build_id = f"{ts:%Y-%m-%d.%H:%M}-{short}" + ("-dirty" if dirty else "")
    version_string = f"{NAME} {base}, {build_id}"

    return {
        "name": NAME,
        "version": base,                              # semver release
        "version_id": base,
        "build_id": build_id,                         # date+sha, ONL-style
        "build_sha1": git_sha(),
        "build_short_sha1": short,
        "build_timestamp": f"{ts:%Y-%m-%dT%H:%M:%SZ}",
        "build_epoch": epoch,
        "dirty": dirty,
        "version_string": version_string,
        # platform identity pulled straight from the switch DB
        "onie_platform": p["onie_platform"],
        "platform": f"{p['vendor']}-{p['model']}",
        "arch": arch["id"],
        "asic": asic["id"],
        "asic_family": asic.get("family"),
        "kernel": p["kernel"],
        "datapath": view["datapath"],
        "pretty_name": f"{NAME} {base} ({p['vendor']} {p['model']}, {asic['id']})",
    }


def os_release(ident):
    """The subset of fields that belong in /etc/os-release."""
    return "\n".join([
        f'NAME="{ident["name"]}"',
        f'VERSION="{ident["version"]} ({ident["build_id"]})"',
        f'VERSION_ID="{ident["version_id"]}"',
        f'BUILD_ID="{ident["build_id"]}"',
        f'PRETTY_NAME="{ident["pretty_name"]}"',
        f'ID=edgenos',
        f'EDGENOS_PLATFORM="{ident["onie_platform"]}"',
        f'EDGENOS_ARCH="{ident["arch"]}"',
        f'EDGENOS_ASIC="{ident["asic"]}"',
        f'EDGENOS_KERNEL="{ident["kernel"]}"',
        f'HOME_URL="https://github.com/wrightca1"',
    ]) + "\n"


def main(argv):
    ap = argparse.ArgumentParser(description="EdgeNOS version stamper")
    ap.add_argument("platform", help="ONIE platform string (switch DB key)")
    ap.add_argument("--epoch", type=int, default=None,
                    help="build epoch (unix seconds); else SOURCE_DATE_EPOCH; else 0")
    ap.add_argument("--json", metavar="PATH", help="write version.json here")
    ap.add_argument("--os-release", metavar="PATH", dest="osr", help="write os-release here")
    ap.add_argument("--print", action="store_true", help="print the version string")
    args = ap.parse_args(argv[1:])

    try:
        ident = build_identity(args.platform, epoch=args.epoch)
    except DBError as e:
        raise SystemExit(f"error: {e}")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(ident, f, indent=2)
            f.write("\n")
    if args.osr:
        with open(args.osr, "w", encoding="utf-8") as f:
            f.write(os_release(ident))
    if args.print or not (args.json or args.osr):
        print(ident["version_string"])
        print(f"  platform : {ident['onie_platform']}")
        print(f"  arch/asic: {ident['arch']} / {ident['asic']} ({ident['asic_family']})")
        print(f"  kernel   : {ident['kernel']}   datapath: {ident['datapath']}")


if __name__ == "__main__":
    main(sys.argv)
