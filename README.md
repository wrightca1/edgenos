# EdgeNOS

Unified, multi-architecture / multi-ASIC network OS build system. A **switch
database** drives everything: each board is a data entry that resolves to a CPU
arch, a switch ASIC, and a bill-of-materials of packages.

See [`DESIGN.md`](DESIGN.md) for the full architecture.

## Quick start

```sh
# list known switches
./bin/edgenos db list

# validate the switch database (schema + referential integrity)
./bin/edgenos db validate

# resolved view of one platform (platform + arch + asic merged)
./bin/edgenos db show arm-accton-as4610-54-r0

# pick your switch — the downloadable catalog
./bin/edgenos catalog

# build a downloadable ONIE installer (base system for the arch+asic + packages)
./bin/edgenos pkg base --from ../newnos/output/images/rootfs.sqsh \
    --platform powerpc-accton_as5610_52x-r0
./bin/edgenos build powerpc-accton_as5610_52x-r0 --source-root /home/smiley/edgecore
#   -> output/images/EdgeNOS-0.1.0-powerpc-accton_as5610_52x-r0.bin
```

## Status

Phases 1–3 are in place: the switch DB + schema + version stamper (P1), the `.epk`
package system — host builder + on-box `epkg` with an arch/ASIC install guard (P2),
and the image recipe — `pkg base` + `build` producing per-platform ONIE installers
(`.bin` for the 5610, `.swi` for the 4610), each carrying a base system for its
arch+ASIC plus component packages, stamped and self-describing (P3). Next is Phase 4:
packaging the remaining components and migrating the two forks onto `core/`+`platform/`.
See `DESIGN.md`.

## Layout

| Dir | What |
|-----|------|
| `switchdb/` | the database — platforms, archs, asics, schema |
| `core/` | shared arch/ASIC-agnostic code (datapath framework, control plane) |
| `arch/`, `asic/`, `platform/` | per-axis support (plugins selected by the DB) |
| `packaging/` | `version/` stamper, `pkgtool/` package system |
| `images/` | per-platform image recipes |
| `tools/`, `bin/` | DB tooling + the `edgenos` CLI |

## Requirements

Python 3 with `pyyaml` and `jsonschema` (for DB validation).
