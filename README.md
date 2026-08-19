# EdgeNOS

> ### This branch: Arista DCS-7050TX-64 (BCM56855 / Trident2)
>
> A **third switch**, added to `platform/arista-7050tx-64/` and
> `asic/bcm56855/`. It routes — 40G uplink, OSPF adjacency Full, 35 routes, and
> a hardware FIB whose adds, next-hop changes and withdrawals are each verified
> against the chip — on a **stock 6.12 kernel with no out-of-tree kernel
> modules**, because the BDE and packet path were rewritten in user space.
>
> **Status `bringup`, not production.** Copper bring-up is unreliable, the SDK
> agent and routing daemons are started by hand, and there has been no cold-boot
> test. Everything that does not work is listed in the platform README.
>
> ⚠ **Read [`platform/arista-7050tx-64/PROVENANCE.md`](platform/arista-7050tx-64/PROVENANCE.md)
> before reusing this.** The board vendor's own data — port map, cooling curve,
> retimer tuning — is deliberately **not** shipped here; it is generated on the
> switch itself. Building this without those files gives you a switch whose fans
> run at 100% and whose SDK will not attach, and the platform README says how to
> produce them.
>
> Everything below describes EdgeNOS generally and applies to all three switches.


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
arista 7050tx-64       x86_64   bcm56855   6.12    bringup     (source only -- see notes)
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

**A third switch is being added on this branch: the Arista DCS-7050TX-64**
(x86_64, BCM56855 / Trident2) — `platform/arista-7050tx-64/`. It routes: 40G
uplink up, OSPF adjacency Full, and a hardware FIB whose adds, next-hop changes
and withdrawals are each verified against the chip rather than inferred.

It is **`bringup`, not production**, and differs from the two production boards
in three ways worth knowing before reusing anything from it:

* **No kernel BDE and no KNET.** A user-space BDE (`asic/bcm56855/bde_shim.c`)
  drives the chip over sysfs PCI resources and a reserved DMA region, with tap
  netdevs for packet I/O. That is what lets it run a stock 6.12 kernel with
  nothing out-of-tree — and it is why this board has no `linux-kernel-bde`.
* **Targets `sdkpoc`, not `edged`.** `sdkpoc` is the bring-up agent: cold init,
  port bring-up, tap datapath and FIB sync in one binary, written while the
  chip's behaviour was still being learned. Converging it onto `core/datapath`
  is the next structural step.
* **The image ships none of the board vendor's data.** `config.bcm`, the cooling
  curve and the retimer tuning are generated on the switch itself from what is
  already on it. Read
  [`platform/arista-7050tx-64/PROVENANCE.md`](platform/arista-7050tx-64/PROVENANCE.md)
  before reusing any of this, and the platform README for what does not work yet
  — copper bring-up is unreliable, and there has been no cold-boot test.

## Licensing

All components are distributable. Kernel / BDE-KNET / Buildroot / Quagga are GPL; the
Broadcom OpenBCM SDK, OpenMDK, and PHY firmware are source-available (distribution +
derivative grant). Keep the Broadcom notices; the result is source-available, not pure
OSI. See the per-component licenses.
