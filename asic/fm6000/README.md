# asic/fm6000 — Intel/Fulcrum FM6000 ("Alta") support

First non-Broadcom ASIC in EdgeNOS. **Dataplane not yet implemented — this is an M2 milestone.**
The mgmt-plane MVP (M0/M1) does not use anything here.

Unlike the Broadcom ASICs (OpenMDK/OpenBCM, `edged`/`bcmd`), the FM6000 is driven by the **FocalPoint**
model. There is no mainline driver; the dataplane must be built from the RE work:

- **Access:** the ASIC is not a standard PCI netdev — its registers/tables are reached via the Arista
  **SCD FPGA** (PCI `3475:0001`) 12×12 management crossbar (Phase 7b). CPU punt/inject uses `fpdma`
  with an F64/ISL header (Phase 7c).
- **Bring-up (Phase 7e):** BIST memory init (+fusebox repair) → **load microcode** (SPICO SerDes +
  parser/FFU) → MA/FFU/hash/ACL tables → AN/EPL/SerDes → forwarding.
- **Model:** programmable MAPPER + microcode/AlgoMatch FFU, GLORT logical ports, watermark MMU.

## Reference (in the arista-reverse-engineering repo)
- `notes/analysis/phase7*` — FM6000 silicon (tables, chip access, packet DMA, buffering, init).
- `notes/analysis/phase3i*` — SCD board register map.
- `reference/fm6000/` — the public Intel FM6000 datasheet.
- `reference/live-captures/7150-fm6000/` — live register/table dumps (mapper, microcode, glort, mmu,
  epl SerDes/PCS registers) — the empirical basis for the driver.

## Blocking unknowns for M2
1. FM6000 microcode blobs (`fm6000_spico_code` + parser/FFU ucode) — extract from the EOS rootfs squashfs.
2. Exact per-step register init sequences — datasheet init chapter + `libFocalpointSDK` trace.
3. Clean-room vs. reuse: `libFocalpointSDK` is closed; the datasheet + live captures allow a clean-room HAL.
