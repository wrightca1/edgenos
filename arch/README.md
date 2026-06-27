# arch/<arch>/ — per-CPU-architecture support

Toolchain wiring, kernel defconfig fragments, and arch-specific build glue, keyed
by the `id` in `switchdb/arch/<arch>.yml`. Add a dir here when introducing a new CPU
(e.g. arm64, x86_64). The switch DB entry declares the triple/endianness; this dir
holds the code/config that acts on it.
