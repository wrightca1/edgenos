# asic/bcm56855 — Broadcom Trident2 (BCM56855)

The chip on the Arista DCS-7050TX-64. **The SDK identifies it as `BCM56855_A2`
and drives it with the `BCM56850_A0` driver** — same Trident2 family, and the
`56850` driver is the one that binds.

## What is here

| file | what it is |
|---|---|
| `bde_shim.c` | a **user-space BDE**: PCI mapping, DMA allocation, interrupts |
| `sdkpoc.c` | the SDK agent — cold init, port bring-up, tap netdevs, FIB sync |
| `Makefile` | links both against an external OpenBCM tree |

## Two things that are unusual, and deliberate

**There is no kernel BDE module.** No `linux-kernel-bde`, no `linux-user-bde`, no
KNET. `bde_shim.c` implements the BDE interface entirely in user space over
`/sys/bus/pci/.../resource0` and a reserved DMA region (`memmap=64M$0xd0000000`),
and packet I/O reaches Linux through **tap** devices rather than KNET. That
removes the out-of-tree kernel modules that normally pin a NOS to one kernel
version — this box runs a stock 6.12.

**The OpenBCM SDK is an external dependency and is never vendored.** Point
`OPENBCM_SDK` at your own tree:

```
make STATIC=1 OPENBCM_SDK=/path/to/sdk-6.5.24
```

`STATIC=1` is not optional if the binary runs from the initrd.

⚠ **`sdkpoc.c` contains a table of PCS register writes captured from EOS**
(`SDKPOC_PCSSEQ`). It is retained as a verified-executable instrument and is
**not** what brings the link up — a control run without it reaches `link=1` just
the same. It is EOS-derived material all the same, which is why this branch
belongs on the private GitLab remote and not on the public GitHub one.
