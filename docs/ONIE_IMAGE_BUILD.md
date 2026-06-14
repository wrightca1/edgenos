# Building an ONIE Image for EdgeNOS (AS5610-52X)

This document explains, in plain terms, how the installable EdgeNOS image is
built — what the pieces are, what each build script does, the order to run them
in, and the environment quirks that will bite you. If you only want to ship a
new `edged` and reflash, skip to [The fast path](#the-fast-path).

---

## 1. What an ONIE image actually is

The deliverable is a single file:

```
output/images/edgenos-as5610-52x-dualslot.bin     (~34 MB)
```

It is a **self-extracting shell script with a tar archive appended**. ONIE runs
the shell header (`installer/install-dual-slot.sh`); the header finds the
`__ARCHIVE__` marker, untars the payload, and writes it to disk. The payload has
exactly two members:

| Member | What it is | Where it lands |
|---|---|---|
| `uImage-powerpc.itb` | A U-Boot **FIT image**: gzip kernel + flattened DTB + initramfs | raw partition (sda5 or sda7) |
| `rootfs.sqsh` | xz-compressed **squashfs** root filesystem | raw partition (sda6 or sda8) |

So building an image is really three sub-builds that get stitched together:

```
  kernel + DTB + initramfs  ─┐
                             ├─►  uImage-powerpc.itb (FIT) ─┐
  (out-of-tree modules)      │                              ├─► .bin installer
                             │                              │
  Buildroot userland         ├─►  rootfs.sqsh  ────────────┘
  + edged + quagga + overlay ┘
```

The single most useful thing to understand: **the kernel/DTB/initramfs side and
the rootfs side are independent.** If you only changed userspace (e.g. `edged`),
the FIT does not change and can be reused as-is — you only rebuild `rootfs.sqsh`.
That is why the fast path is fast.

---

## 2. The components and where they come from

| Piece | Built by | Source of truth |
|---|---|---|
| Linux kernel `uImage` + `as5610-52x.dtb` | `scripts/build-kmodules.sh` | `config/kernel/as5610_defconfig`, `kernel/dts/as5610-52x.dts`, `kernel/patches/` |
| Out-of-tree `.ko` (bde, cpld, tmon, retimer) | `scripts/build-kmodules.sh` | `asic/bde`, `platform/cpld`, `platform/retimer`, `asic/tmon` |
| ASIC SDK libs + `edged` | `scripts/rebuild-edged-with-sdk.sh` | `asic/openmdk`, `asic/mdk-init`, `asic/edged` |
| OSPF daemons (`zebra`, `ospfd`) | `scripts/build-quagga.sh` | static Quagga 1.2.4 |
| Root filesystem (`rootfs.sqsh`) | `scripts/assemble-rootfs-from-base.sh` | a proven Buildroot base + `config/rootfs/overlay` + the artifacts above |
| FIT (`uImage-powerpc.itb`) | `scripts/package-image.sh` | kernel.gz + DTB + `initramfs/nos-init.c` |
| `.bin` installer | `installer/build-image.sh --dual-slot` | the FIT + `rootfs.sqsh` + `installer/install-dual-slot.sh` |

`output/` and `asic/openmdk/` are **git-ignored**. A clean checkout therefore has
no build artifacts and a bare OpenMDK tree — see step 0 below.

---

## 3. The build environment

Builds run in Docker (rootless is fine for everything except the from-scratch
Buildroot base). Set the socket once per shell:

```bash
export DOCKER_HOST=unix:///run/user/1000/docker.sock
```

Three images are referenced by the scripts. **Know which is which:**

| Image | Used for | Has PPC cross-gcc? | squashfs-tools |
|---|---|---|---|
| `edgenos-builder` (alias of `edgenos/builder9:1.8-rootless`) | rootfs assembly, quagga | **no** (current image lost it) | **4.3** (old) |
| `debian:bullseye` | `quick-rebuild-edged.sh` (apt-installs cross-gcc each run) | yes (installed at runtime) | n/a |
| host (Fedora) | native `assemble-rootfs-from-base.sh`, `build-image.sh` | — | **4.5** (new) |

> **Two environment facts that matter.** (1) The current `edgenos-builder`
> image's `squashfs-tools` is **4.3**, which cannot *unsquash* a v4.5-made
> rootfs (it errors `Data queue size is too large`). (2) That same image is
> **missing the PPC cross-compiler**, so `package-image.sh` (which compiles the
> initramfs) fails inside it. Both are worked around below by running the
> affected steps **natively on the host**, which has squashfs-tools 4.5 and, for
> the initramfs, the cross-gcc (or use `debian:bullseye`). These are documented
> so you don't chase them again.

---

## 4. Full build from a clean checkout

```bash
export DOCKER_HOST=unix:///run/user/1000/docker.sock
cd /home/smiley/edgecore/newnos

# 0. restore the OpenMDK divergence and build the builder image
scripts/apply-openmdk-patches.sh
docker build --network host -t edgenos-builder .

# (optional) cheap sanity gate that guards hard-won fixes from regressing
scripts/pre-build-checks.sh

# 1. kernel uImage + DTB + out-of-tree modules + versioned module tree
KVER=6.1.175 scripts/build-kmodules.sh
#    -> output/kernel/{uImage,as5610-52x.dtb,modules/}, output/modules/*.ko

# 2. ASIC SDK libs + edged  (result MUST be ~19 MB; a ~2 MB edged links but crashes)
scripts/rebuild-edged-with-sdk.sh
#    -> output/edged-rebuilt

# 3. OSPF control-plane daemons
scripts/build-quagga.sh
#    -> output/{zebra,ospfd}-ppc

# 4. rootfs.sqsh = proven base + current overlay + edged + modules + quagga
#    Run NATIVELY (host squashfs-tools 4.5); SRC overrides the in-container path.
SRC=$(pwd) bash scripts/assemble-rootfs-from-base.sh
#    -> output/images/rootfs.sqsh

# 5. FIT (only needed if kernel/DTB/initramfs changed). Needs a PPC cross-gcc,
#    so run it where one exists (debian:bullseye apt-installs it, or a host gcc):
docker run --rm --network=host -v "$(pwd):/build/src" --entrypoint /bin/bash \
    debian:bullseye -c 'apt-get update -qq && apt-get install -y -qq \
      gcc-powerpc-linux-gnu u-boot-tools cpio gzip >/dev/null && \
      bash /build/src/scripts/package-image.sh'
#    -> output/images/uImage-powerpc.itb  (+ a single-slot .bin)

# 6. dual-slot ONIE installer
bash installer/build-image.sh --dual-slot
#    -> output/images/edgenos-as5610-52x-dualslot.bin
```

> **Where does the "proven base" rootfs in step 4 come from?**
> `assemble-rootfs-from-base.sh` does **not** run Buildroot — it unsquashes an
> existing `output/images/rootfs.sqsh`, swaps in the new edged/modules/overlay,
> and re-squashes. The base itself is a **Buildroot 2023.02.9** userland
> (systemd, openssh, iproute2, ethtool, i2c-tools, u-boot-tools). It was built
> once with the from-scratch Buildroot flow under **root** Docker (rootless
> Docker can't chown Buildroot's high UIDs). Day to day you never rebuild it;
> you assemble on top of it. Keep a known-good `rootfs.sqsh` around as the base.

---

## 5. The fast path

Most changes are userspace-only (`edged`, overlay scripts, configs). The kernel,
DTB, and initramfs are unchanged, so **reuse the existing FIT** and skip step 5
entirely:

```bash
export DOCKER_HOST=unix:///run/user/1000/docker.sock
cd /home/smiley/edgecore/newnos

# A. rebuild edged  (full SDK-correct build, ~19 MB)
scripts/rebuild-edged-with-sdk.sh && cp asic/edged/edged output/edged-rebuilt
#    or, for a tiny bcm56840 datapath tweak, the minimal relink:
#    scripts/quick-rebuild-edged.sh && cp asic/edged/edged output/edged-rebuilt

# B. re-squash the rootfs with the new edged + current overlay (native)
SRC=$(pwd) bash scripts/assemble-rootfs-from-base.sh

# C. stitch the (unchanged) FIT + new rootfs into the dual-slot installer
bash installer/build-image.sh --dual-slot
#    -> output/images/edgenos-as5610-52x-dualslot.bin
```

This is the exact path used to ship the port-LED feature. It takes a couple of
minutes versus ~30+ for a full build.

### Verifying the image before you flash it

```bash
# The bin is a shell header + tar payload
grep -aoE 'uImage-powerpc.itb|rootfs.sqsh' output/images/edgenos-as5610-52x-dualslot.bin

# Spot-check the baked rootfs really has your new edged
rm -rf /tmp/vr && unsquashfs -no-xattrs -d /tmp/vr output/images/rootfs.sqsh \
    usr/sbin/edged >/dev/null 2>&1
sha256sum /tmp/vr/usr/sbin/edged output/edged-rebuilt   # should match
```

---

## 6. Per-script reference

| Script | Runs where | Purpose |
|---|---|---|
| `scripts/apply-openmdk-patches.sh` | host | Restores EdgeNOS's OpenMDK changes (Warpcore driver/ucode, bcm56840 datapath, etc.) into the git-ignored `asic/openmdk` tree. Run once on a fresh checkout. |
| `scripts/build-kmodules.sh` | `debian:bookworm` | Re-fetches/configures the kernel the same way the image build does (so vermagic + symbol CRCs match), builds `uImage`+DTB, the full versioned module tree, and the out-of-tree `.ko`s. `KVER=` selects the version. |
| `scripts/rebuild-edged-with-sdk.sh` | `edgenos-builder` | The canonical `edged` build: builds the 16 OpenMDK SDK `.a` libs via `mdk-init`, then links `edged`. Output `output/edged-rebuilt`; asserts > 15 MB. |
| `scripts/quick-rebuild-edged.sh` | `debian:bullseye` | Minimal incremental relink (recompiles a few bcm56840 datapath files + relinks). Fastest `edged` turnaround; needs the SDK already built. |
| `scripts/build-quagga.sh` | `edgenos-builder` | Cross-builds static Quagga 1.2.4 (`zebra`+`ospfd`) → `output/{zebra,ospfd}-ppc`. |
| `scripts/assemble-rootfs-from-base.sh` | host (`SRC=$(pwd)`) or container | Rebuilds `rootfs.sqsh` from the proven base + overlay + edged + modules + quagga, enabling all systemd services. **No Buildroot run.** |
| `scripts/package-image.sh` | needs PPC gcc | Compiles `initramfs/nos-init.c`, builds the FIT (`uImage-powerpc.itb`), and produces a single-slot `.bin`. |
| `installer/build-image.sh [--dual-slot]` | host | Stitches the FIT + `rootfs.sqsh` behind the chosen installer header into the final `.bin`. |
| `scripts/pre-build-checks.sh` | host | Fails the build if a hard-won fix regressed (IND_40BITIF bit, PAXB sub-window 7, CPLD write bounds, no stale `switchd`, DS100DF410 CDR reset wired). |

---

## 7. Gotchas (all hit for real)

1. **`edged` size.** A ~2 MB `edged` links cleanly but crashes seconds after
   start (inconsistent SDK libs). Every consumer enforces **> 15 MB**; a good
   build is ~19 MB.
2. **squashfs-tools 4.3 vs 4.5.** The builder image's 4.3 can't unsquash a
   4.5-made image and needs `-da 64 -fr 64`. Run the assembly **natively on the
   host** (`SRC=$(pwd) bash scripts/assemble-rootfs-from-base.sh`).
3. **No cross-gcc in `edgenos-builder`.** `package-image.sh` compiles the
   initramfs and fails there; run it in `debian:bullseye` (apt-installs the
   toolchain) or on a host with `powerpc-linux-gnu-gcc`. Or skip it: if the
   kernel/DTB/initramfs didn't change, the existing FIT is still valid.
4. **FIT load addresses.** The current map (in `package-image.sh`) is kernel
   `0x0`, **DTB `0x03000000`, initramfs `0x03100000`**. The 6.1 kernel
   decompresses past the old 15 MB DTB address; if the DTB sits too low U-Boot
   aborts with `image is not a fdt`. Do not revert to the old `0x00f00000`.
5. **`-lgcc` for the initramfs.** `nos-init.c` is built freestanding
   (`-nostdlib -nostartfiles -nodefaultlibs`); PPC still needs `-lgcc` for the
   `_savegpr_*/_restgpr_*` helpers, or the link fails.
6. **Rootless Docker + Buildroot.** The from-scratch Buildroot base can't be
   built rootless (high-UID chown). Build the base once under root Docker, then
   only ever assemble on top of it.
7. **Junk files from the rebuild script.** A previous quoting bug in
   `quick-rebuild-edged.sh` created files named `Recompiling`/`Relinking`/
   `Updating` in the repo root (fixed); if you see them, they're harmless to
   delete.

---

## See also

- [`DUAL_SLOT.md`](DUAL_SLOT.md) — how the A/B slots, U-Boot env, and rollback work, and how `nos-upgrade` flashes a slot.
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — what the running software is and where the knowledge came from.
- [`../BOOT.md`](../BOOT.md) — the U-Boot → FIT → kernel boot chain in detail.
- [`../installer/ONIE_ISSUES.md`](../installer/ONIE_ISSUES.md) — the BusyBox quirks the installer works around.
