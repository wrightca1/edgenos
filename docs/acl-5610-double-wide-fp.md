# AS5610 (BCM56846 / Trident+) IFP double-wide FP programming — from the OpenBCM SDK source

Reverse-engineered from the **OpenBCM SDK source** (`OpenBCM/sdk-6.5.16/`), the
authoritative reference implementation for our chip. The file
`src/bcm/esw/trident/field.c` is literally titled *"BCM56840 Field Processor
installation functions"* — BCM56840/56846 is the exact device.

This documents how the SDK writes a **double-wide (paired-slice) FP_TCAM entry**, so
`edged`'s hand-rolled `asic/bcm56846/acl.c` can replicate it without running the SDK.

## Public-code availability (what documents this, what doesn't)

| Source | Have it? | Useful for double-wide FP on Trident+? |
|---|---|---|
| **OpenBCM SDK** `src/bcm/esw/{trident,triumph,trx}/field.c` | yes (`OpenBCM/sdk-6.5.16`) | **YES — authoritative.** `trident/field.c` = "BCM56840 FP installation functions". |
| OpenBCM `src/bcm/esw/trident2/field.c` | yes | Partial. TD2 folds the port bitmap into the main key and uses a different writer (`_bcm_field_td2_qual_tcam_key_mask_set`); it **does NOT show the `FP_GLOBAL_MASK_TCAM` / `FP_GM_FIELDS` writes our chip needs.** Do not copy TD2 for 56846. |
| OpenMDK CDK | yes | Field/memory **layout only** (`bcm56840_a0_defs.h` macros). No install logic — that's what we're reconstructing. |
| OpenNSL | `~/Downloads/OpenNSL-master.zip` | High-level API wrapper over the same SDK; no lower-level detail than OpenBCM source. |
| SONiC **SAI** | not local | SAI is a vendor-abstraction; the Broadcom SAI impl calls the closed `libsai`/SDK. It would show ACL *table/entry* API use, **not** raw FP_TCAM/slice programming. Not helpful here. |

**Conclusion:** the OpenBCM `trident`/`triumph`/`trx` `field.c` is the only public source
that documents the exact register/memory sequence; everything below is cited from it.

## Call chain (who writes what)

- Dispatch (Trident): `trident/field.c:2622-2623`
  `fp_tcam_policy_install = _bcm_field_tr_entry_install` (defined in `triumph/field.c:3057`).
- Multi-part orchestrator: `_field_tcam_policy_install` (`field_common.c:29758`); install
  loop `:30138-30178`, iterating `idx = parts_count-1 .. 0`.
- Per-physical-slice writer: `_bcm_field_tr_entry_install` (`triumph/field.c:3057`) —
  erases TCAM line (`:3106`), writes POLICY (`:3239`), writes `FP_GLOBAL_MASK_TCAM`
  (`:3303-3350`), writes `FP_GM_FIELDS` for part 1 (`:3363-3380`), writes TCAM key (`:3353`).
- Key/VALID encoder: `_bcm_field_trx_tcam_get` (`trx/field.c:2482`, Trident branch `:2513`).

## Double-wide = TWO full FP_TCAM entries, one per physical slice

- `_bcm_field_entry_tcam_parts_count` (`field_common.c:5990`): a
  `_FP_GROUP_SPAN_DOUBLE_SLICE` group has **`parts_count = 2`** (`:6014-6019`).
- The loop installs `f_ent[0]` (primary slice) and `f_ent[1]` (secondary slice) as
  **separate entries, each with its own TCAM index** (`:30170`).
- Index: `_bcm_field_entry_tcam_idx_get` (`field_common.c:35056`):
  `tcam_idx = fs->start_tcam_idx + slice_idx`, with a **different `fs` (physical slice)**
  per part. When the two physical slices are adjacent (base = slice*256), the secondary
  is `primary + 256`. Our slices (6+7, or Cumulus 8+9) are adjacent → **secondary = primary + 256**.
- **VALID:** each half gets `VALID = 3` when the group is lookup-enabled
  (`trx/field.c:2510`, `:2532`), i.e. **both halves are VALID=3**. There is no separate
  "wide valid" bit inside FP_TCAM — pairing lives in `FP_PORT_FIELD_SEL`.
- **Key split:** the SDK writes the whole key/mask as one blob via overlay fields `KEYf`
  / `MASKf` (`trx/field.c:2535`), never per-`Fn`. For hand-coding via CDK that means:
  put qualifier **values** in `F1..F4`/`FIXED` (+`PAIRING_*`), masks in the `_MASK`
  variants. For a dst-only / match-all rule both halves are key=0/mask=0.
- `PAIRING_F1..F4`/`PAIRING_FIXED` = the *paired slice's overlaid* qualifier bits packed
  into the same 256-wide line (pairing overlay `trident/field.c:1245-1348`, offsets
  `fixed_pairing_overlay=215`, `ipbm_pairing_overlay=0`). Populated by the offset table,
  not by an explicit `PAIRING_*` write in the SDK.

## The Trident-mandatory writes edged was MISSING

1. **`FP_GLOBAL_MASK_TCAM` at EACH slice index (primary AND secondary), VALID=1.**
   `triumph/field.c:3303-3350`, explicit comment: *"For Trident, the VALID field of
   FP_GLOBAL_MASK_TCAM must be set, even if IPBM and IPBM_MASK are all zeroes."*
   Match-any ingress port = **IPBM=0, IPBM_MASK=0, VALID=1** (SDK default — NOT a bitmap
   of all-ones, NOT an X/Y transform). edged wrote only the primary and used a wrong
   X/Y-encoded value.

2. **`FP_GM_FIELDS` at the SECONDARY index, VALID=1.** `field_common.c:30142` sets
   `_FP_ENTRY_USES_IPBM_OVERLAY` on **part 1**; `triumph/field.c:3363-3380` then does
   `WRITE_FP_GM_FIELDS` with VALID=1. edged omitted this **entirely** — the paired lookup
   cannot complete without the part-1 IPBM overlay. Prime suspect for the persistent
   `counter=0`.

## Registers that make a double-wide group "live" (beyond the entry)

Set in `_bcm_field_trx_ingress_selcodes_install` (`trx/field.c:1560`):

- `FP_PORT_FIELD_SEL[<each ingress port>]`:
  - `SLICE{S}_{P}_PAIRINGf = 1` — array `_bcm_field_trx_slice_pairing_field[slice_num/2]`
    (`trx/field.c:120-129, 1712`), indexed by **physical `slice_num/2`**. Physical 6+7 →
    `SLICE7_6_PAIRINGf`; physical 8+9 → `SLICE9_8_PAIRINGf`.
  - `SLICE{P/S}_DOUBLE_WIDE_MODEf = 0` (that bit is for *intra*-slice double-wide only,
    `trx/field.c:1706`).
  - `SLICE{P/S}_F1/F2/F3f` = select codes (`trx/field.c:1730-1748`), for **both** slices.
- `FP_SLICE_ENABLE`: **both** `FP_SLICE_ENABLE_SLICE_Nf=1` and
  `FP_LOOKUP_ENABLE_SLICE_Nf=1`, for **both** slices (`trx/field.c:33615-33649`;
  `field_common.c:22548`).
- `FP_SLICE_MAP`: virtual→physical + `VIRTUAL_SLICE_GROUP` (`trident/field.c:2157-2205`).

## ⚠ The consistency rule (likely the historical bug)

The **pairing field index, the two FP_TCAM base indices, the two SLICE_ENABLE/LOOKUP bit
pairs, and the SLICE{P/S}_Fx selects must all name the SAME physical slice pair.**
edged's `cumulus_replicate.c` sets `SLICE8_F2=1` + `SLICE9_8_PAIRING=1` (naming slices
8/9) while the ACL entry is written at idx 1537 (physical slice 6). Mixing "virtual 8/9"
naming with physical-6/7 indices silently prevents the pair from forming. **Open item:**
confirm whether `FP_PORT_FIELD_SEL`/`FP_SLICE_ENABLE` are physical- or virtual-indexed on
this chip (the running SDK "oracle" resolves this instantly; it is currently down — see
below) and align all four to one pair.

## Concrete recipe (match-all the IFP will consult)

Primary physical slice `P`, secondary `S=P+1` (adjacent, bases `P*256`, `P*256+256`):

1. `FP_TCAM[P*256+i]`   = VALID=3, key/mask/pairing per rule (match-all → all 0).
2. `FP_TCAM[P*256+256+i]` = VALID=3, all 0 (the paired half).
3. `FP_GLOBAL_MASK_TCAM[P*256+i]`     = VALID=1, IPBM=0, IPBM_MASK=0.
4. `FP_GLOBAL_MASK_TCAM[P*256+256+i]` = VALID=1, IPBM=0, IPBM_MASK=0.   ← was missing
5. `FP_GM_FIELDS[P*256+256+i]`        = VALID=1, key/mask=0.            ← was missing
6. `FP_POLICY_TABLE[P*256+i]`         = action (+ COUNTER_MODE for the match signal).
7. Group-live regs: `SLICE{S}_{P}_PAIRING=1`, `SLICE{P/S}_Fx` selects, both
   `FP_SLICE_ENABLE`/`FP_LOOKUP_ENABLE` bits — all for the **same** physical pair.

## Fields the IpType `FIXED` value (found 2026-07-08)

Every Cumulus IPv4 FP entry uses `FIXED=0x100 / FIXED_MASK=0x300` and
`PAIRING_FIXED=0x200 / PAIRING_FIXED_MASK=0x600` (capture `mem_FP_TCAM.txt`). `FIXED` is a
DltaCam field (X=`FIXEDf`, Y=`FIXED_MASKf`, same K0/K1 as F2): `X=care&val`, `Y=~care|val`
→ `FIXEDf=0x100, FIXED_MASKf=0x1fdff, PAIRING_FIXEDf=0x200, PAIRING_FIXED_MASKf=0x7fbff`.
edged had left the FIXED **value** 0 → the entry required IpType==0, which no IPv4 packet
satisfies (blocked every match incl match-all). Fixed in acl.c.

## Diagnostics / gotchas

- **CDK TCAM readback is unreliable** on this chip: write `IPBM=0x1fff…`, CDK reads back a
  shifted value (hardware normalizes X/Y). Trust only the **counter**, not field readback.
- `edged_acl_diag` (SIGUSR1 → `/tmp/edged-acl.log`) scans **all 2048 FP_COUNTER_TABLE**
  entries for nonzero — the only reliable "did anything match?" signal (a match may land
  at `COUNTER_INDEX != tcam idx`).
- The **SDK "oracle"** (`/root/bcm.user`, a PPC SDK build used read-only via the kernel
  BDE) is the way to see true chip state, but is **down after the reflash**: LUBDE ioctl
  ABI mismatch (bcm.user sdk-6.5.16 sends `_IOC(_IOC_NONE,'L',5)`; the reflashed
  `linux-user-bde.ko` returns ENOTTY). Restore = build a matching `linux-user-bde.ko`
  from OpenBCM 6.5.16 for kernel 6.1.175-edgenos and swap it (rmmod is refcount-0 but
  risky without serial), or rebuild bcm.user from the SDK the module came from.

## 2026-07-09 — bugs fixed, and the definitive result

Four real bugs found+fixed this pass (all SDK-backed, all keepers):
1. **`FIXED`/IpType value** never set → entry required IpType==0 → no IPv4 packet matched.
   Now `FIXED=0x100/mask 0x300`, `PAIRING_FIXED=0x200/mask 0x600` (X/Y encoded).
2. **`FP_GM_FIELDS`** at the secondary index was omitted entirely (Trident part-1 IPBM
   overlay). Added (VALID=1).
3. **`FP_GLOBAL_MASK_TCAM`** written only at the primary; now at both slice indices,
   and the gate is the SDK's plain `IPBM=0/IPBM_MASK=0/VALID=1` (not the X/Y guess).
4. **`acl_enable_port_filter()` looped `p<64`** but the uplinks ingress on chip ports
   **65/66** (`PORT_TABm_MAX=66`, portmap.c:42 SFP+1-8 → chip 65..72). So the per-port
   IFP `FILTER_ENABLE` was never set on the ports the traffic enters. Now `p<=PORT_TABm_MAX`.
   Also completed the (6,7) double-wide pair (`SLICE7_6_PAIRING=1` + `SLICE7_Fx`).

**Result: the IFP still performs NO lookup.** A match-all count entry placed in four
different physical slices at once (probe: bases 50/562/1074/1586) counts zero on every
slice, and a deny rule drops nothing. The on-chip readback proves the config is not just
written but **live and correct on the ingress ports**:
```
INGRESS port=65 FILTER_EN=1 PFS S6.F2=1 S7.F2=5 S8.F2=1 S9.F2=5 PAIR7_6=1 PAIR9_8=1
INGRESS port=66 FILTER_EN=1 PFS S6.F2=1 S7.F2=5 S8.F2=1 S9.F2=5 PAIR7_6=1 PAIR9_8=1
```
Every entry/slice/port/gate value the SDK sets is present AND verified live, yet the IFP
consults nothing. This **rules out register/memory config entirely** and localizes the wall
to the SDK `soc_init` FP-engine bring-up — an integrated init step (not any static register
value) that OpenMDK's `bmd_init` skips. The remaining paths are: run the SDK's `soc_init`
FP portion on the box (the oracle/full-SDK path), or the L3 `DST_DISCARD` fallback.

A probe hook (`probe` rule name → match-all count entries at even-slice bases) and an
ingress-port readback (`FP_PORT_FIELD_SEL[65/66]` + FILTER_ENABLE) were added to the
file-diag (`/tmp/edged-acl.log`) for future slice-geometry/engine investigation.

## Table sizes (BCM56840_A0 CDK)

`FP_TCAM` 0..2047 · `FP_GLOBAL_MASK_TCAM` 0..2047 · `FP_GM_FIELDS` 0..2047 ·
`FP_POLICY_TABLE` (512-deep, counter/stat indices separate) · `FP_SLICE_ENABLE`:
`SLICE_ENABLE_SLICE_N` = bit N, `LOOKUP_ENABLE_SLICE_N` = bit 10+N (N=0..9).
Runtime `FP_SLICE_ENABLE=0x000f33ff` ⇒ lookup on physical slices 2,3,6,7,8,9.

---

# 2026-07-09/10 — ★ SOLVED IN SILICON ★ It was slice PLACEMENT, not engine init

## The real root cause: non-uniform slice geometry
Trident+ IFP FP_TCAM slices are **not** uniform 256 entries (`trx.h:756-758`,
`field_common.c:20427`):
- **Physical slices 0–3 = 128 entries each** (single-wide slice size)
- **Physical slices 4–9 = 256 entries each** (double-wide slice size)
- 4·128 + 6·256 = 2048 (`FP_TCAMm_MAX = 2047`).

Correct index → physical-slice map:

| Phys | index range | size | lookup(`0x000f33ff`) |
|---|---|---|---|
| 0 | 0–127 | 128 | off |
| 1 | 128–255 | 128 | off |
| 2 | 256–383 | 128 | on (paired 3_2) |
| 3 | 384–511 | 128 | on |
| 4 | **512–767** | 256 | off → we enable it |
| 5 | 768–1023 | 256 | off |
| 6 | 1024–1279 | 256 | on (paired 7_6) |
| 7 | 1280–1535 | 256 | on |
| 8 | 1536–1791 | 256 | on (paired 9_8) |
| 9 | 1792–2047 | 256 | on |

Everything (FP_TCAM index, `FP_LOOKUP_ENABLE_SLICE_N`, `FP_PORT_FIELD_SEL.SLICEn`,
pairing) uses the **same physical slice number**. `FP_SLICE_MAP` only sets the
virtual-slice-*group* for priority; it does NOT remap the TCAM index or key-select.

**Why nothing ever matched:** in the live config every lookup-enabled slice (2,3,6,7,8,9)
is **double-wide-paired**, and every single-wide/unpaired slice (0,1,4,5) has lookup
**off**. So there was no slice that was both lookup-enabled AND single-wide — a lone
single-wide entry could never be consulted anywhere. edged's entry at idx 1537 is
physical slice **8** (not 6); idx 562 is slice **4** (lookup off).

## PROVEN fix (oracle, live, no reset): single-wide slice 4
Steps (all RMW, no chip reset — validated on <mgmt-net-host>):
1. `FP_SLICE_ENABLE |= FP_LOOKUP_ENABLE_SLICE_4` (bit 14) → `0x000f73ff`
2. `FP_PORT_FIELD_SEL[each ingress port]`: `SLICE4_F1=5, SLICE4_F2=1, SLICE4_F3=7`;
   leave `SLICE5_4_PAIRING=0`, `SLICE4_DOUBLE_WIDE_MODE=0`
3. Entry at idx 512–767: `FP_TCAM` VALID=3, `FP_GLOBAL_MASK_TCAM` VALID=1/IPBM_MASK=0,
   `FP_POLICY_TABLE` DROP=1 — **single-wide, no secondary, no PAIRING_* fields**

**RESULT: a match-any entry here dropped 100% of swp1+swp2 traffic** — first-ever IFP
match on EdgeNOS. Mechanism fully proven. (edged's double-wide at slice 8 is NOT
consulted even for match-any — the pairing/wide-mode isn't right; single-wide slice 4
sidesteps all of it.)

## DstIP key offset — the last open detail
Match-any drops, but no `F2=DstIp` constraint has matched yet. The single-wide DstIp
position is selcode-dependent (`trident/field.c` `_field_trident_qualifiers_init`,
"FPF2 single wide" `f2_offset=46`):
- selcode 0/1 → DstIp at key bit `f2_offset+64 = 110` (32b)
- selcode 7 → DstIp at key bit `f2_offset+48 = 94`
- `F2=2`=SrcIp6, `F2=3`=DstIp6 (NOT DstIp — Cumulus `SLICE0_F2=2` slices are IPv6)

Tried F2-field words 0/1/2/3 and F2-field bit 62 — none matched (match-any still drops,
so mechanism is fine). Suspected cause: the F2 **field** starts at entry bit 48 (CDK
`F2f=cdk_field(fp_tcam,48,175)`) while the qualifier base `f2_offset=46` — a skew between
the F2-field view and the KEY-overlay view. NEXT: write via the **KEY overlay** at the
absolute qualifier bit (110) instead of the F2 field, or read Cumulus's exact single-wide
DstIp entry. Cumulus's captured ACL is DOUBLE-wide (idx 1555 = phys slice 8, DstIp @F2
word2 = `0x00000000<ip>0000000000000000`), so it's only a reference for the double-wide
layout, not single-wide.

## Recovery gotcha
A **match-any DROP flushes ARP + OSPF**; clearing the entry alone does NOT self-recover
(ARP stays FAILED). Recover with `systemctl restart edged && systemctl restart swp-l3`.
A **dst-IP-scoped** deny only drops that one flow, so it's the safe way to test. An empty
`acls.conf` reload does NOT rewrite `FP_SLICE_ENABLE` — restore it explicitly.

## What's left (feature)
- Lock the single-wide DstIP offset (KEY-overlay at bit 110) — 1 verify.
- Bake into edged: `ACL_IDX_BASE=512`, `FP_SLICE_ENABLE|=bit14`, `SLICE4` single-wide
  selcodes, single-wide entry (no secondary/pairing).
- Then: per-port `apply` (scope via `FP_GLOBAL_MASK_TCAM` IPBM — oracle applies the
  `IFP_GM_LOGIC_TO_PHYS_MAP` remap the CDK omits), multi-rule precedence (lower idx =
  higher prio), match counters (COUNTER_MODE/INDEX were flaky via `mod`), then src/proto/L4.

---

## FINAL ROOT CAUSE (2026-07-12) — the IFP wall bottoms out at the BDE, not the ACL

The single-wide slice-4 bake-in above is byte-perfect: on the live chip every enable/map/
selcode/entry/policy the SDK programs is verified correct (FP_SLICE_ENABLE=0x000f73ff,
FP_SLICE_MAP VS6->PS4, selcodes, PORT_TAB.FILTER_ENABLE, IFP_BYPASS=0, entry VALID=3,
FP_POLICY_TABLE G/R/Y_DROP=1, GMASK VALID=1). Yet even a true match-any never drops and the
FP counter is 0 across all 2048 entries — the IFP does not consult ANY slice. Exhaustive
disambiguation ruled out encoding: both mask polarities were tested (edged's masks-zero entry
and an SDK-`mod`-written masks-all-ones entry), neither drops, so whichever is match-any is
simply not consulted.

The engine only consults after the SDK's `soc_init`/`bcm_init` runtime activation, which our
setup cannot complete:
- `soc_skip_reset=1`: aborts before `misc_init` (fatal XLPORT reads on unclocked ports).
- Full reset (`soc_skip_reset=0`, `phy_null=1`, MIIM patched, `xgxs_lcpll_xtal_refclk=1`):
  the LCPLL config lands correctly (verified MDIV=20/NDIV=140/PDIV=7 = SDK 156.25MHz values)
  but the PLL never LOCKS under sdk-6.5.16's `soc_reset_bcm56840_a0` (single 10ms lock check,
  no VCO calibration kick). Non-fatal warning, fatal consequence: the SDK's CMIC soft-reset
  kills our polled kernel-BDE's SCHAN and it never recovers — after ANY SDK reset even plain
  register reads time out until `systemctl restart edged` (bmd_init) re-establishes SCHAN.

Cumulus (SDK 6.3.8, interrupt-driven BAR0-mmap user-BDE) locks the same PLL on the same board
and survives its own reset; ours does not. **So the blocker is the BDE architecture, not the
ACL programming or config values.** Unblocking the 5610 IFP requires re-architecting the BDE
to survive/re-init the CMIC-SCHAN across an SDK reset (the Cumulus BDE model). This commit is
the known-good rollback point immediately before that BDE work. The one SDK patch worth
keeping is captured in `sdk-patches/`. Everything else on the 5610 (forwarding/L3/OSPF/ECMP)
works. See memory `project_acl_soc_init_pathA_deadend` for the full trace.
