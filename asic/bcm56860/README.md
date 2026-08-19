# bcm56860 — Broadcom Trident2+ support

The SDK-facing layer for the BCM56860, shared by any board using that chip. Board
specifics live in `platform/<board>/`.

| file | what it does |
|---|---|
| `bde_shim.c` | user-space BDE — maps the chip's BARs through `/dev/mem` and the PCI resource files, provides a DMA pool and interrupt stubs. No vendor kernel module. |
| `sdkpoc.c` | cold initialisation and port bring-up |
| `tapbridge.c` | presents hardware ports to Linux as `tap` interfaces, so an ordinary routing daemon can run over them |
| `l3sync.c` | mirrors the Linux FIB into the chip's route and host tables, and installs the field-processor rules that keep the control plane alive once the chip is routing |

## Two things worth knowing before reading the code

**The chip will happily route the routing protocol away.** Once `MY_STATION` tells
the ASIC "frames to this MAC are mine to route", the OSPF packets that build the
routes get routed too, and the adjacency dies at `ExStart`. Vendor software
solves this with ingress field-processor rules that copy protocol traffic to the
CPU, not with host-table entries. `l3sync.c` reproduces that; without it the L3
tables install cleanly and the box still cannot route.

**Interrupts are not connected.** `soc_attach()` refuses to proceed without an
interrupt line unless `polled_irq_mode=1` is set, after which the SDK runs its
own polling thread. The generated `config.bcm` sets it.

## Building

Requires an OpenBCM SDK tree, which is **not** redistributed here. Point
`sdk-defines.mk` at your own and `make STATIC=1 sdkpoc`.
