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

## Roadmap

- **Phase 1 (done):** switch DB + schema + the two known platforms; version stamper; CLI.
- **Phase 2 (done):** `pkgtool` — `.epk` format (`epk.py`), builder (`pkgbuild.py`),
  on-box `epkg` (`epkg.py`); `edged` packaged end-to-end from real newnos artifacts.
  Reproducible builds, checksum + tamper verification, arch/ASIC install guard.
- **Phase 3:** image recipe — DB → BOM → squashfs → ONIE installer; reproduce the 5610 image from packages.
- **Phase 4:** migrate 5610, then 4610, onto `core/` + `platform/`; retire the forks.

## Current support matrix

| Platform | arch | asic | kernel | datapath | status |
|----------|------|------|--------|----------|--------|
| `powerpc-accton_as5610_52x-r0` | powerpc | bcm56846 (Trident+) | 5.10 | edged | production |
| `arm-accton-as4610-54-r0` | armhf | bcm56340 (Helix4) | 4.14 | bcmd | production |
