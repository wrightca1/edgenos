# 0.3.0-alpha59 — differential state capture, and the bug it found

No new generator. This release is the result of a measurement: dump the chip's
register state after a **working** boot and after a **standalone** boot, and diff.
The question it was built to answer was whether standalone mode is missing static
configuration (fixable, and nameable by address) or differs only dynamically.

It found one real bug, and it retired the open question.

## Method

`asic/fm6000/fm6000_csrdump.c` reads an explicit address list and prints
`AAAAAAAA VVVVVVVV`. The list is generated from the SDK register map — **677
registers, 128,414 addresses**, excluding 26 bulk tables (L2L_MAC_TABLE at 262,144
entries, NEXTHOP_WIDE at 131,072, and so on) that would dominate the diff with
table content.

⚠ The tool takes a list. It never sweeps. See its header.

**Noise floor measured first: zero.** Two snapshots five seconds apart on the same
boot are byte-identical across all 128,414 registers, so any cross-boot difference
is signal, not jitter.

Both snapshots were taken after `FULLSEQ DONE` and **before** `edgenos-up.sh`, so
the control plane is not a variable.

## Result: 3,884 registers differ (3.02%)

Of those, 1,949 are counters and status (`*_USAGE`, `*_STATE`, `STATS`, `CRM`,
sampling) and are expected to differ. That leaves **1,935 configuration registers**,
which sort into exactly three groups:

| group | count | verdict |
|---|---|---|
| replay writes it, a generator claims it, chip is at reset default | **413** | **a bug** |
| written by nobody — not the replay, not any generator | 1,143 | uninitialized RAM |
| miscellaneous (2 SERDES_TX_CFG, 1 FC_MRL_TOKEN_LIMIT) | 3 | noted, not chased |

## The bug: an incomplete generator ran last and won

`fm6000_l2arpre` and `fm6000_l2arseq` are **mutually exclusive alternatives** — the
replay path picks one (`L2ARSEQ` vs `L2ARPRE`, and `L2ARPRE` is the default).
`STANDALONE_ORDER` listed **both**, with `l2arpre` after `l2arseq`.

`l2arpre` holds 25,426 writes to `l2arseq`'s 29,110, because it captured only the
**first** write of two-write sequences. Running second, it reverted them:

    0x144010   l2arseq:  ffffffff  then  00000000     <- correct final value
               l2arpre:  ffffffff                     <- ran last, and won

352 addresses were left at their reset value, **328 of them `L2AR_CAM_DMASK`**.

That is not a cosmetic difference. An all-ones mask in a ternary CAM is an entry
that **matches everything**. The action RAM behind those entries is never written by
anybody, so standalone mode was running with 328 live wildcard L2 rules pointing at
uninitialized memory.

**Fix:** `fm6000_l2arpre` removed from `STANDALONE_ORDER`. `l2arseq` is the superset
and the only correct choice when there is no replay behind it to fill the gaps.

**Verified on a clean standalone boot: 352 of 352 now match the working boot exactly.**

## The other 1,143 are uninitialized RAM, and provably harmless

They are written by nobody — absent from `fwd4.txt` and from every generator's
`-n` output — and their differing bits are scattered uniformly across all bit
positions, which is what powered-up SRAM looks like, not what configuration looks
like.

`FFU_SLICE_SCENARIO_CAM` settles it. Broken out by slice:

    slices  0-5    64 addresses each,   0 differ   (  0% )   <- the programmed slices
    slices  6-23   64 addresses each,  30-43 differ (~55%)   <- unused

The slices the FFU actually uses are **byte-identical between boots**. Only the
unused ones differ, and they differ randomly. The same argument retires the
`L2AR_ACTION_*` group (MIRROR, VID, TRAP_HEADER, MA_WRITEBACK, W16AB, W8ABCDE,
STATS_IDX*, 433 addresses).

The decisive point: **the working boot carries the same garbage in those locations
and forwards correctly.** So the garbage is not what breaks standalone.

## The 61 that remain are link state, not configuration

`L2F_TABLE_256` still differs on 61 words after the fix. Decoding the entries as
64-bit values and XOR-ing them, the differing bit positions across all 58 entries are:

    bit 20: 58 entries
    bit 40:  3 entries

Port 20 is et2 and port 40 is et1 — the two front-panel links. `L2F_TABLE_256` is a
**port-membership bitmap**, and the difference is which ports are members. That is
derived from link state, not written as configuration. Moving `fm6000_sweepinit`
last (after `fm6000_eplinit`) was tried and did not change it, which is itself the
evidence: it is not an ordering problem.

`fm6000_sweepinit` stays last regardless — that ordering is correct on its own
merits and matches what `gen_split` already documents.

## What this closes

**Standalone mode is not missing static configuration.** After this fix the only
config state that differs from a working boot is uninitialized RAM the working boot
also has, plus two link-membership bits. The remaining forwarding failure is
therefore **dynamic** — interleaving or a runtime protocol — which is consistent with
the earlier `RESIDUAL_FIRST` result that neither batch order forwards.

That converts the central open question from "what are we not writing?" (answered:
nothing) to "what sequence are we not performing?".

## Not fixed here

Standalone still does not punt: `et1 rx=0`, no OSPF adjacency, 5 kernel routes.
The L2AR fix removed a genuine hazard but was not sufficient on its own.

## Measured

| | alpha58 standalone | alpha59 standalone |
|---|---|---|
| generators run | 42 (0 non-zero) | 41 (0 non-zero) |
| config regs differing from a working boot | 1,552 | **1,249** |
| of which are a real gap | 413 | **61** (link bits) |
| `post-spico` | `et1=08c0/0940 et2=08c0/0940` | same |
| forwarding | no | no |

Replay mode is untouched by this change — line 462 still selects `l2arpre` via
`gen_preloop`, and only `STANDALONE_ORDER` was edited. Verified after restoring
`fwd4.txt`/`fwd5.txt`: 44 kernel routes, OSPF adjacency up, `fibd: programmed 14
route(s)`, `et1 rx=46`, and 0% loss both to the OSPF neighbor and to a destination
learned behind it.

## Build

    VERSION=0.3.0-alpha59 KERNEL=$PWD/ex/linux-i386 BASE_INITRD=$PWD/ex/initrd-i386.gz \
        sh ./build-release-swi.sh -o $PWD/edgenos-7150-0.3.0-alpha59.swi

Kernel and base initrd extracted from alpha58.swi pulled off the switch. Note the
two footguns: `BASE_INITRD` must be an **absolute** path (the script `cd`s first)
and must **end in `.gz`** (it is read with `zcat`, which appends the suffix).

md5 `42c522f87d1397cd700aa1a99583d75c`, 19,047,457 bytes, verified on the switch
after transfer.
