#!/usr/bin/env python3
"""
imgbuild — the EdgeNOS image recipe. Turns a switch-DB platform into a downloadable
ONIE installer carrying a base system for that CPU arch + ASIC plus its component
packages.

Flow (per platform):
  1. resolve platform -> arch, asic, components, installer (switch DB)
  2. extract the base .epk -> its rootfs squashfs blob -> unpack to a work tree
  3. overlay each component .epk found for this arch/asic onto the tree
  4. stamp identity: /etc/os-release, /etc/edgenos/version.json, and an installed-
     package DB under /var/lib/edgenos/epkg/installed (image is self-describing)
  5. mksquashfs -all-root -> composed rootfs.sqsh
  6. wrap in the platform's installer envelope:
        onie-sfx  -> version-templated install.sh + tar(FIT, rootfs.sqsh)  (.bin)
        onl-swi   -> zip(rootfs.sqsh, manifest.json) SWI [+ mkshar ONIE installer]

The proven base rootfs has no device nodes, so the unpack/repack round-trip needs no
root/fakeroot; -all-root forces uid/gid 0 in the result.

Usage:
    imgbuild.py <onie_platform> --source-root DIR [-o OUTDIR] [--epoch N]
"""
import os, sys, json, argparse, shutil, subprocess, tempfile, tarfile, re, glob

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                            # edgenos/
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "packaging", "pkgtool"))
sys.path.insert(0, os.path.join(ROOT, "packaging", "version"))
import yaml                                             # noqa: E402
import epk                                              # noqa: E402
import version as versionmod                            # noqa: E402
from switchdb import SwitchDB, DBError                  # noqa: E402


def log(msg):
    print(f"imgbuild: {msg}")


def base_version():
    return open(os.path.join(ROOT, "VERSION"), encoding="utf-8").read().strip()


def load_recipe(platform):
    path = os.path.join(ROOT, "images", platform + ".yml")
    if not os.path.exists(path):
        raise SystemExit(f"error: no image recipe at images/{platform}.yml")
    with open(path, encoding="utf-8") as f:
        return yaml.safe_load(f)


def run(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def extract_base_rootfs(base_epk, dest_sqsh):
    """Pull the base squashfs blob out of a base .epk."""
    m = epk.read_manifest(base_epk)
    if m.get("type") != "base":
        raise SystemExit(f"error: {base_epk} is not a base package")
    with tempfile.TemporaryDirectory() as td:
        epk.extract_payload(base_epk, td)
        blob = os.path.join(td, m["base_blob"].lstrip("/"))
        if not os.path.exists(blob):
            raise SystemExit(f"error: base blob {m['base_blob']} missing from {base_epk}")
        shutil.move(blob, dest_sqsh)
    return m


def stamp_identity(tree, platform, components_meta, epoch):
    ident = versionmod.build_identity(platform, epoch=epoch)
    # /etc/os-release (overwrite whatever the base shipped). Debian symlinks this to
    # ../usr/lib/os-release; unlink first so we always leave a real, predictable file.
    osr = os.path.join(tree, "etc/os-release")
    if os.path.islink(osr) or os.path.exists(osr):
        os.unlink(osr)
    with open(osr, "w", encoding="utf-8") as f:
        f.write(versionmod.os_release(ident))
    # /etc/edgenos/version.json — canonical EdgeNOS identity
    ed = os.path.join(tree, "etc/edgenos")
    os.makedirs(ed, exist_ok=True)
    with open(os.path.join(ed, "version.json"), "w", encoding="utf-8") as f:
        json.dump(ident, f, indent=2)
        f.write("\n")
    # installed-package DB — makes the image self-describing
    db = os.path.join(tree, "var/lib/edgenos/epkg/installed")
    os.makedirs(db, exist_ok=True)
    for m in components_meta:
        with open(os.path.join(db, m["name"] + ".json"), "w", encoding="utf-8") as f:
            json.dump(m, f, indent=2)
    return ident


def compose_rootfs(view, recipe, args, workdir):
    """Build the composed rootfs.sqsh; return (path, ident, included, missing)."""
    arch, asic = view["arch"]["id"], view["asic"]["id"]
    version = base_version()
    pkgs_dir = os.path.join(ROOT, recipe.get("packages_dir", "output/packages"))

    # --- base ---
    base_name = f"base_{version}_{arch}-{asic}.epk"
    base_epk = os.path.join(pkgs_dir, base_name)
    if not os.path.exists(base_epk):
        raise SystemExit(f"error: base package not found: {base_epk}\n"
                         f"  build it: bin/edgenos pkg base --from <rootfs.sqsh> --platform {args.platform}")
    base_sqsh = os.path.join(workdir, "base.sqsh")
    base_meta = extract_base_rootfs(base_epk, base_sqsh)
    log(f"base: {base_name} ({base_meta['files'][0]['size']//(1<<20)} MiB)")

    tree = os.path.join(workdir, "rootfs")
    os.makedirs(tree)
    log("unpacking base rootfs…")
    # -no-xattrs: the base may carry SELinux xattrs that need root to restore;
    # EdgeNOS doesn't use SELinux, so drop them rather than fail as non-root.
    run(["unsquashfs", "-q", "-no-xattrs", "-f", "-d", tree, base_sqsh])

    # --- overlay component packages (those present for this arch/asic) ---
    included = [base_meta_summary(base_meta)]
    missing = []
    for comp in view["platform"].get("components", []):
        cand = sorted(glob.glob(os.path.join(pkgs_dir, f"{comp}_*_{arch}-{asic}.epk")))
        if not cand:
            missing.append(comp)
            continue
        cepk = cand[-1]
        cm = epk.read_manifest(cepk)
        ok, problems = epk.verify(cepk)
        if not ok:
            raise SystemExit(f"error: {cepk} failed verify: {problems}")
        epk.extract_payload(cepk, tree)
        included.append(cm)
        log(f"overlaid {comp}: {os.path.basename(cepk)} ({len(cm.get('files', []))} files)")
    if missing:
        log(f"NOTE: {len(missing)} DB component(s) not yet packaged, kept from base bits: "
            + ", ".join(missing))

    # --- stamp ---
    ident = stamp_identity(tree, args.platform, [m for m in included if m.get("type") != "base"], args.epoch)
    log(f"stamped {ident['version_string']}")

    # --- re-squash ---
    out_sqsh = os.path.join(workdir, "rootfs.sqsh")
    log("building rootfs squashfs…")
    run(["mksquashfs", tree, out_sqsh, "-all-root", "-comp", "xz", "-no-xattrs",
         "-noappend", "-no-progress", "-quiet"])
    return out_sqsh, ident, included, missing


def base_meta_summary(base_meta):
    return {"name": "base", "version": base_meta["version"], "arch": base_meta["arch"],
            "asic": base_meta["asic"], "type": "base"}


# ----------------------------------------------------------- installer wrappers
def wrap_onie_sfx(recipe, sqsh, ident, args, outdir):
    cfg = recipe["onie_sfx"]
    template = os.path.join(args.source_root, cfg["installer_template"])
    fit = os.path.join(args.source_root, cfg["fit"])
    for p in (template, fit):
        if not os.path.exists(p):
            raise SystemExit(f"error: onie-sfx input missing: {p}")
    fit_name = cfg.get("fit_name_in_payload", "uImage-powerpc.itb")
    rootfs_name = cfg.get("rootfs_name_in_payload", "rootfs.sqsh")

    # version-template the installer script
    script = open(template, encoding="utf-8").read()
    script = re.sub(r'NOS_VERSION="[^"]*"', f'NOS_VERSION="{ident["version"]}"', script)

    with tempfile.TemporaryDirectory() as td:
        shutil.copy(fit, os.path.join(td, fit_name))
        shutil.copy(sqsh, os.path.join(td, rootfs_name))
        payload_tar = os.path.join(td, "payload.tar")
        with tarfile.open(payload_tar, "w") as t:           # template untars this
            for n in (fit_name, rootfs_name):
                t.add(os.path.join(td, n), arcname=n)
        out = os.path.join(outdir, f"EdgeNOS-{ident['version']}-{ident['onie_platform']}.bin")
        with open(out, "wb") as o:
            o.write(script.encode())
            if not script.endswith("\n"):
                o.write(b"\n")
            with open(payload_tar, "rb") as p:
                shutil.copyfileobj(p, o)
        os.chmod(out, 0o755)
    return out


def wrap_onl_swi(recipe, sqsh, ident, args, outdir):
    """Build the ONL SWI (zip of rootfs + manifest). The final mkshar ONIE-installer
    wrap is attempted if the ONL tool + payload scaffold are available."""
    import zipfile
    cfg = recipe.get("onl_swi", {})
    manifest = {
        "version": "1.0",
        "platform": ident["onie_platform"],
        "arch": ident["arch"],
        "asic": ident["asic"],
        "edgenos_version": ident["version"],
        "build_id": ident["build_id"],
        "kernel": ident["kernel"],
    }
    swi = os.path.join(outdir, f"EdgeNOS-{ident['version']}-{ident['onie_platform']}.swi")
    with zipfile.ZipFile(swi, "w", zipfile.ZIP_STORED) as z:
        z.write(sqsh, "rootfs-" + ident["arch"] + ".sqsh")
        z.writestr("manifest.json", json.dumps(manifest, indent=2))
    log(f"built SWI {os.path.basename(swi)}")

    mkshar = cfg.get("mkshar")
    if mkshar:
        mkshar = os.path.join(args.source_root, mkshar)
    if mkshar and os.path.exists(mkshar):
        log("note: mkshar present; ONIE-installer wrap can be produced with the ONL "
            "installer scaffold (run inside the ONL builder for full fidelity).")
    else:
        log("note: SWI built; final ONIE self-installer wrap (mkshar) deferred — "
            "needs the ONL installer scaffold/tooling.")
    return swi


def main(argv):
    ap = argparse.ArgumentParser(description="Build an EdgeNOS image/installer for a platform")
    ap.add_argument("platform", help="ONIE platform string (switch DB key)")
    ap.add_argument("--source-root", default=".", help="root for recipe-relative input paths")
    ap.add_argument("--epoch", type=int, default=int(os.environ.get("SOURCE_DATE_EPOCH", "0")))
    ap.add_argument("-o", "--outdir", default=os.path.join(ROOT, "output", "images"))
    args = ap.parse_args(argv[1:])

    try:
        view = SwitchDB().resolve(args.platform)
    except DBError as e:
        raise SystemExit(f"error: {e}")
    recipe = load_recipe(args.platform)
    installer = recipe.get("installer") or view["platform"]["installer"]
    os.makedirs(args.outdir, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="edgenos-img-") as wd:
        sqsh, ident, included, missing = compose_rootfs(view, recipe, args, wd)
        sz = os.path.getsize(sqsh)
        log(f"composed rootfs.sqsh: {sz//(1<<20)} MiB")
        if installer == "onie-sfx":
            out = wrap_onie_sfx(recipe, sqsh, ident, args, args.outdir)
        elif installer == "onl-swi":
            out = wrap_onl_swi(recipe, sqsh, ident, args, args.outdir)
        else:
            raise SystemExit(f"error: unknown installer type '{installer}'")

    print()
    print(f"=> {os.path.relpath(out, ROOT)}  ({os.path.getsize(out)//(1<<20)} MiB)")
    print(f"   {ident['pretty_name']}")
    print(f"   packages: " + ", ".join(f"{m['name']}-{m['version']}" for m in included))
    if missing:
        print(f"   (from base, not yet repackaged: {', '.join(missing)})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
