#!/usr/bin/env python3
"""
Generate per-switch instruction pages from the switch database.

Emits docs/switches/<onie_platform>.md for every switch (install + verify + recover)
and docs/switches/README.md as the index. Database-driven, so a new switch entry
yields its own page automatically — run `edgenos docs` to regenerate.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from switchdb import SwitchDB                            # noqa: E402

OUT = os.path.join(ROOT, "docs", "switches")


def version():
    return open(os.path.join(ROOT, "VERSION"), encoding="utf-8").read().strip()


def artifact(p, ver):
    ext = "bin" if p.get("installer") == "onie-sfx" else "swi"
    return f"EdgeNOS-{ver}-{p['onie_platform']}.{ext}"


def install_section(p, art):
    """ONIE install steps, tailored to the installer envelope."""
    common = f"""\
### 1. Get the switch into ONIE install mode

Power on (or reboot) and at the boot menu choose **ONIE → ONIE: Install OS**
(or from a running NOS: `onie-select -i -f` then reboot).

### 2. Install over the network

Serve `{art}` from any HTTP/TFTP server, then from the ONIE prompt:

```sh
onie-nos-install http://<your-server>/{art}
```

ONIE downloads the image, runs the EdgeNOS installer, writes the OS to disk,
sets the bootloader, and reboots into EdgeNOS automatically.
"""
    if p.get("installer") == "onie-sfx":
        common += """
> This switch uses a self-extracting installer (`.bin`): it partitions the disk,
> writes the kernel FIT + rootfs squashfs, and configures U-Boot.
"""
    else:
        common += """
> This switch uses an ONL-style installer (`.swi`): it installs the loader FIT +
> SWI under `/mnt/onl` and boots via the ONL loader.
"""
    return common


def page(db, key, ver):
    p = db.platforms[key]
    a, s = db.arch[p["arch"]], db.asic[p["asic"]]
    art = artifact(p, ver)
    comps = ", ".join(f"`{c}`" for c in p.get("components", []))
    L = []
    L.append(f"# EdgeNOS on {p['vendor'].title()} {p['model'].upper()}\n")
    L.append(f"> ONIE platform string: **`{key}`**  ·  EdgeNOS **{ver}**  ·  status: **{p.get('status','?')}**\n")
    L.append("## Hardware\n")
    L.append("| | |")
    L.append("|---|---|")
    L.append(f"| CPU | {a['name']} (`{a['id']}`) |")
    L.append(f"| Switch ASIC | {s['name']} (`{s['id']}`) |")
    L.append(f"| Kernel | {p['kernel']} |")
    L.append(f"| Datapath | `{p.get('datapath')}` |")
    L.append(f"| Verify the string on the box | `onie-sysinfo -p` → `{key}` |\n")
    L.append("## Download\n")
    L.append(f"Grab the installer for this switch from the EdgeNOS releases:\n")
    L.append(f"- **`{art}`**\n")
    L.append("## Install\n")
    L.append(install_section(p, art))
    L.append("## Verify after first boot\n")
    L.append("```sh")
    L.append("# version + platform identity")
    L.append("cat /etc/edgenos/version.json")
    L.append("cat /etc/os-release | grep EDGENOS_")
    L.append("# what the image is made of (self-describing package list)")
    L.append("ls /var/lib/edgenos/epkg/installed/")
    L.append("```\n")
    L.append(f"Expected: `EDGENOS_ARCH={p['arch']}`, `EDGENOS_ASIC={p['asic']}`, "
             f"`EDGENOS_KERNEL={p['kernel']}`.\n")
    L.append("## What's installed\n")
    L.append(f"Components on this image: {comps}.\n")
    L.append("## Recover / reinstall\n")
    L.append("Boot back into ONIE and either reinstall (`onie-nos-install …`) or "
             "uninstall (`onie-nos-uninstall`). ONIE is the safety net — it can always "
             "be re-entered from the boot menu.\n")
    L.append("---\n")
    L.append(f"_Generated from `switchdb/platforms/{p['vendor']}-{p['model']}.yml` "
             f"by `edgenos docs`._\n")
    return "\n".join(L)


def main(argv):
    db = SwitchDB()
    ver = version()
    os.makedirs(OUT, exist_ok=True)
    rows = []
    for key in db.list_platforms():
        p = db.platforms[key]
        fn = f"{key}.md"
        with open(os.path.join(OUT, fn), "w", encoding="utf-8") as f:
            f.write(page(db, key, ver))
        rows.append((p, fn))
    # index
    with open(os.path.join(OUT, "README.md"), "w", encoding="utf-8") as f:
        f.write("# EdgeNOS — supported switches\n\n")
        f.write("Pick your switch for download + install instructions.\n\n")
        f.write("| Switch | Arch | ASIC | Kernel | Installer | Instructions |\n")
        f.write("|--------|------|------|--------|-----------|--------------|\n")
        for p, fn in rows:
            f.write(f"| {p['vendor'].title()} {p['model'].upper()} | {p['arch']} | "
                    f"{p['asic']} | {p['kernel']} | {p.get('installer')} | [{fn}]({fn}) |\n")
    print(f"wrote {len(rows)} switch pages + index to docs/switches/")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
