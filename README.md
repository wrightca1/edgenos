# EdgeNOS

A unified, **multi-architecture / multi-ASIC** network operating system build system.
One **switch database** describes every supported switch; from it, EdgeNOS builds a
**per-switch ONIE installer** carrying a base system for that CPU + ASIC plus its
software components as packages.

Pick your switch → download its installer → `onie-nos-install`. That's it.

```
$ edgenos catalog
EdgeNOS 0.1.0 — supported switches

MODEL                  ARCH     ASIC       KERNEL  STATUS      DOWNLOAD
accton as4610-54       armhf    bcm56340   6.1     production  EdgeNOS-0.1.0-arm-accton-as4610-54-r0.swi
accton as5610-52x      powerpc  bcm56846   6.1     production  EdgeNOS-0.1.0-powerpc-accton_as5610_52x-r0.bin
```

## How it fits together

Four layers, each feeding the next — all driven by the switch database:

```
switchdb/         one YAML per switch (arch, asic, kernel, component list)
   │              + per-axis entries: arch/<arch>.yml, asic/<asic>.yml
   ▼
.epk packages     each software piece, tagged <name>_<ver>_<arch>-<asic>.epk
   │                base   = the whole base rootfs (one per switch)
   │                edged/bcmd/quagga/bde/platform-svc = the components
   ▼
imgbuild          base + components → stamp version → squashfs → installer
   │
   ▼
ONIE installer    one downloadable file per switch (.bin or .swi)
```

The three axes — **arch** (CPU), **asic** (silicon), **platform** (board) — are kept
orthogonal, so adding a switch is data + a folder, never a fork. The platform layer is
ONL-inspired (a per-board class with `baseconfig()` + an ONLP-style HAL + a resolver),
but images are **per switch**, not one-image-per-arch. See [`DESIGN.md`](DESIGN.md) and
the [ONL comparison](DESIGN.md#prior-art-onl-and-where-edgenos-mirrors-vs-differs).

## Install on a switch

Each switch has its own page under [`docs/switches/`](docs/switches/). In short:

```sh
# on the switch, in ONIE install mode:
onie-nos-install http://<server>/EdgeNOS-<ver>-<your-switch>.<bin|swi>
```

Download the file from [Releases](../../releases).

## CLI

```sh
edgenos catalog                       # the "pick your switch" index
edgenos db validate | show <plat>     # the switch database
edgenos version <plat>                # the build identity for a switch
edgenos pkg base  --from <rootfs> …   # capture a base system as a package
edgenos pkg build <spec> …            # build a component .epk
edgenos pkg install|verify|list …     # on-box package manager (epkg)
edgenos build <plat>                  # assemble that switch's ONIE installer
edgenos platform name|show|init       # resolve / bring up the board (ONL-style)
edgenos docs                          # regenerate the per-switch instruction pages
```

## Build from source

The datapath compiles in a cross-build container (no host toolchain needed):

```sh
build/build-sdk-and-edged.sh                 # AS5610: OpenMDK SDK libs + linked edged (PowerPC)
edgenos pkg build packaging/specs/as5610-52x/edged.yml --source-root .. --arch powerpc --asic bcm56846
edgenos build powerpc-accton_as5610_52x-r0 --source-root ..
```

The AS5610 `edged` is built from this tree (verified: 11/11 sources compile + link to an
18.8 MB PowerPC binary). The AS4610 `bcmd` builds via its OpenBCM recipe (`build-bcmd.sh`).

## Layout

| Dir | What |
|-----|------|
| `switchdb/` | the database — platforms, archs, asics, JSON schema |
| `core/` | shared, arch/ASIC-agnostic code (`datapath/`, `control-plane/`, `platform/`) |
| `arch/<arch>/` | per-CPU toolchain + build fragments |
| `asic/<asic>/` | per-ASIC SDK glue + chip code |
| `platform/<board>/` | per-board: drivers, config, DTS, services, platform class |
| `packaging/` | `version/` stamper, `pkgtool/` (.epk + on-box `epkg`), `specs/`, `imgbuild.py` |
| `images/` | per-platform image recipes |
| `build/` | cross-build scripts |
| `tools/`, `bin/` | DB/catalog/docs tooling + the `edgenos` CLI |
| `docs/switches/` | per-switch install pages (generated) |

## Status

Production-capable on two switches (AS5610-52X, AS4610-54T), both fully package-composed
and self-describing (each image records its component list under
`/var/lib/edgenos/epkg/installed`). The unified AS5610 `edged` is built and linked from
source. Adding a third switch is a `switchdb/` entry + a `platform/<board>/` folder.

**Arista DCS-7050SX2-72Q** (BCM56860 / Trident2+) is supported on the
`publish/arista-7050sx2-72q-td2plus` branch — a vendor switch running EdgeNOS instead
of its factory OS, forwarding IPv4 and IPv6 **in the switch chip**. Measured rather
than asserted: 1000/1000 and 997/1000 packets delivered end to end with the CPU
counter flat at its idle rate, where software forwarding would have shown ~2000.
OSPFv2 and OSPFv3 reach full adjacency on multiple ports, peering with a
Broadcom-based switch on one side and a Cisco Nexus on the other. Fan control is not
implemented. See `platform/arista-7050sx2-72q/README.md`, and `PROVENANCE.md` there
for what is deliberately not shipped and why.

## Support this project

EdgeNOS is developed on real Edgecore hardware bought out of pocket. If it saved you
time, [buy me a coffee](https://buymeacoffee.com/wrightca1).

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-FFDD00?logo=buymeacoffee&logoColor=black)](https://buymeacoffee.com/wrightca1)

## Licensing

**EdgeNOS itself is MIT licensed** (`LICENSE`). MIT rather than Apache-2.0 because the
tree contains GPL-2.0 code and Apache-2.0 is GPLv2-incompatible; not GPL because the
Broadcom SDK's terms are not GPL-compatible, so binaries linking it could not be
distributed under GPL at all.

That grant covers the code EdgeNOS Contributors wrote. It does not relicense what
EdgeNOS builds on: kernel / BDE-KNET / Buildroot / Quagga / FRR / glibc are GPL or
LGPL, and the Broadcom OpenBCM SDK, OpenMDK and PHY firmware are source-available
(distribution + derivative grant) — keep the Broadcom notices. Where an image ships
GPL binaries, distributing it carries their source obligation.

One file is a per-file exception: `platform/arista-7050sx2-72q/scdreset.c` is
GPL-2.0-only, because its SMBus master was transcribed from Arista's GPL driver rather
than reimplemented from the register map. See `LICENSING.md` for the full scope.
