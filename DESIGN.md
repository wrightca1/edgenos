# EdgeNOS — Unified Version & Package Architecture

EdgeNOS targets multiple switches that differ along **three orthogonal axes**, and
more of each are coming:

- **arch** — the host CPU (powerpc, armhf, … arm64, x86_64)
- **asic** — the switch silicon (bcm56846, bcm56340, … future)
- **platform** — the physical board (a specific arch × asic × ports × firmware)

The design keeps these axes separate so that **adding a switch is data + a plugin
dir, never a fork.**

## Keystone: the switch database

`switchdb/` is the single source of truth. Both the version system and the package
system read from it; nothing hard-codes a board.

```
switchdb/
  platforms/<onie-string>.yml   bill-of-materials + build params for one board
  arch/<arch>.yml               toolchain triple, kernel arch, endianness, quirks
  asic/<asic>.yml               SDK stack, CMIC variant, kernel modules, datapath
  schema/*.schema.json          contract every entry must satisfy (CI-validated)
```

A platform entry *references* an arch and an asic by id. `tools/switchdb.py`
`resolve()` merges the three into one view consumed downstream:

```yaml
onie_platform: arm-accton-as4610-54-r0   # the primary key
arch: armhf            # -> switchdb/arch/armhf.yml
asic: bcm56340         # -> switchdb/asic/bcm56340.yml
kernel: "4.14"
datapath: bcmd
components: [bcmd, linux-kernel-bde, linux-user-bde, linux-bcm-knet, quagga, onlp, platform-svc]
firmware:   [bcm84758_ucode]
installer:  onl-swi
persistence: onl-swi
```

## Version system

One semver release (`edgenos/VERSION`, e.g. `0.1.0`) resolves **per platform** to a
build identity that carries arch/asic/kernel + git SHA + build timestamp. This is
ONL's `make-versions` model, generalized and platform-aware.

`packaging/version/version.py <onie_platform>` emits:
- `version.json` → shipped to `/etc/edgenos/version.json` (and read back by `show version`)
- `os-release` fields → `/etc/os-release` (`ID=edgenos`, `EDGENOS_ARCH/ASIC/PLATFORM/KERNEL`)

Builds are reproducible: the timestamp comes from `--epoch` / `SOURCE_DATE_EPOCH`,
never the wall clock. A release ultimately pins a **lockfile** of exact package
versions per platform (Phase 2).

Today neither NOS has any version identity — the 5610 has a hard-coded `"0.1.0"`
string used only in installer console output; the 4610 inherits a *stale* ONL stamp.
This replaces both.

## Package system — hybrid, custom `.epk`

Every component builds into an **arch/ASIC-tagged package**:
`edged_0.1.0_powerpc-bcm56846.epk`, `bcmd_0.1.0_armhf-bcm56340.epk`.

An `.epk` is an uncompressed outer tar holding `manifest.json` (name, version, arch,
asic, depends, `type: base|overlay`, `runtime_installable`, pre/post hooks, per-file
sha256) + `data.tar.xz` (payload). Payload is **xz**, not zstd, so the on-box `epkg`
is pure Python stdlib (`tarfile`/`lzma`) with zero extra deps. Builds are reproducible
(normalized mtime/uid/gid, sorted members, `SOURCE_DATE_EPOCH`).

- **Build-time (immutable base):** the image recipe reads a platform's `components:`
  list, pulls the matching `.epk`s, lays them into the rootfs, builds the squashfs +
  installer.
- **Runtime (selective overlays):** on-box `epkg install foo.epk` validates the
  package's arch/asic against the running platform's DB entry, lands it on the writable
  overlay, runs hooks, restarts the unit — for components flagged `runtime_installable`
  (datapath, firmware, control-plane).

Immutable base + selective live overlays = the chosen hybrid model. (Builder + on-box
installer land in Phase 2.)

## Repo layout

```
edgenos/
  switchdb/        the database (above)
  core/            shared, arch/ASIC-agnostic: datapath framework, control-plane, platform-svc
  arch/<arch>/     per-arch toolchain + kernel support
  asic/<asic>/     per-ASIC SDK glue, port logic, PHY/serdes
  platform/<board>/ DTS, CPLD, ONLP, portmap, firmware list, persistence model
  packaging/       pkgtool (build .epk + on-box installer), version (stamper)
  images/          per-platform image recipe: resolve DB -> packages -> installer
  tools/           switchdb.py and friends
  bin/edgenos      top-level CLI
```

## Adding a new switch (the whole checklist)

1. `switchdb/platforms/<onie-string>.yml` — the bill-of-materials.
2. `platform/<board>/` — DTS, CPLD, ONLP, portmap, firmware.
3. New CPU? add `switchdb/arch/<arch>.yml` + `arch/<arch>/`. New silicon? add
   `switchdb/asic/<asic>.yml` + `asic/<asic>/`.
4. `edgenos build <onie-string>` → reads DB, builds/pulls `.epk`s, emits the installer.

No copied recipes, no second init system, no divergent overlays. arch and asic are
plugins selected by data.

## Prior art: ONL (and where EdgeNOS mirrors vs. differs)

EdgeNOS deliberately borrows ONL's platform model, with one key difference:

| Aspect | ONL | EdgeNOS |
|--------|-----|---------|
| Platform key | ONIE platform string | **same** (the switch-DB key) |
| Per-board source | `packages/platforms/<v>/<arch>/<board>/` | **same shape** — `platform/<board>/` |
| Platform class | `OnlPlatform_<string>` with `baseconfig()` | **same idea** — `EdgeNOSPlatform_<string>` in `platform/<board>/platform.py` |
| HW abstraction | ONLP C HAL (sfp/fan/psu/led/thermal), board lib bound by boot symlink | **ONLP-style** `PlatformHAL` seam in `core/platform/base.py` |
| Device registry | *derived* by scanning packages | **explicit** `switchdb/` + `edgenos catalog` (you wanted a database) |
| **Image scope** | **one per arch, all boards, detect at boot** | **one per switch** ("pick your switch") |

So the layout, the per-board platform class, the `baseconfig()` bring-up, and the HAL
are ONL-like; the image is per-switch, and the supported-device list is a declarative
database rather than something derived by enumeration. `core/platform/current.py`
resolves the platform (version.json → os-release → `onie-sysinfo`) and loads the board
class — the ONL `current.py` analog, but in a per-switch image it resolves the single
board.

## Roadmap

- **Phase 1 (done):** switch DB + schema + the two known platforms; version stamper; CLI.
- **Phase 2 (done):** `pkgtool` — `.epk` format (`epk.py`), builder (`pkgbuild.py`),
  on-box `epkg` (`epkg.py`); `edged` packaged end-to-end from real newnos artifacts.
  Reproducible builds, checksum + tamper verification, arch/ASIC install guard.
- **Phase 3 (done):** image recipe — `pkgbase.py` captures a proven rootfs into a
  first-class `base_<ver>_<arch>-<asic>.epk`; `imgbuild.py` composes base + component
  `.epk`s, stamps os-release/version.json + a self-describing installed-pkg DB,
  re-squashes, and wraps per platform (`onie-sfx` `.bin` for the 5610, `onl-swi` `.swi`
  for the 4610). `catalog` lists downloadable switches from the DB. Both real installers
  produced + validated. (The 4610 final `mkshar` ONIE-wrap runs in the ONL builder.)
- **Phase 4 (in progress):** migrate the boards onto `core/`+`platform/`.
  - **AS5610 source migrated** (`MIGRATION.md`): `edged` split into `core/datapath` +
    `asic/bcm56846` + the board dir; build wired; structure validated (compile pending
    the cross-build container).
  - **ONL-style platform layer**: per-board class + `baseconfig()` + ONLP-style HAL +
    resolver (see "Prior art: ONL").
  - **Control plane → `core/control-plane/`** (quagga: recipe + config + units).
  - **AS5610 image fully package-composed**: all 6 components are real `.epk`s
    (`edged`, `linux-kernel-bde`, `linux-user-bde`, `bde-tmon`, `quagga` [asic=any],
    `platform-svc` [board drivers + bring-up + platform class]) — nothing "from base
    bits". The shipped image is self-describing (installed-pkg DB lists all 6).
  - Next: migrate the 4610 onto the same framework; flesh out the HAL with real CPLD/
    sensor reads; then retire the forks once the unified build is container-validated.

## Current support matrix

| Platform | arch | asic | kernel | datapath | status |
|----------|------|------|--------|----------|--------|
| `powerpc-accton_as5610_52x-r0` | powerpc | bcm56846 (Trident+) | 5.10 | edged | production |
| `arm-accton-as4610-54-r0` | armhf | bcm56340 (Helix4) | 4.14 | bcmd | production |
