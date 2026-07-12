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

## 0004-trident-misc-memclear-skip.patch (WIP — hybrid, not sufficient yet)
Early-return from `_soc_trident_clear_all_memory()` (`src/soc/esw/trident.c`) when
`soc_skip_reset` is set — edged already initialized the memories and this HW memory-init
polls a done bit that times out on our polled BDE (and would wipe edged's live tables).
Gets `init all` past clear_all_memory, but misc_init still has further memory HW-init
SchanTimeouts after it ("Misc init failed"). REMAINING WALL: misc_init's memory init
needs DMA/SLAM completion signaling the polled BDE lacks — this is where BDE DMA/interrupt
support (Option 2) genuinely matters. See memory `project_acl_soc_init_pathA_deadend`.
