# EdgeNOS — System Summary

EdgeNOS is a unified, multi-architecture / multi-ASIC network operating system for
bare-metal Ethernet switches. A single source tree builds per-switch ONIE images
across different CPU architectures (PowerPC, ARM) and switching silicon (Broadcom
Trident/Helix families), driven by a database of switch definitions rather than
per-board forks.

This document summarizes the framework, the packaging system, the platform/HAL
layer, the on-box CLI, the image pipeline, and the new modular web UI.

---

## 1. Framework / repository model

Everything lives in one repository, organized around three independent axes:

- **platform** — a specific product (e.g. `accton-as5610-52x`, `accton-as4610-54`),
  identified by its ONIE platform string.
- **arch** — the CPU architecture (`powerpc`, `armhf`, …).
- **asic** — the switching silicon (`bcm56846`, `bcm56340`, …).

A **switch database** (`switchdb/`, YAML) maps each ONIE platform string to its
arch, asic, and the list of components it ships. `tools/switchdb.py` resolves a
platform to its full build recipe. Adding a new switch is a data change (a new YAML
file), not a new code fork.

```
switchdb/platforms/accton-as5610-52x.yml   # platform -> arch/asic/components
platform/accton-as5610-52x/                # board specifics: DTS, drivers, platform class
core/                                      # shared framework (platform base, HAL, CLI, webui)
packaging/                                 # package format + build/install tooling
build/                                     # FIT / image / SDK build harnesses
```

---

## 2. Versioning

Platform identity and version are carried in two on-box files:

- `/etc/edgenos/version.json` — version string, platform, arch, asic, kernel,
  datapath daemon.
- `/etc/os-release` — standard fields plus `EDGENOS_ARCH/ASIC/PLATFORM/KERNEL`.

The on-box tooling resolves the running platform from these (falling back to the
ONIE sysinfo) so the same binaries behave correctly on any supported switch.

---

## 3. Packaging — the `.epk` format

EdgeNOS uses a **hybrid package format** designed to install on switches whose
Python is a minimal build with **no compression modules** (no `zlib`/`lzma`):

- An `.epk` is an **uncompressed outer tar** containing `manifest.json` +
  `data.tar.gz`.
- Compression is done with the **`gzip` CLI** via subprocess (the switches ship the
  `gzip`/`xz` binaries even though Python can't compress in-process).
- Everything is **sha256-hashed** — the payload as a whole and each file — and the
  hashes are verified on install.

Tooling:

| Tool | Role |
|------|------|
| `packaging/pkgtool/epk.py` | format library (pack/unpack, gzip via CLI, hashing) |
| `packaging/pkgtool/pkgbuild.py` | build an `.epk` from a spec (`packaging/specs/*.yml`) |
| `packaging/pkgtool/epkg.py` | on-box package manager (install/remove/verify/list/info) |

The on-box installed-package database lives at
`/var/lib/edgenos/epkg/installed/<name>.json` — this is the **source of truth for
"what's installed"**, and (see §6) what the web UI uses to decide which feature
pages to show. Packages can be marked `runtime_installable` and carry `postinst`
hooks (e.g. enable/restart a systemd unit).

---

## 4. Platform layer & HAL

Modeled after Open Network Linux's platform abstraction:

- **`EdgeNOSPlatformBase`** — declares the board's bring-up: driver load order
  (with params) and post-driver init phases, mirroring the proven boot sequence.
- **`PlatformHAL`** — uniform hardware access:
  - **thermals** (hwmon),
  - **optics / SFPs** (SFF-8472/8636 decode from the eeprom sysfs nodes),
  - **fans, PSUs, LEDs** — per-board, via the platform CPLD (5610: CPLD sysfs;
    4610: CPLD over i2c).

Robustness notes from this work:
- SFP eeprom reads are **bounded with a timed subprocess** so a flaky/empty i2c
  cage can't block the caller.
- The **5610 PSU decode** was corrected to the proven register map (PSU1 status in
  CPLD reg `0x02`, PSU2 in `0x01`; present is **active-low**, power-good = bit1).

---

## 5. On-box CLI & images

- **CLI** (`/usr/sbin/edgenos`, from the `edgenos-cli` package):
  `edgenos version | platform [name|show|init|hal] | pkg [list|info|verify|install|remove]`.
- **Images** — per-switch, ONL-style ONIE installers. The image pipeline composes
  the resolved component set (datapath daemon, platform-svc, CLI, routing stack,
  …) into a bootable, ONIE-installable image for each switch, with the kernel +
  device tree packaged as a FIT.

---

## 6. Web UI (new)

A **lightweight, modular, capability-driven web UI** for EdgeNOS switches.

**Design**
- **Pure Python standard library** (`http.server` + a small router) — no Flask, no
  external dependencies, because the switches' Python is minimal.
- **Modular**: each feature is a module under `core/webui/modules/` that declares a
  `detect()`. The navigation only shows modules that are **present on this box** —
  e.g. the OSPF page appears only where `ospfd` is running; a future BGP page drops
  in the same way. The **Apps** page can install features that aren't present yet
  (via `epkg`), after which their pages appear automatically.
- **Security**: the server **binds to the management interface's IP only**
  (auto-detected). Exposing it on other interfaces is an explicit opt-in
  (`allow_all=1` / interface list in `/etc/edgenos/webui.conf`).
- **Install-on-demand**: shipped as the `edgenos-webui` package — *not* baked into
  the image by default; you install it when you want it.

**Pages**
- **Dashboard** — identity, sensors (HAL thermals/fans/PSUs), installed packages.
- **Interfaces** — show interface state and **configure IPv4 addresses** (applied
  live and recorded for persistence).
- **ECMP** — which interfaces participate in equal-cost multipath, and the
  multipath routes themselves (kept separate from the Interfaces page).
- **OSPF** — neighbors, router-id, **learned routes** (each network with the router
  / interface it was learned from, ECMP-badged), and live config changes (add a
  network) applied **through the ospfd vty without restarting the daemon**.
- **Apps** — installed packages and features available to install.

**Robustness**
- All routing-daemon (vty) queries are **bounded** (idle detection + a hard
  deadline) so a slow daemon can never hang a page; OSPF output is cached briefly so
  reloads are instant.

---

## 7. Hardware / device-tree work done in this cycle

- **i2c mux flakiness root-caused and fixed.** The Accton boards use pca954x i2c
  muxes for the SFP cages. The device trees set the **legacy `deselect-on-exit`**
  property, which the 6.1 kernel driver ignores, leaving the mux in
  *idle = as-is* (it holds the last-selected channel → cross-talk / ghost
  addresses / stale reads). Replaced it with the property the driver honors,
  **`i2c-mux-idle-disconnect`**, on every pca9548 node. Verified on hardware
  (`idle_state` now `-2 = disconnect`, SFP eeproms read coherently).
- **FIT builds migrated into the unified tree.** `build/build-fit-4610.sh` and
  `build/build-fit-5610.sh` now compile the **canonical** device trees in the
  EdgeNOS repo (no fork dependency), giving a single source of truth.

---

## 8. Validation status

- **AS5610-52X** (PowerPC / BCM56846): datapath up, OSPF running, web UI live on the
  management interface; PSU/sensor reporting corrected; OSPF learned routes and ECMP
  views verified against the live router.
- **AS4610-54T** (ARM / BCM56340): datapath up, OSPF running, web UI deployed; i2c
  mux fix flashed and verified on hardware.

---

*Generated as part of the EdgeNOS development effort. The system is multi-switch by
construction — new platforms are added as data (a switch-DB entry + board specifics),
and new features are added as packages and self-registering UI modules.*
