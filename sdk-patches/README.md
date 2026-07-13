# SDK patches (OpenBCM sdk-6.5.16)

The BCM SDK (`OpenBCM/sdk-6.5.16`) is an external, non-git tree that the
`build/build-bcmd-5610.sh` oracle build compiles. These patches are the
EdgeNOS-specific modifications applied to that tree. Apply them against a
pristine SDK before building `bcm.user`:

```sh
cd /home/smiley/edgecore/OpenBCM/sdk-6.5.16
patch -p1 < /path/to/edgenos/sdk-patches/0001-cmic_pcie_cdr_bw_adj-skip.patch
```

## 0001-cmic_pcie_cdr_bw_adj-skip.patch
Early-return out of `cmic_pcie_cdr_bw_adj()` (`src/soc/esw/drv.c`). That routine
is an optional PCIe-SerDes CDR jitter tune that MIIMs the PCIe PHY (id=0xfa);
on the AS5610 with `soc_skip_reset=1` that MIIM times out and aborts `soc_init`.
Skipping it lets `soc_init` proceed past the MIIM (the PCIe link is already up
via edged's kernel BDE) and makes SDK reset attempts non-fatal/recoverable. This
is the one SDK patch worth keeping; see `docs/acl-5610-double-wide-fp.md` and the
`project_acl_soc_init_pathA_deadend` memory for the full IFP-bring-up story.

## 0002-soc_reset_bcm56840_a0-skip-when-skip_reset.patch (WIP — hybrid bring-up)
Early-return out of `soc_reset_bcm56840_a0()` when `soc_skip_reset` is set. Part of
the **hybrid** IFP bring-up (feature/5610-bde-mmap-intr): edged's bmd_init brings the
chip fully up (LCPLLs locked, SBUS/blocks out of reset, SCHAN working — and it STAYS
live/SCHAN-readable after edged exits), then the SDK runs `soc_init` on the live chip
with all reset skipped so it doesn't unlock the PLLs / rewrite the SBUS ring-map (which
kills the working SCHAN). PROVEN: with this patch `soc_init` gets past `soc_reset`
preserving edged's SCHAN. REMAINING BLOCKER: the per-port XLPORT serdes init (fatal
`XLPORT_XGXS_CTRL_REG` reads) runs BEFORE `misc_init` and times out on the port blocks
edged didn't bring up (edged links only swp1/swp2). Next: skip/tolerate the whole port
init (it's irrelevant to the FP engine) to reach `misc_init`/`bcm_field_init`. Not yet
end-to-end; see memory `project_acl_soc_init_pathA_deadend`.

## 0003-serdes-reset-skip-when-skip_reset.patch (WIP — hybrid)
Early-return from the 5 per-port serdes-reset helpers in `src/soc/common/drv.c`
(`soc_xgxs_reset`, `soc_xgxs_in_reset`, `soc_wc_xgxs_reset`, `soc_wc_xgxs_in_reset`,
`soc_wc_xgxs_power_down`) when `soc_skip_reset` is set. These do fatal
`XLPORT_XGXS_CTRL_REG` reads on port blocks edged didn't bring up (edged links only
swp1/swp2) → SchanTimeOut. Ports are irrelevant to the FP engine. **With 0002+0003,
`init soc` COMPLETES cleanly on edged's live chip (no SchanTimeOut) — the reset+port
wall that blocked every prior approach is broken.**

## 0004-trident-misc-memclear-skip.patch (WIP — hybrid)
Three skips in `_soc_trident_misc_init` / helpers (`src/soc/esw/trident.c`) when
`soc_skip_reset` is set — edged already initialized these: `_soc_trident_clear_all_memory`,
the `FP_GLOBAL_MASK_TCAM` `soc_mem_clear`, and `_soc_trident_port_mapping_init`. Each was a
misc_init op that timed out on the live chip.

## Hybrid status / remaining wall (2026-07-12)
With 0002+0003, **`init soc` completes cleanly** on edged's live chip. `init all` gets deep
into `misc_init` but hits two BDE-level limits that patches can't paper over:
1. **MMU/EPIPE register SCHAN writes time out** (e.g. addr 0x0c380001 MMU block, 0x0e170000
   EPIPE) — the SDK's internal SBUS ring map (from the skipped `soc_reset_bcm56840_a0`)
   doesn't match the chip's live ring map, so SCHAN ops to those blocks route wrong.
2. **Memory clears use TableDMA** (`_soc_xgs3_mem_dma`, e.g. FP_GLOBAL_MASK_TCAM) which
   times out — the polled custom BDE doesn't signal DMA completion.
Tolerating SCHAN timeouts globally CORRUPTS SCHAN ("invalid S-Channel reply") — reverted.
=> To finish the hybrid, the BDE needs real TableDMA completion + the SBUS ring map must be
reconciled (set CMIC_SBUS_RING_MAP to the SDK's expectation without the disruptive full
reset, or align edged's map). This is the genuine Option-2 BDE work, now precisely scoped.
See memory `project_acl_soc_init_pathA_deadend`.

## Hybrid progress update (2026-07-12, session 2)
Config that advances furthest (stop edged first, then bcm.user):
`os=unix phy_null=1 *_intr_enable=0 polled_irq_mode=1 soc_skip_reset=1
 table_dma_enable=0 tslam_dma_enable=0 mem_cache_enable=0`
- **SBUS timeout (0002 now writes CMIC_SBUS_TIMEOUT=0x13500)**: edged leaves it at
  0x7d0 which is too low for MMU/EPIPE SCHAN ops. Verified it holds through init.
- **table_dma_enable=0 + tslam_dma_enable=0**: memory ops fall back to SCHAN PIO,
  eliminating the `_soc_xgs3_mem_dma` TableDmaTimeOut on the polled BDE.
- **mem_cache_enable=0**: skips the FP_GLOBAL_MASK_TCAM SW-cache full-memory read.
- Together these took misc_init from 111 SchanTimeouts + "invalid S-Channel reply"
  (SCHAN desync) down to ~6, desync GONE.
REMAINING WALL: misc_init's MMU/EPIPE register ops (addr 0x0c380001 MMU, 0x0e170000
EPIPE) time out **non-deterministically** — same op passes one run, times out the
next, even with CMIC_SBUS_TIMEOUT confirmed at 0x13500. This is marginal SCHAN
reliability to blocks edged only partially brought up (the full reset that fully
inits them is exactly what we skip). Fixing it likely needs either bringing those
blocks fully out of reset without disturbing edged's SCHAN, or a BDE-level SCHAN
completion/retry that tolerates the marginal timing. See memory
`project_acl_soc_init_pathA_deadend`.
