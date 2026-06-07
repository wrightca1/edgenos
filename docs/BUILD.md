# Building the EdgeNOS Image — Process & Gotchas

How to actually build `output/images/edgenos-as5610-52x.bin` on the current build
host (rootless Docker), including the gotchas that aren't obvious from the README.

**This doc covers the _build process_ only.** For everything downstream — FIT image
format, U-Boot environment, the ONIE install steps, and recovery — see
[`../BOOT.md`](../BOOT.md). For the 13 ONIE BusyBox installer quirks see
[`../installer/ONIE_ISSUES.md`](../installer/ONIE_ISSUES.md). Don't duplicate those here.

---

## TL;DR

```bash
export DOCKER_HOST=unix:///run/user/1000/docker.sock
cd newnos
docker build --network host -t edgenos-builder .          # rebuild after ANY source edit (source is COPYd in)
R="docker run --rm --network host -v $(pwd)/output:/build/output edgenos-builder"
$R kernel            # uImage + DTB              (rootless OK)
$R modules           # out-of-tree .ko          (rootless OK)
$R sdk               # OpenMDK libs + edged     (rootless OK)
# rootfs: needs ROOT docker (Buildroot chown). For a small change use the Fast path below.
```

Image layout (assembled by `scripts/build-installer.sh`; FIT internals in `BOOT.md`):
`edgenos-as5610-52x.bin` = `installer/install.sh` header + `payload.tar`
(`uImage-powerpc.itb` = kernel.gz + DTB + initramfs · `rootfs.sqsh`).

---

## Targets (`docker run … edgenos-builder <target>`)

| target    | builds                                    | rootless-safe? |
|-----------|-------------------------------------------|----------------|
| `kernel`  | `uImage` + `as5610-52x.dtb`               | ✅ |
| `modules` | out-of-tree kernel modules                | ✅ |
| `sdk`     | OpenMDK CDK/BMD/PHY libs (+ edged)         | ✅ |
| `rootfs`  | Buildroot base → `rootfs.sqsh`            | ❌ fails rootless (chown) |
| `image`   | FIT + ONIE installer `.bin`               | ⚠️ force-rebuilds initramfs |
| `all`     | …→**switchd**→rootfs→image                | ❌ broken (no `switchd` target) |

The builder Dockerfile **COPYs the source tree in at build time** — rebuild
`edgenos-builder` after editing `kernel/`, `config/`, or `asic/` (or bind-mount those
dirs per the README "Iterative Development" block).

---

## Fast path — DTS or rootfs-overlay change (no Buildroot)

For a `kernel/dts/*.dts` tweak and/or a `config/rootfs/overlay/*` file change, you do
**not** need Buildroot. Rebuild the DTB and re-squash the existing `rootfs.sqsh`:

```bash
export DOCKER_HOST=unix:///run/user/1000/docker.sock
cd newnos
docker build --network host -t edgenos-builder .                       # pick up edits

# (a) DTS change → rebuild DTB (also rebuilds uImage)
docker run --rm --network host -v $(pwd)/output:/build/output edgenos-builder kernel
strings output/kernel/as5610-52x.dtb | grep -c atmel,24c02            # sanity: edit landed

# (b) re-squash rootfs with the overlay file(s), then assemble FIT + .bin reusing the
#     (unchanged) initramfs. build-installer.sh REUSES initramfs.cpio.gz; the `image`
#     TARGET does not (it rebuilds it and needs Buildroot's busybox).
docker run --rm --network host --entrypoint /bin/bash \
    -v $(pwd)/output:/build/output edgenos-builder -c '
  set -e
  rm -rf /tmp/sqroot
  unsquashfs -no-xattrs -d /tmp/sqroot /build/output/images/rootfs.sqsh >/dev/null
  cp /build/config/rootfs/overlay/usr/sbin/platform-init.sh /tmp/sqroot/usr/sbin/
  rm -f /build/output/images/rootfs.sqsh
  mksquashfs /tmp/sqroot /build/output/images/rootfs.sqsh -comp xz -noappend -all-root -no-xattrs >/dev/null
  cp /build/output/images/initramfs.cpio.gz /build/initramfs.cpio.gz
  bash /build/scripts/build-installer.sh fit
  bash /build/scripts/build-installer.sh image
  ls -la /build/output/images/edgenos-as5610-52x.bin'

strings output/images/uImage-powerpc.itb | grep -c atmel,24c02        # DTB is in the FIT
```

The initramfs is just busybox + `nos-init` (kernel-version independent — see `BOOT.md`),
so reusing it across a DTS-only rebuild is safe.

---

## Rebuilding the SDK after a PHY/SDK source change

Editing `asic/openmdk/**` (e.g. the Warpcore driver) requires rebuilding the
**SDK libs** that edged statically links (16 `*.a` in `output/sdk/{bmd,phy,cdk,libbde}`),
not just edged. Use the script:

```bash
# edit e.g. asic/openmdk/phy/PKG/chip/bcmi_warpcore_xgxs/bcmi_warpcore_xgxs_drv.c
scripts/rebuild-edged-with-sdk.sh        # → output/edged-rebuilt (~18.9 MB)
```

**Verify the result is ~18.9 MB.** A ~2 MB edged means the SDK libs were
inconsistent/incomplete — it links without error but **crashes seconds after
start**. Keep `output/edged-v10`/`v11` (known-good KR4, 18.9 MB) as fallback.

Why a script and not `docker run … edgenos-builder sdk`? The `sdk` target works
for clean builds, but on this host two things bite (cost hours on 2026-06-03):

1. **SDK build dirs are root-owned** (`asic/openmdk/*/build` from a prior root
   docker build) → a rootless container can't write them. The script builds in a
   writable `/tmp` copy.
2. **`instpkgs.pl` is non-idempotent** — it errors if `pkgsrc/` is missing AND
   errors creating chip subdirs that already exist, so re-running it over a
   pre-generated tree dies (`Error creating directory …/pkgsrc/chip/bcm56526`).
   The script sidesteps it: copy your edited chip driver into the *generated*
   `pkgsrc/PKG/chip` **and** `pkgsrc/chip` copies, then `touch` the whole
   `pkgsrc` tree so instpkgs sees "up to date" and **skips** regeneration.

Other notes:
- The actual SDK build is `make -C asic/mdk-init MDK=… BLDDIR=…` (PHY_SYS_USLEEP
  etc. live in `PHY_CPPFLAGS`). **Not** `scripts/build-sdk.sh` — that one is
  broken (missing PHY_SYS_USLEEP, no chip curation; don't use it).
- The `_bde_dma_alloc` / "mdk-init link failed (DMA stubs)" error at the end is
  **benign** — the 16 libs build before it; only the optional test tool fails.
- The `edgenos-builder` image's `asic/openmdk/*/build/*.a` are **incomplete
  intermediates** (yield the 2 MB crashing edged) — don't copy those out; the
  real libs only exist after a full mdk-init build into `output/sdk`.

## Gotchas (all hit on 2026-06-02)

1. **Rootless Docker + Buildroot chown.** `rootfs` dies with
   `tar: Cannot change ownership to uid 197611 … Invalid argument` — rootless's userns
   can't map the high UIDs in Buildroot package tarballs. Use **system/root docker**
   for `rootfs`, or the Fast path (no Buildroot).

2. **`output/` owned by real root from old system-docker builds.** A rootless
   container (uid 0 → your host uid) can't overwrite/delete them. Classic blocker:
   `output/images/fitbuild/` (755 root) stops the `image` step with `Permission
   denied`. Workaround: `mv` the dir aside (parent `output/images` is 777, so a
   rename is allowed) and let the build recreate it; or swap in a me-owned copy
   (`mv output/sdk output/sdk.rootbak; cp -a output/sdk.rootbak output/sdk`).

3. **`all` aborts on `switchd`.** `build_switchd` runs `make -C /build/asic/switchd`
   which doesn't exist (this project uses `edged`). Run targets individually.

4. **squashfs xattrs.** `unsquashfs`/`mksquashfs` on the container overlay fs warn
   `failed to write xattr security.selinux … not supported` and exit non-zero
   (trips `set -e`). Add `-no-xattrs` to both.

5. **`image` target force-rebuilds the initramfs** (needs Buildroot busybox). Use
   `scripts/build-installer.sh fit` + `image`, which reuse `initramfs.cpio.gz`.

---

## Flashing & post-boot

Build host serves the `.bin` over HTTP; install from the ONIE shell. Full steps,
U-Boot env, and recovery: **[`../BOOT.md`](../BOOT.md)** (and `ONIE_ISSUES.md`).

```bash
cd output/images && python3 -m http.server 8080            # on build host (10.1.1.30)
# in ONIE: onie-nos-install http://10.1.1.30:8080/edgenos-as5610-52x.bin
# then fix U-Boot env (delete onie_boot_reason, restore nos_bootcmd) — see BOOT.md
```

Post-boot EdgeNOS: `edged.service` is disabled (`systemctl start edged`); `swp1` IP is
not persistent and the L3 punt programs on edged start
(`ip addr add 10.101.101.1/29 dev swp1 && systemctl restart edged`). See
`docs/DATAPATH_BRINGUP.md`.
