# 0.3.0-alpha36 — L3AR slice 2, and the whole L3AR block is authored

Slice 2 was the last one. **All five L3AR slices are now generated from our own source.**

## What changed

`asic/fm6000/fm6000_l3arslice2.c` (from `asic/fm6000/tools/gen_l3ar_slice2.py`) programs slice
2 — the classify-and-tag stage. Per rule it selects a **VID** profile (the bulk of the stage:
profiles 6, 8, 10, 12, 18, 20, 22, 24), a **trap header** command on rules 2-3, an **L2 lookup**
command profile with the MA1/MA2 MAC muxes on rules 22/23/26/27, **policer index** muxes (POL1,
POL2 and POL3 all appear here), `ALU46` profiles on four rules, and the **W16ABC** action-data
channel plus **W8E** on rules 15/16.

**800 writes**, disjoint from slices 0, 1, 3 and 4 — checked, zero overlap with all four.

Three rules are dead in EOS's table (never-match bit); emitted as never-match with zero actions.
Slice 2 rewrites `ACTION_FLAGS` in **twelve** distinct combinations, more than any other slice,
so all four flag words are carried per rule.

`--verify`: **725 of 725 live words identical, 0 differing, 3 dead rules confirmed never-match
on both sides.** Passed first build.

## The L3AR block, complete

| slice | function | authored in |
|---|---|---|
| 0 | forwarding — L2 lookup key, DGLORT/DMASK, ALU46 | `fm6000_l3arinit` (pre-existing) |
| 1 | SGLORT + csGlort assignment | alpha32 |
| 2 | VID assignment and trap | **alpha36** |
| 3 | ALUs, policers, VID, W16ABC | alpha35 |
| 4 | QoS classification + ALU46 operand | alpha34 |

## Measured

| | alpha31 (before) | alpha36 |
|---|---|---|
| executed writes | 130,966 | **130,672** |
| ours | 107,828 (82.3%) | **110,863 (84.8%)** |
| et1 / et2 | `0940` / `0940` | `0940` / `0940` |

**+3,035 writes moved from EOS's tables into our source** across alpha32/34/35/36.

Dataplane: `edgenos-up.sh` RC=0, 41 kernel routes, `fibd: programmed 14 route(s)`. Transit
passes — frames in on et2, out on et1 with SMAC rewritten to the router MAC, DMAC to the
next-hop, **TTL `0x3f` = 63**, and `et2 rx=0` on the TAP counters confirming the forwarding was
done in silicon rather than by the kernel.

## What made this tractable

Three things from this session, in order of leverage:

1. **The SDK register map** (`asic/fm6000/tools/sdk_regmap.py`) — 703 registers with widths and
   strides, plus 3,169 field definitions. Everything else follows from it.
2. **Byte-verification against the image.** Every slice's functional decode looked plausible and
   was wrong in some detail: slice 4 hid a whole QoS mux behind an unread RAM4 word 1, slice 3's
   flag rewrites were invisible until 10 words failed to match. A verify that only checked
   behaviour would have shipped all of them.
3. **Two working lanes as controls.** The three-way diff — keep only what both good lanes agree
   on and the subject differs from — is what turned "everything looks the same" into short,
   specific candidate lists.

md5 `b52c39c27f77dfd1b25a57dfcd15cff4`, verified on the switch before booting.
