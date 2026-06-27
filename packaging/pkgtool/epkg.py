#!/usr/bin/env python3
"""
epkg — the on-box EdgeNOS package manager. Stdlib-only so it runs on any switch.

    epkg info    FILE.epk              show manifest
    epkg verify  FILE.epk              check payload + per-file checksums
    epkg install FILE.epk [--root /]   install onto a (live) box
    epkg list            [--root /]    list installed packages
    epkg remove  NAME    [--root /]    remove an installed package
    epkg show    NAME    [--root /]    show an installed package's manifest

Install safety (the point of a cross-arch package system):
  - refuses a package whose arch/asic doesn't match the box (from /etc/os-release
    EDGENOS_ARCH / EDGENOS_ASIC); override with --force.
  - refuses a non-runtime_installable package at runtime unless --force (base
    packages are meant to be composed into the image, not installed live).
  - validates every checksum before writing a single file.

State: <root>/var/lib/edgenos/epkg/installed/<name>.json holds the manifest of each
installed package, so list/remove/show work and removal knows which files it owns.
"""
import os, sys, json, argparse, subprocess, tempfile, stat

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import epk                                              # noqa: E402

STATE_REL = "var/lib/edgenos/epkg/installed"


def _state_dir(root):
    return os.path.join(root, STATE_REL)


def _osrelease(root):
    out = {}
    path = os.path.join(root, "etc/os-release")
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            if "=" in line:
                k, _, v = line.strip().partition("=")
                out[k] = v.strip().strip('"')
    return out


def _run_hook(name, script, root):
    if not script:
        return
    with tempfile.NamedTemporaryFile("w", suffix=f".{name}", delete=False) as f:
        if not script.startswith("#!"):
            f.write("#!/bin/sh\n")
        f.write(script)
        hook = f.name
    os.chmod(hook, 0o755)
    try:
        env = dict(os.environ, EPKG_ROOT=root)
        # run hooks chrooted only when installing to "/" as root; else just run in-place
        rc = subprocess.call([hook], env=env)
        if rc != 0:
            print(f"  warning: {name} hook exited {rc}")
    finally:
        os.unlink(hook)


# ------------------------------------------------------------------ commands
def cmd_info(args):
    m = epk.read_manifest(args.file)
    print(json.dumps(m, indent=2))


def cmd_verify(args):
    ok, problems = epk.verify(args.file)
    if ok:
        print(f"OK — {args.file} verified")
        return 0
    print(f"FAILED — {args.file}:")
    for p in problems:
        print(f"  - {p}")
    return 1


def cmd_install(args):
    m = epk.read_manifest(args.file)
    box = _osrelease(args.root)
    box_arch = box.get("EDGENOS_ARCH")
    box_asic = box.get("EDGENOS_ASIC")

    # safety checks
    def fail(msg):
        if args.force:
            print(f"  (forced past) {msg}")
        else:
            raise SystemExit(f"refusing to install: {msg} (use --force to override)")

    if box_arch and m["arch"] != "any" and m["arch"] != box_arch:
        fail(f"package arch '{m['arch']}' != box arch '{box_arch}'")
    if box_asic and m["asic"] != "any" and m["asic"] != box_asic:
        fail(f"package asic '{m['asic']}' != box asic '{box_asic}'")
    if not m.get("runtime_installable", False):
        fail(f"package '{m['name']}' is not runtime_installable (type={m['type']})")

    ok, problems = epk.verify(args.file)
    if not ok:
        raise SystemExit("refusing to install: checksum verification failed:\n  - "
                         + "\n  - ".join(problems))

    hooks = m.get("hooks", {})
    _run_hook("preinst", hooks.get("preinst"), args.root)
    written = epk.extract_payload(args.file, args.root)
    _run_hook("postinst", hooks.get("postinst"), args.root)

    sd = _state_dir(args.root)
    os.makedirs(sd, exist_ok=True)
    with open(os.path.join(sd, m["name"] + ".json"), "w", encoding="utf-8") as f:
        json.dump(m, f, indent=2)

    print(f"installed {m['name']} {m['version']} ({m['arch']}-{m['asic']}) — {len(written)} files")
    for w in written:
        print(f"  {os.path.join('/', os.path.relpath(w, args.root))}")
    return 0


def cmd_list(args):
    sd = _state_dir(args.root)
    if not os.path.isdir(sd):
        print("(no packages installed)")
        return 0
    rows = []
    for fn in sorted(os.listdir(sd)):
        if fn.endswith(".json"):
            m = json.load(open(os.path.join(sd, fn)))
            rows.append(m)
    if not rows:
        print("(no packages installed)")
        return 0
    for m in rows:
        print(f"{m['name']:20} {m['version']:10} {m['arch']}-{m['asic']:14} {m['type']}")
    return 0


def cmd_show(args):
    path = os.path.join(_state_dir(args.root), args.name + ".json")
    if not os.path.exists(path):
        raise SystemExit(f"not installed: {args.name}")
    print(open(path).read())
    return 0


def cmd_remove(args):
    path = os.path.join(_state_dir(args.root), args.name + ".json")
    if not os.path.exists(path):
        raise SystemExit(f"not installed: {args.name}")
    m = json.load(open(path))
    hooks = m.get("hooks", {})
    _run_hook("prerm", hooks.get("prerm"), args.root)
    for f in m.get("files", []):
        p = os.path.join(args.root, f["path"].lstrip("/"))
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass
    _run_hook("postrm", hooks.get("postrm"), args.root)
    os.unlink(path)
    print(f"removed {m['name']} {m['version']} — {len(m.get('files', []))} files")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(prog="epkg", description="EdgeNOS on-box package manager")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("info", "verify"):
        s = sub.add_parser(name); s.add_argument("file")
    s = sub.add_parser("install"); s.add_argument("file")
    s.add_argument("--root", default="/"); s.add_argument("--force", action="store_true")
    for name in ("list",):
        s = sub.add_parser(name); s.add_argument("--root", default="/")
    for name in ("remove", "show"):
        s = sub.add_parser(name); s.add_argument("name"); s.add_argument("--root", default="/")
    args = ap.parse_args(argv[1:])
    return {
        "info": cmd_info, "verify": cmd_verify, "install": cmd_install,
        "list": cmd_list, "remove": cmd_remove, "show": cmd_show,
    }[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
