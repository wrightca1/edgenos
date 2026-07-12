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
