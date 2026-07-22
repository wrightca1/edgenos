# asic/fm6000 — Intel/Fulcrum FM6000 ("Alta") support

First non-Broadcom ASIC in EdgeNOS, and the first **clean-room** datapath (no vendor SDK linked — cf.
`asic/bcm56846` which links OpenMDK). This is the **M2** milestone; the mgmt-plane MVP (M0/M1) uses nothing
here. The code is a skeleton driven entirely by the reverse-engineering writeups in the arista RE repo —
every constant cites its source.

## Access model (corrected)
The FM6000 is a **direct PCIe endpoint** — Intel `8086:155b` (= Fulcrum `1823:1770`) at `02:00.0`, with its
own BAR0. It is **not** hidden behind the SCD FPGA (`3475:0001`, which is separate board control). So:

- **Switch CSRs**: userspace `mmap` of the FM6000's `resource0`. Word-addressed —
  `*(u32*)(bar0 + (word_idx << 2))`. On x86_64 no MMIO ordering barriers are needed (the PPC Broadcom path
  needed a kernel BDE for `eieio`/`sync`; we don't).
- **Packet DMA**: the on-chip engine at `BAR0 + 0x5000`, dual 32-byte-descriptor rings. Needs kernel/VFIO
  backing for coherent low-4GiB memory + MSI (abstracted behind `struct fpdma_backing`).
- CPU punt/inject frames carry the **F64/ISL** tag (DGLORT/SGLORT/FTYPE/SWPRI/VLAN at L2 offset 12).

## Files
| File | Role | Provenance |
|---|---|---|
| `fm6000_regs.h` | Block bases + recovered register/DMA offsets, descriptor geometry | phase7g, FPDMA.md |
| `fm6000_hw.{c,h}` | PCIe bind (sysfs walk + BAR0 mmap), CSR read/write/poll, delay | phase7g §a, FPDMA.md alta_probe |
| `fm6000_ucode.{c,h}` | Microcode load: parser/FFU **text CSR replay** + SerDes **SPICO SBus** upload | phase7g §c/d |
| `fm6000_boot.{c,h}` | `fm6000BootSwitch` ordering + BIST/memory-init skeleton | phase7g §b |
| `fpdma.{c,h}` | Packet-DMA ring engine (0x5000 block, TX/RX rings, punt/inject) | FPDMA.md |
| `kmod/fm6000dma.c` | **Clean-room DMA/MSI kernel module** (default backing) — BAR0 + coherent low-4GiB pool + MSI | phase13 |
| `fpdma_kmod.{c,h}` | Userspace side of the kmod (mmap BAR0 + pool, MSI fd) — same seam as VFIO | phase13 |
| `fpdma_vfio.{c,h}` | Alt DMA backing via **VFIO** (IOMMU-capable boxes only) | — |
| `fm6000_bringup.c` | Standalone end-to-end bring-up/punt diagnostic (`make fm6000_bringup`) | — |
| `fm6000.mk` | Build fragment (no vendor SDK) | — |

## DMA backing — kmod (default here) or VFIO (portable alt)
`fpdma` needs coherent low-4GiB memory + MSI. `fm6000_edged` tries two backings, in order:

1. **kmod (`kmod/fm6000dma.ko`) — the default, required on the 7150.** The 7150's AMD RS780 has **no usable
   IOMMU** (0 iommu_groups, GART fallback — see phase13 live probe), so VFIO can't work. This tiny GPL module
   binds the FM6000 (`8086:155b`), exposes BAR0 + a `dma_alloc_coherent` pool (`pool_mb`, default 4) pinned
   below 4 GiB (32-bit master) + an MSI eventfd, all via `/dev/fm6000dma`. `fpdma_kmod.c` mmaps them.
   Build: `make -C kmod KDIR=<kernel>`; load: `insmod fm6000dma.ko`. All packet-ring logic stays in `fpdma.c`.
2. **VFIO (`fpdma_vfio`) — only on IOMMU-capable boxes.** `intel_iommu=on`, bind `vfio-pci`. Kept for
   portability; unused on the 7150.

Either way `fpdma.c` + the rings are identical — only the `struct fpdma_backing` differs. The whole set links
into `fm6000_bringup` / `edged-7150` and runs `backend → boot_switch → fpdma_init → inject/punt`.

## Clean-room boundary (important)
The **procedures** are reimplemented from behavioral RE. The **payloads** are Arista/Intel proprietary and
are **NOT** vendored: the parser/FFU microcode (`fm6000Microcode.raw`) and the SerDes SPICO blob (12000 B,
embedded in `libFocalpointSDK.so`) are staged on the box by the operator (extracted from the running EOS
image) and loaded by path at runtime (`FM6000_FW_*` in `fm6000_ucode.h`). Nor is the proprietary 3508-macro
Intel register header (`fm6000_api_regs_int`) copied in — only the offsets we independently recovered/cited.

## What still needs one live register trace (marked `TODO(live-trace)` in code)
Recovered to *structure*, but a few **runtime-computed values** aren't constant-foldable from disassembly —
one trace on the powered box pins them (see arista `edgenos/GAPS.md` §A):
1. BIST MARCH/fusebox descriptor **data values** (`fm6000_boot.c`).
2. SBus-controller **framing** at `0xB0500` — the one stubbed primitive (`fm6000_sbus_write`).
3. DMA **COMMAND/DMA_CFG** enable bits + descriptor status/OWN bit semantics (`fpdma.c`).
4. SPICO CRC-verify **interrupt** command encoding (`fm6000_ucode.c`).

## Reference (arista RE repo)
- `notes/analysis/phase7g-fm6000-bringup-recovered.md` — init/microcode/BIST (primary source for this dir).
- `edgenos/FPDMA.md`, `edgenos/SCD.md`, `edgenos/GAPS.md` — DMA engine, board control, gap tracker.
- `reference/fm6000/` datasheet; `reference/live-captures/7150-fm6000/` live dumps.
