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

## Why this is now THE path (2026-07-17) — static replication EXHAUSTIVELY ruled out
The IFP-arming wall is no longer a theory; it was proven by elimination on live hardware this
session (see `docs/acl-programming-model.md` §4 and `memory/project_fp_gm_fields_decisive_bug`):
- Fixed every SDK-verified static bug: `FP_GM_FIELDS` KEY (matches trx/field.c:2107), the
  `FP_GLOBAL_MASK_TCAM` X/Y-pipe writes (Trident writes X/Y separately, not the combined view),
  and the `TCP_FN`/`TTL_FN`/`TOS_FN` identity key-gen tables (`_bcm_field_trx_tcp_ttl_tos_init`).
- Opened the port gate fully (match-any `IPBM_MASK=0`).
- Diffed EVERY FP/IFP register against the working-Cumulus capture (edged's built-in FPREG diff):
  all FP-lookup registers match; the only 3 gaps (`ING_CONFIG_2`, `ING_CONFIG_64` `ARP_VALIDATION_EN`,
  `VFP_KEY_CONTROL`) are ARP/VFP/general-ingress — non-IFP — and closing them changed nothing.
- Test rig: test-system eth1 → **swp5** (cabled), fabricated unicast floods, link tcpdump-confirmed,
  entry correct (`G_DROP=1`, `COUNTER_MODE=7`). **FP_COUNTER stayed 0 in every configuration.**
Conclusion: no static register/memory value arms the IFP lookup; it needs the SDK's dynamic
`field_init`/group-create machinery. Hence the full SDK (which runs that machinery in `soc_init`/
`bcm_field_init`) is the only path to native FP ACLs + CoPP. All the static fixes above are
correct and stay in edged (they're prerequisites the SDK also does).

## Recommended architecture: mirror the 4610 `bcmd` (Option A — full SDK owns the chip)
Running the SDK's `soc_init` RE-INITIALIZES the chip (reset, DMA, ports, L3), so it cannot coexist
with edged's hand-built datapath — one owns the chip. The clean, proven model is the 4610's `bcmd`
(full SDK + datapath on the `bcm_*` API). Do the same on the 5610:
- **M3 — BDE integration (the gating step).** Wrap the 5610's user-mode BDE
  (`asic/bcm56846/bde_interface.c`: mmap /dev/mem BAR0 + PAXB sub-window) behind the SDK's
  `bde_t`/`bde_create` so `soc_probe`/`soc_init` reach the chip over PCIe. No KNET. This is the
  one genuinely new piece vs the 4610 (which used the SDK kernel BDE); everything else follows the
  4610 recipe. Biggest risk item — budget the most time here.
- **M4 — Attach + `soc_init`/`bcm_init`, prove the IFP.** Boot `bcm.user` on the box (serial for
  recovery), run init, create a match-any `bcm_field` group + entry, flood via swp5, confirm the
  counter increments. This is the go/no-go: it proves the SDK arms what static replication couldn't.
- **M5 — Datapath on `bcm_*`.** Re-home the 5610 datapath (L3 v4/v6, ECMP, VLAN, DMA/punt, OSPF,
  swp taps) onto the SDK API, following `bcmd` on the 4610. Then `bcm_field` for real ACLs/CoPP.
  Largest chunk, but mechanical given the 4610 template + edged's existing logic as the spec.

Effort read: M3 is the unknown (days — BDE shim + first successful `soc_probe`); M4 is a short
validation once M3 lands; M5 is weeks but low-risk (port existing, working logic to a known API).
The decisive checkpoint is **M4** — do M3+M4 first and STOP to confirm the IFP fires before
committing to the M5 datapath rewrite.

## M3 DONE + VALIDATED, M4 STARTED (2026-07-17)
Ran the prebuilt `bcm.user` (137MB, deployed to `/var/bcm.user`) with `BCM5610_BDE=1` on the box:
```
bcm5610_bde: BCMb846 rev 02, BAR0 phys 0xa0000000 (262144 bytes)
bcm5610_bde: DMA 4 MB at phys 0x2800000
PCI unit 0: Dev 0xb846, Chip BCM56846_A1, Driver BCM56840_B0
SOC unit 0 attached to PCI device BCM56846_A1
```
**M3 PROVEN:** the custom `bcm5610_bde.c` adapter (ioctl → `/dev/linux-kernel-bde`, base_address=0
so CMREAD/CMWRITE fall through our PAXB sub-window) lets the real SDK read the chip ID and attach.
The `_PPC_IOC(d,..)` encoding is correct (d∈{1,2,3}<<30 == PPC dir{2,4,6}<<29 — matches the kernel
module). config.bcm found (via `/etc/edged/config.bcm`, has `xgxs_lcpll_xtal_refclk=1`).

**M4 blocker:** `init soc; init bcm` fails in `soc_reset_bcm56840_a0`:
`LCPLL 0/1/2/3 not locked` → then `soc_schan_op operation timed out` → `bcm_init failed`. Register
reads work (LCPLL status readable), but the LCPLLs won't (re-)lock and SCHAN then times out. NOT a
config gap (refclk is set). Most likely cause: running the SDK's `soc_reset` on a chip edged had
ALREADY initialized (PLLs already up / CMIC DMA state), not a cold chip. **Next step: cold-boot the
box, hold edged off (it auto-starts + inits the chip), and run `bcm.user init` on a VIRGIN chip** —
if soc_reset locks the LCPLLs from cold, SCHAN should come up and bcm_init completes → then the M4
go/no-go: `fp` create a match-any group+entry, flood swp5, watch the counter. If LCPLL still won't
lock from cold, debug the reset sequence / CMIC access in the kernel BDE. bcm.user @ /var/bcm.user;
run dir /var/sdk (config.bcm staged); serial screen `serialcap` on 10.22.1.56 for recovery.

---

## FINAL STATUS — 2026-07-19 (supersedes the M4-blocker note above)

The M4 LCPLL/SCHAN blocker is **SOLVED**. The full Broadcom SDK now attaches, runs `soc_init` +
`bcm_init`, and programs a configurable dst-IP ACL into the BCM56846 IFP **TCAM — verified in silicon**.
The remaining gap is the IFP *lookup arming* (below), not the SDK bring-up.

### What works (silicon-verified)
- **S-Channel**: the 56846 is a **CMICe** chip (engine at `CMIC_SCHAN_CTRL=0x50`), not CMICm. Five fixes
  (see `patches/sdk-6.5.16-bcm56846-fixes.patch`) made SCHAN complete:
  1. `feature.c` `soc_features_bcm56840_b0`: do NOT force cmicm/new_sbus_format for 0xb846; DO enable
     `soc_feature_schmsg_alias` (chip uses the 0x800 msg-buffer, `CDK_XGS_CHIP_FLAG_SCHAN_EXT`).
  2. `schan.c` `soc_schan_init`: removed the `||0xb846` cmicm force → uses `soc_cmice_schan_init`.
  3. `drv.c` `soc_endian_config`: force `CMIC_ENDIAN_SELECT=0x04000004` (ES_BIG_ENDIAN_DMA_OTHER, PIO
     endian OFF) for 0xb846 — matches edged; the default `0x05000005` byte-swapped the msg buffer.
  4. `schan.c` `soc_schan_header_cmd_set`: force `src_blk=0` for 0xb846 (chip has `SCHAN_SB0`; reply
     routes to src_blk, and CMIC_BLOCK=5 sent the ACK to the wrong block).
  5. (feature.c) schmsg_alias again = the 0x800 buffer alias.
  Plus kept: `soc_skip_reset` LCPLL/SBUS-timeout path (`esw/drv.c`), the CMICe reg graceful-skips
  (`reg.c`), and `trident.c` skip_reset hybrid guards. Custom BDE = `asic/bcm56846/sdk_bde/bcm5610_bde.c`.
- `soc_init` completes (411,530 SchanOps, tables cleared via SCHAN), `bcm_init` completes, `bcm_field`
  creates + installs a dst-IP DROP entry. `dump FP_TCAM` shows `VALID=3, F2=<dstIP>, MASK, KEY` correct.

### Required config.bcm knobs (first-wins parser → edit in place, don't append duplicates)
`soc_skip_reset=1`, `polled_irq_mode=1`, `schan_intr_enable=0`, `tdma_intr_enable=0`,
`tslam_intr_enable=0`, `phy_null=1`, `bcm_linkscan_interval=0`, `parity_enable=0`, `mem_scan_enable=0`,
`mem_clear_hw_acceleration=0` (CRITICAL: forces SCHAN table-clears, DMA clears silently fail here),
`skip_ipmc_init=1` (ipmc_init hits "Table full"; bcm_init rolls back the whole unit on any module error).
Run: `BCM5610_BDE=1 /var/bcm.user`, then `init soc; init bcm`.

### OPEN BLOCKER — IFP lookup does not evaluate live traffic
The ACL is in the TCAM but never matches a packet (verified: 2000 injected IPv4 pkts to a drop-ACL'd
dst FLOOD through the chip, `RPKT.xe4 +2000` / `TDBGC3.xe4x +2000`, FP stat stays 0). Exhaustively ruled
out: `IFP_BYPASS_ENABLE=0` (not bypassed), `FP_SLICE_MAP` (slice0→phys0 mapped), `FP_PORT_FIELD_SEL`
(fpf2=0 is the CORRECT registered selcode for a DstIp-only group — trident/field.c:526), entry
`VALID=3`, `init misc`, and edged's arming regs (`ING_CONFIG_2=0x1ff`, `VFP_KEY_CONTROL=0x3`). Every FP
memory/register the SDK programs is correct, yet the lookup doesn't fire — the project's long-standing
"FP un-armable" wall, now proven NOT to be the TCAM/selcodes/slice-map/enable.
**Next step (recommended):** capture-and-diff vs live Cumulus (drops correctly on this exact silicon).
Install a dst-IP drop ACL on Cumulus, confirm it drops, dump the ingress-pipeline ENABLE/config register
block (`ING_BYPASS_CTRL`, `ING_CONFIG*`, `AUX_ARB_CONTROL`, FP/aux arbiter + lookup-enable), and diff
against the SDK state — the differing register(s) are the missing arming.
**Production note:** configurable dst-IP ACLs already SHIP via the L3 `DST_DISCARD` hybrid; the SDK-IFP
path is a hardware-offload upgrade, currently blocked only on this arming step.

### Test harness (staged on box .238 / test-system 10.22.1.56)
swp5 = xe4 = logical port 5. Inject: `sudo ip neigh replace 10.101.105.1 lladdr 80:a2:35:81:ca:b3 nud
permanent dev eth1; sudo ping -f -c N 10.101.101.5` (routes via 5610). FP stat = the observable.
