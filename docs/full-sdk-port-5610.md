# Full OpenBCM SDK port for the AS5610 (PowerPC / BCM56840, Trident+)

**Goal:** run the full OpenBCM SDK on the 5610 so `soc_init`/`bcm_init` do the *correct* IFP
bring-up (which raw register replication can't reproduce — see `docs/acl-design.md` §9 and
`project_acl_phase1` memory), giving working `bcm_field` ACLs like the 4610 already has.

**Model (proven on the 4610):** `bcmd` is the SDK's `systems/linux/user/common/socdiag.c` with
`diag_shell();` → `bcmd_run();`, so the SDK's own init runs before our datapath. Recipe:
`edgenos/build/build-bcmd.sh`. The 5610 mirrors this with PPC/56840 deltas.

## Deltas vs the 4610
| | AS4610 (works) | AS5610 (this port) |
|---|---|---|
| Chip | BCM56340 (Helix4) | **BCM56840_A0** (Trident+) |
| Arch | ARM / iProc | **PowerPC** |
| SDK platform | `iproc-4_4` (builds `bcm.user`) | **needs a custom user platform** |
| CMIC | on-die | PCIe / PAXB |
| BDE | SDK KNET (kernel) | **5610's custom user-mode BDE** (mmap /dev/mem BAR0 + PAXB) |

## Progress (this session)
- **Feasibility CONFIRMED.** `BCM_56840_A0 = 1` in `make/Make.config` (+ `make/local/esw/Make.pkg.56840`);
  Trident+ field driver at `src/bcm/esw/trident/field.c`; PPC toolchain `powerpc-linux-gnu-gcc` works.
- **✅ SDK BUILDS + LINKS for PPC/56840 (milestones 1+2 DONE).** `build/build-sdk-5610.sh` produces
  **159 libs** — incl. `libbcm.a` (the `bcm_field` API) and `libsoc.a` (`soc_init` = the correct FP
  init) — and links a **137 MB static `bcm.user`** (ELF 32-bit MSB PowerPC). Verified with `file`.
- **How** (the gates cleared, all in the build script):
  - Build **`user_libs`** directly in `systems/linux/user/common` (the `gto` platform doesn't need a
    custom dir after all — just don't build its kernel modules; `user_libs` skips them).
  - `ALLOWED_MAKE_VERSIONS` += 4.3 (SDK rejects make ≥4.2).
  - `Makefile.unix-user`: `-Werror`→`-Wno-error` (gcc-10 warns more than the SDK's era).
  - Add `-I systems/bde/linux/include` (the user build omits the BDE headers).
  - Link: drop `-lnsl` (in modern libc) + `-Wl,-z,muldefs` (gcc-10 `-fno-common` collides the
    *unused* chip globals trident3/helix5; our chip is `trident.o`, so first-def is harmless).
  - Runs in a `sdk5610build:1` docker image (debian:bullseye + PPC toolchain). Incremental — in a
    time-capped sandbox, re-run until done; on a normal host it's one ~40-min shot.
- Binary: `OpenBCM/sdk-6.5.16/build/linux/user/common/bcm.user`.

## Next milestones
1. ✅ ~~Custom 5610 user platform~~ — not needed; `user_libs` in `common` (gto platform) works.
2. ✅ ~~Build the SDK user libs + `bcm.user`~~ — DONE (159 libs + PPC `bcm.user`).
3. **BDE integration** — plug the 5610's existing user-mode BDE (mmap /dev/mem BAR0 + PAXB sub-window,
   see `asic/bcm56846/bde_interface.c`) into the SDK's `bde_create`/`bde_t` interface, so the SDK
   reaches the chip over PCIe. (The 5610 has no KNET; this replaces the SDK's kernel BDE.)
4. **Attach + `soc_init`/`bcm_init`** on the box (serial console for recovery; ONIE-reimageable) —
   verify the IFP is live (a `bcm_field` match-any counter increments).
5. **Datapath port** — re-implement the 5610 datapath (L3 v4/v6, ECMP, VLAN, DMA, OSPF punt) on the
   `bcm_*` API (like `bcmd` on the 4610), or run SDK-init + keep edged's datapath if the BDE can be
   shared. Then `bcm_field` ACLs.

## Notes
- Box is a **test switch with serial console**; worst case reimage from the ONIE installer
  (`EdgeNOS-0.2.1-powerpc-...bin`; config backup at `/home/smiley/5610-restore-20260702/`).
- The 4610 SDK libs live prebuilt at `edgecore-4610-54t/live-investigation/sdk-ref/OpenBCM/.../build/`
  — reference for what a completed PPC build should produce.
