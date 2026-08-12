# arch/<arch>/ — per-CPU-architecture support

Toolchain wiring, kernel defconfig fragments, and arch-specific build glue, keyed
by the `id` in `switchdb/arch/<arch>.yml`. Add a dir here when introducing a new CPU
(e.g. arm64, x86_64). The switch DB entry declares the triple/endianness; this dir
holds the code/config that acts on it.

| Dir | CPU | Consumed by |
|-----|-----|-------------|
| `powerpc/` | Freescale e500v2 / P2020 (AS5610-52X) | `platform/accton-as5610-52x/Makefile` includes `toolchain.mk` |
| `armhf/` | Broadcom iProc Cortex-A9 (AS4610-54) | `build/build-bcmd.sh` reads `CROSS_COMPILE` from `toolchain.mk` |

## Consuming a fragment from something that isn't make

Board Makefiles just `include` the fragment. Shell scripts can't, so each fragment
carries a `print-%` pattern rule to query a single variable:

```sh
CROSS_COMPILE=$(make -sf arch/armhf/toolchain.mk print-CROSS_COMPILE)
```

This is how `build-bcmd.sh` avoids spelling the triple out itself. A pattern rule
never becomes make's default goal, so including a fragment from a board Makefile
cannot hijack its `all` target.

Note that `build-bcmd.sh` takes **only** `CROSS_COMPILE`: `bcm.user` is compiled and
linked by the OpenBCM SDK's own makefile with its own flags, so the fragment's
`CFLAGS`/`LDFLAGS` do not apply to it. They apply to components built against the
fragment directly, the way `edged` is on PowerPC.
