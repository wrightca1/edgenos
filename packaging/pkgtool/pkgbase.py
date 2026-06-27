#!/usr/bin/env python3
"""
pkgbase — capture a proven rootfs into a first-class base .epk.

The "base system for the asic and cpu" is a package like any other: a base.epk
carries the platform's base root filesystem as a single squashfs blob, tagged with
arch+asic, type=base, not runtime_installable (it's composed into the image, never
live-installed). The image recipe (imgbuild.py) extracts it and appends the version
stamps + component metadata on top.

Capturing the squashfs as an opaque blob (rather than unpacking + repacking the tree)
keeps the base byte-for-byte faithful to the proven artifact — no fidelity loss on
symlinks, device nodes, perms, or ownership.

Usage:
    pkgbase.py --from ROOTFS.sqsh --platform <onie_platform> [--version V] [-o OUTDIR]
"""
import os, sys, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))          # edgenos/
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import epk                                              # noqa: E402
from switchdb import SwitchDB, DBError                  # noqa: E402

BASE_BLOB_PATH = "/.edgenos/base/rootfs.sqsh"           # where the blob lives in the payload


def base_version():
    with open(os.path.join(ROOT, "VERSION"), encoding="utf-8") as f:
        return f.read().strip()


def main(argv):
    ap = argparse.ArgumentParser(description="Capture a rootfs into a base .epk")
    ap.add_argument("--from", dest="src", required=True, help="proven rootfs squashfs")
    ap.add_argument("--platform", required=True, help="ONIE platform string (switch DB key)")
    ap.add_argument("--version", help="override base version (default edgenos/VERSION)")
    ap.add_argument("--epoch", type=int, default=int(os.environ.get("SOURCE_DATE_EPOCH", "0")))
    ap.add_argument("-o", "--outdir", default=os.path.join(ROOT, "output", "packages"))
    args = ap.parse_args(argv[1:])

    if not os.path.isfile(args.src):
        raise SystemExit(f"error: not found: {args.src}")
    try:
        view = SwitchDB().resolve(args.platform)
    except DBError as e:
        raise SystemExit(f"error: {e}")
    arch, asic = view["arch"]["id"], view["asic"]["id"]
    version = args.version or base_version()

    payload, files_meta = epk.build_payload([(BASE_BLOB_PATH, args.src, 0o644)], args.epoch)
    manifest = {
        "format_version": epk.FORMAT_VERSION,
        "name": "base",
        "version": version,
        "arch": arch,
        "asic": asic,
        "type": "base",
        "runtime_installable": False,
        "summary": f"Base root filesystem for {view['platform']['vendor']} "
                   f"{view['platform']['model']} ({arch}/{asic})",
        "depends": [],
        "hooks": {h: None for h in epk.HOOK_NAMES},
        "files": files_meta,
        "base_blob": BASE_BLOB_PATH,                    # imgbuild reads this
        "source_onie_platform": args.platform,
        "build": {"epoch": args.epoch},
    }
    os.makedirs(args.outdir, exist_ok=True)
    out_path = os.path.join(args.outdir, epk.pkg_filename(manifest))
    final = epk.write_epk(out_path, manifest, payload, args.epoch)
    ok, problems = epk.verify(out_path)
    print(f"built {os.path.relpath(out_path, ROOT)}")
    print(f"  base {final['version']}  arch={arch} asic={asic}  "
          f"rootfs {files_meta[0]['size']//(1<<20)} MiB")
    print(f"  self-verify: {'OK' if ok else 'FAILED: ' + '; '.join(problems)}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
