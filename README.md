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

# compute the version identity for a platform
./bin/edgenos version powerpc-accton_as5610_52x-r0 --print
```

## Status

Phase 1 (foundation) is in place: the switch DB, its schema, the two production
platforms (AS5610-52X, AS4610-54T), the version stamper, and the CLI. The package
builder (`.epk`) and image recipe are the next phases — see the roadmap in `DESIGN.md`.

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
