# 0.3.0-alpha35 — L3AR slice 3 authored

Third L3AR slice replaced by a generator. Otherwise alpha34.

## What changed

`asic/fm6000/fm6000_l3arslice3.c` (from `asic/fm6000/tools/gen_l3ar_slice3.py`) programs
L3AR slice 3 — the arithmetic stage. Per rule it selects an ALU13 and/or ALU46 **command** and
**operand** profile, a **policer index** mux (`POL2_IDX` on the low rules, `POL3_IDX` on the
high ones), and for rules 15-26 a **VID** mux and the **W16ABC** action-data channel. Keys are
mostly `ACTION_FLAGS` combinations accumulated by slices 0-2, plus `L3_PROT_ID2`, `MAP_VID2`,
`L2_DMAC_ID3`, `L2_TYPE_ID2`, `SRC_PORT_ID4`, `NEXTHOP_TAG` and `FFU_DATA_TAG2B`.

**800 writes.** Disjoint from slices 0, 1 and 4 — checked, zero overlap with all three.

## Two things the verify caught

**Rules 23, 24 and 30 are DEAD in EOS's table.** Each carries a bit in the never-match state
(Key=0, KeyInvert=0) — rule 23's is `MAP_VID2` bit 0 — so none can ever fire, even though all
three hold RAM4 action words. We emit them as never-match with zero actions rather than
reproducing actions that can never execute. `--verify` therefore compares live rules
byte-for-byte and separately confirms the dead ones are never-match on *both* sides.

**Slice 3 rewrites flags, and a blanket pass-through was wrong.** Six distinct combinations:
rule 14 clears LO bits 6-7; several rules clear HI bit 0 (`ACTION_FLAGS` bit 26) while others
set it; `Set_HI` also takes `0x8` and `0x40000`. The first build emitted `Mask_HI=0x3ffffff`
everywhere and `--verify` failed on exactly the 10 words that differ. Fixed by carrying all
four flag words per rule.

Final: **725 of 725 live words identical, 0 differing, 3 dead rules confirmed never-match.**

## Measured

| | alpha34 | alpha35 |
|---|---|---|
| executed writes | 130,688 | **130,672** |
| ours | 109,263 (83.6%) | **110,063 (84.2%)** |
| et1 / et2 | `0940` / `0940` | `0940` / `0940` |

Dataplane: `edgenos-up.sh` RC=0, 41 kernel routes, `fibd: programmed 14 route(s)`. Transit
passes — frames in on et2, captured out on et1 with SMAC rewritten to the router MAC, DMAC to
the next-hop, **TTL `0x3f` = 63** from the peer's 64.

## L3AR status

| slice | function | authored |
|---|---|---|
| 0 | forwarding | `fm6000_l3arinit` (slice 0 only) |
| 1 | SGLORT + csGlort | alpha32 |
| 2 | VID assignment and trap | **remaining** |
| 3 | ALUs, policers, VID, W16ABC | alpha35 |
| 4 | QoS classification + ALU46 | alpha34 |

Slice 2 is the last one, and the densest: RAM3 28, RAM4 31, RAM5 31 of 32 rules.

md5 `a6e88145f2698643e6682bc7ed286dcf`, verified on the switch.
