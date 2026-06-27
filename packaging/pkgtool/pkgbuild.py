#!/usr/bin/env python3
"""
EdgeNOS package builder — turn a package spec (YAML) into an arch/ASIC-tagged .epk.

A spec declares the *logical* package (name, type, files, hooks). arch/asic are
either pinned in the spec or supplied at build time; --platform resolves them from
the switch DB and cross-checks the spec's pins, so a package can never be tagged for
an ASIC the platform doesn't have.

Usage:
    pkgbuild.py SPEC.yml --source-root DIR --platform <onie_platform> [-o OUTDIR]
    pkgbuild.py SPEC.yml --source-root DIR --arch powerpc --asic bcm56846

Spec format (YAML):
    name: edged
    summary: ...
    type: overlay            # base | overlay
    runtime_installable: true
    arch: powerpc            # optional pin; "any" or omit to take from build args
    asic: bcm56846           # optional pin
    version: 0.1.0           # optional; defaults to edgenos/VERSION
    depends: [linux-user-bde]
    files:
      - {src: output/edged-rebuilt, dst: /usr/sbin/edged, mode: "0755"}
    hooks:
      postinst: |
        #!/bin/sh
        systemctl daemon-reload || true
"""
import os, sys, argparse
import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))          # edgenos/
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import epk                                              # noqa: E402
from switchdb import SwitchDB, DBError                  # noqa: E402

HOOK_NAMES = epk.HOOK_NAMES


def base_version():
    with open(os.path.join(ROOT, "VERSION"), encoding="utf-8") as f:
        return f.read().strip()


def resolve_tags(spec, args):
    """Determine (arch, asic), honoring spec pins + build args + switch DB."""
    arch, asic = spec.get("arch"), spec.get("asic")
    if args.platform:
        try:
            view = SwitchDB().resolve(args.platform)
        except DBError as e:
            raise SystemExit(f"error: {e}")
        p_arch, p_asic = view["arch"]["id"], view["asic"]["id"]
        if arch and arch not in ("any", p_arch):
            raise SystemExit(f"error: spec pins arch={arch} but platform {args.platform} is {p_arch}")
        if asic and asic not in ("any", p_asic):
            raise SystemExit(f"error: spec pins asic={asic} but platform {args.platform} is {p_asic}")
        arch = p_arch if (not arch or arch == "any") else arch
        asic = p_asic if (not asic or asic == "any") else asic
    if args.arch:
        arch = args.arch
    if args.asic:
        asic = args.asic
    arch = arch or "any"
    asic = asic or "any"
    return arch, asic


def main(argv):
    ap = argparse.ArgumentParser(description="Build an EdgeNOS .epk package")
    ap.add_argument("spec", help="package spec YAML")
    ap.add_argument("--source-root", default=".", help="root that spec 'src' paths are relative to")
    ap.add_argument("--platform", help="ONIE platform string (resolves arch/asic via switch DB)")
    ap.add_argument("--arch", help="override arch tag")
    ap.add_argument("--asic", help="override asic tag")
    ap.add_argument("--version", help="override package version (default: edgenos/VERSION)")
    ap.add_argument("--epoch", type=int, default=int(os.environ.get("SOURCE_DATE_EPOCH", "0")),
                    help="build epoch for reproducible output (default SOURCE_DATE_EPOCH or 0)")
    ap.add_argument("-o", "--outdir", default=os.path.join(ROOT, "output", "packages"))
    args = ap.parse_args(argv[1:])

    with open(args.spec, encoding="utf-8") as f:
        spec = yaml.safe_load(f)
    for req in ("name", "type", "files"):
        if req not in spec:
            raise SystemExit(f"error: spec missing required key '{req}'")
    if spec["type"] not in ("base", "overlay"):
        raise SystemExit("error: type must be 'base' or 'overlay'")

    arch, asic = resolve_tags(spec, args)
    version = args.version or spec.get("version") or base_version()

    # stage files: (dst, abs_src, mode)
    staged = []
    for entry in spec["files"]:
        src = os.path.join(args.source_root, entry["src"])
        if not os.path.isfile(src):
            raise SystemExit(f"error: source file not found: {src}")
        mode = int(str(entry.get("mode", "0644")), 8)
        staged.append((entry["dst"], src, mode))

    payload, files_meta = epk.build_payload(staged, args.epoch)

    hooks = {h: (spec.get("hooks", {}) or {}).get(h) for h in HOOK_NAMES}
    manifest = {
        "format_version": epk.FORMAT_VERSION,
        "name": spec["name"],
        "version": version,
        "arch": arch,
        "asic": asic,
        "type": spec["type"],
        "runtime_installable": bool(spec.get("runtime_installable", spec["type"] == "overlay")),
        "summary": spec.get("summary", ""),
        "depends": spec.get("depends", []),
        "hooks": hooks,
        "files": files_meta,
        "build": {"epoch": args.epoch},
    }

    os.makedirs(args.outdir, exist_ok=True)
    out_path = os.path.join(args.outdir, epk.pkg_filename(manifest))
    final = epk.write_epk(out_path, manifest, payload, args.epoch)

    ok, problems = epk.verify(out_path)
    status = "OK" if ok else "FAILED: " + "; ".join(problems)
    print(f"built {os.path.relpath(out_path, ROOT)}")
    print(f"  {final['name']} {final['version']}  arch={final['arch']} asic={final['asic']} "
          f"type={final['type']} runtime_installable={final['runtime_installable']}")
    print(f"  {len(files_meta)} files, payload sha256 {final['payload_sha256'][:16]}…")
    print(f"  self-verify: {status}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
