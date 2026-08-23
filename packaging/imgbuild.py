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
        onie-x86  -> x86 ONIE self-extracting installer: install.sh + tar(bzImage, initrd,
                     rootfs.sqsh, grub.cfg, bootx64.efi)  (.bin; GPT, BIOS + UEFI)
  7. (optional, recipe `qemu_disk:`) also emit a ready-to-boot disk image (qcow2/raw):
        MBR hybrid BIOS+UEFI, EDGENOS-BOOT (kernel/initrd/rootfs/grub.cfg) + EDGENOS-DATA
        (overlay persistence) — built rootless with genimage; for EVE-NG / containerlab / qemu.

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
        # a component .epk matches if its arch/asic is the board's or "any"
        cand = []
        for p in sorted(glob.glob(os.path.join(pkgs_dir, f"{comp}_*.epk"))):
            cm = epk.read_manifest(p)
            if cm["arch"] in (arch, "any") and cm["asic"] in (asic, "any"):
                cand.append((p, cm))
        if not cand:
            missing.append(comp)
            continue
        cepk, cm = cand[-1]
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
    # The ONL loader reads `platforms` (to match the box) and
    # version.SYSTEM_COMPATIBILITY_VERSION before booting a SWI — emit those, or the
    # loader rejects the image. Mirror ONL's manifest shape; carry EdgeNOS identity too.
    manifest = {
        "platforms": [ident["onie_platform"]],
        "arch": ident["arch"],
        "version": {
            "VERSION_ID": ident["version"],
            "BUILD_ID": ident["build_id"],
            "VERSION_STRING": ident["version_string"],
            "SYSTEM_COMPATIBILITY_VERSION": "2",
        },
        "edgenos": {
            "version": ident["version"],
            "asic": ident["asic"],
            "kernel": ident["kernel"],
            "datapath": ident["datapath"],
        },
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


def _template(text, ident):
    return (text.replace("@VERSION@", ident["version"])
                .replace("@VERSION_STRING@", ident["version_string"])
                .replace("@PLATFORM@", ident["onie_platform"]))


def _src(args, cfg, key, required=True):
    rel = cfg.get(key)
    if not rel:
        if required:
            raise SystemExit(f"error: recipe key '{key}' missing")
        return None
    p = os.path.join(args.source_root, rel)
    if required and not os.path.exists(p):
        raise SystemExit(f"error: input missing: {p}  (recipe '{key}')")
    return p


def _stage_x86_boot(cfg, sqsh, ident, args, dest):
    """bzImage, initrd.img, rootfs.sqsh, grub.cfg (templated) -> dest/. Shared by the
    onie-x86 installer and the qemu disk."""
    os.makedirs(dest, exist_ok=True)
    shutil.copy(_src(args, cfg, "kernel"), os.path.join(dest, "bzImage"))
    shutil.copy(_src(args, cfg, "initrd"), os.path.join(dest, "initrd.img"))
    shutil.copy(sqsh, os.path.join(dest, cfg.get("rootfs_name_in_payload", "rootfs.sqsh")))
    with open(_src(args, cfg, "grub_cfg"), encoding="utf-8") as f:
        grub = _template(f.read(), ident)
    with open(os.path.join(dest, "grub.cfg"), "w", encoding="utf-8") as f:
        f.write(grub)


def wrap_onie_x86(recipe, sqsh, ident, args, outdir):
    """x86 ONIE self-extracting installer: templated install.sh + tar payload."""
    cfg = recipe["onie_x86"]
    template = _src(args, cfg, "installer_template")
    grub_efi = _src(args, cfg, "grub_efi")
    with open(template, encoding="utf-8") as f:
        script = _template(f.read(), ident)
    if not script.rstrip("\n").endswith("__ARCHIVE_BELOW__"):
        raise SystemExit("error: onie-x86 installer template must end with the __ARCHIVE_BELOW__ marker")
    if not script.endswith("\n"):
        script += "\n"
    with tempfile.TemporaryDirectory() as td:
        stage = os.path.join(td, "payload")
        _stage_x86_boot(cfg, sqsh, ident, args, stage)
        shutil.copy(grub_efi, os.path.join(stage, "bootx64.efi"))
        payload_tar = os.path.join(td, "payload.tar")
        with tarfile.open(payload_tar, "w") as t:
            for n in sorted(os.listdir(stage)):
                t.add(os.path.join(stage, n), arcname=n)
        out = os.path.join(outdir, f"EdgeNOS-{ident['version']}-{ident['onie_platform']}.bin")
        with open(out, "wb") as o:
            o.write(script.encode())
            with open(payload_tar, "rb") as pt:
                shutil.copyfileobj(pt, o)
        os.chmod(out, 0o755)
    return out


_GENIMAGE_CFG = """\
image efi.vfat {{
    vfat {{ label = "EFI" }}
    size = {efi_mib}M
    mountpoint = "/efi"
}}
image boot.ext4 {{
    ext4 {{ label = "EDGENOS-BOOT" use-mke2fs = true }}
    size = {boot_mib}M
    mountpoint = "/boot"
}}
image data.ext4 {{
    ext4 {{ label = "EDGENOS-DATA" use-mke2fs = true }}
    size = {data_mib}M
    mountpoint = "/data"
}}
image disk.img {{
    hdimage {{
        align = 1M
    }}
    partition mbr {{
        in-partition-table = false
        image = "boot.img"
        offset = 0
        size = 512
        holes = {{"(440; 512)"}}
    }}
    partition grubcore {{
        in-partition-table = false
        image = "grub.img"
        offset = 512
    }}
    partition efi {{
        partition-type = 0xEF
        image = "efi.vfat"
        bootable = true
    }}
    partition boot {{
        partition-type = 0x83
        image = "boot.ext4"
    }}
    partition data {{
        partition-type = 0x83
        image = "data.ext4"
    }}
}}
"""


def emit_qemu_disk(recipe, sqsh, ident, args, outdir):
    """Ready-to-boot disk (qcow2/raw) for EVE-NG / containerlab / qemu: MBR hybrid
    (BIOS boot.img+core.img in the gap, EFI partition for UEFI), EDGENOS-BOOT, EDGENOS-DATA.
    Rootless: genimage + mke2fs -d + mtools, then qemu-img."""
    cfg = dict(recipe.get("onie_x86", {}))
    cfg.update(recipe["qemu_disk"])
    gi = cfg.get("genimage")
    genimage = os.path.join(args.source_root, gi) if gi else None
    if not genimage or not os.path.exists(genimage):
        genimage = shutil.which("genimage")
    if not genimage:
        raise SystemExit("error: genimage not found (recipe qemu_disk.genimage or PATH)")
    fmt = cfg.get("format", "qcow2")
    mbr, core, efidir = _src(args, cfg, "grub_bios_mbr"), _src(args, cfg, "grub_bios_core"), _src(args, cfg, "grub_efi_dir")
    sizes = dict(efi_mib=int(cfg.get("efi_mib", 16)), boot_mib=int(cfg.get("boot_mib", 384)),
                 data_mib=int(cfg.get("data_mib", 2048)))

    with tempfile.TemporaryDirectory(prefix="edgenos-disk-") as td:
        root, inp, tmp, outp = (os.path.join(td, d) for d in ("root", "input", "tmp", "out"))
        for d in (root, inp, tmp, outp):
            os.makedirs(d)
        # /boot : kernel + initrd + rootfs + grub.cfg ;  /efi : EFI/BOOT/bootx64.efi ; /data : empty
        _stage_x86_boot(cfg, sqsh, ident, args, os.path.join(root, "boot", "edgenos"))
        os.makedirs(os.path.join(root, "boot", "grub"))
        shutil.move(os.path.join(root, "boot", "edgenos", "grub.cfg"), os.path.join(root, "boot", "grub", "grub.cfg"))
        shutil.copytree(efidir, os.path.join(root, "efi"))
        os.makedirs(os.path.join(root, "data"))
        with open(os.path.join(root, "data", ".edgenos-data"), "w") as f:
            f.write(f"{ident['version_string']}\n")
        shutil.copy(mbr, os.path.join(inp, "boot.img"))
        shutil.copy(core, os.path.join(inp, "grub.img"))
        cfgp = os.path.join(td, "genimage.cfg")
        with open(cfgp, "w") as f:
            f.write(_GENIMAGE_CFG.format(**sizes))
        env = dict(os.environ)
        host_bin = os.path.dirname(genimage)
        env["PATH"] = host_bin + os.pathsep + os.path.join(os.path.dirname(host_bin), "sbin") + os.pathsep + env.get("PATH", "")
        log("building disk image (genimage)…")
        glog = os.path.join(outdir, "genimage.log")
        with open(glog, "w") as lf:
            r = subprocess.run([genimage, "--config", cfgp, "--rootpath", root, "--inputpath", inp,
                                "--tmppath", tmp, "--outputpath", outp], env=env,
                               stdout=lf, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            sys.stderr.write(open(glog).read()[-4000:])
            raise SystemExit(f"error: genimage failed (rc={r.returncode}); full log: {glog}")
        raw = os.path.join(outp, "disk.img")
        base = f"EdgeNOS-{ident['version']}-{ident['onie_platform']}"
        if fmt == "raw":
            out = os.path.join(outdir, base + ".img")
            shutil.copy(raw, out)
        else:
            out = os.path.join(outdir, base + ".qcow2")
            run(["qemu-img", "convert", "-f", "raw", "-O", "qcow2", raw, out])
    return out


def main(argv):
    ap = argparse.ArgumentParser(description="Build an EdgeNOS image/installer for a platform")
    ap.add_argument("platform", help="ONIE platform string (switch DB key)")
    ap.add_argument("--source-root", default=".", help="root for recipe-relative input paths")
    ap.add_argument("--epoch", type=int, default=int(os.environ.get("SOURCE_DATE_EPOCH", "0")))
    ap.add_argument("-o", "--outdir", default=os.path.join(ROOT, "output", "images"))
    ap.add_argument("--skip-installer", action="store_true", help="don't build the installer envelope")
    ap.add_argument("--skip-disk", action="store_true", help="don't emit the qemu disk image (recipe qemu_disk)")
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
        out = disk = None
        if not args.skip_installer:
            if installer == "onie-sfx":
                out = wrap_onie_sfx(recipe, sqsh, ident, args, args.outdir)
            elif installer == "onl-swi":
                out = wrap_onl_swi(recipe, sqsh, ident, args, args.outdir)
            elif installer == "onie-x86":
                out = wrap_onie_x86(recipe, sqsh, ident, args, args.outdir)
            else:
                raise SystemExit(f"error: unknown installer type '{installer}'")
        if recipe.get("qemu_disk") and not args.skip_disk:
            disk = emit_qemu_disk(recipe, sqsh, ident, args, args.outdir)

    print()
    for o in (out, disk):
        if o:
            print(f"=> {os.path.relpath(o, ROOT)}  ({os.path.getsize(o)//(1<<20)} MiB)")
    print(f"   {ident['pretty_name']}")
    print(f"   packages: " + ", ".join(f"{m['name']}-{m['version']}" for m in included))
    if missing:
        print(f"   (from base, not yet repackaged: {', '.join(missing)})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
