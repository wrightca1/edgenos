# What is left in fwd4.txt, and what to do with each part

Generated from `regmap.py` (register names) plus write-pattern analysis.
**110,689 EOS-derived writes remain** of 389,809 — but they are not one problem, they are six,
and only some are worth attacking.

| # | group | writes | verdict |
|---|---|---:|---|
| 1 | SBus window (`0x00f000`) | 93,727 | SPICO firmware 90,006 + lane tuning 3,721 |
| 2 | CAMs | 5,364 | need entry↔strobe pairing |
| 3 | unnamed registers | ~3,700 | not in the indexed macros |
| 4 | collapsible tables | ~1,400 | **do these next** |
| 5 | CRM command triplet | 1,158 | a command sequence, not state |
| 6 | per-case | 634 | MAPPER_SRC_PORT_TABLE |

## 1. SBus — 93,727 writes, 85% of what remains

Not MMIO. Each SBus transaction is three writes to `0xf001`/`0xf002`, so per-write filtering would
tear transactions apart. Of these, **90,006 are Intel's SPICO SerDes firmware** — already proven
droppable for a fibre-only build (Et1 10GBASE-SR trains and forwards without it; Et2 copper does
not). The remaining 3,721 are per-lane board tuning.

**Action: none.** Drop SPICO for fibre-only; the tuning is board measurement, not something to
generate. This is the single largest item and it is already understood.

## 2. CAMs — 5,364 writes

| register | writes | registers |
|---|---:|---:|
| FFU_SLICE_CAM | 2,480 | 508 |
| PARSER_CAM | 1,308 | 436 |
| L2L_SWEEPER_CAM | 976 | 80 |
| MOD_CAM | 320 | 76 |
| FFU_SLICE_SCENARIO_CAM | 280 | 118 |

Every one is paired with a commit strobe — the FFU's fires at `0x3f0000`, 59 times. Separating
entries from their strobe is exactly what broke the first FFU attempt: links came up, unicast
forwarded, and OSPF never formed because the CPU-punt traps were never applied.

**Action: only with the strobe.** A generator would have to emit entry-then-strobe in the recorded
order, i.e. the EPL "relocate the sequence" treatment rather than a collapse. Tractable, not yet
attempted.

## 3. Unnamed — ~3,700 writes

`0x140000` (1,516), `0x141000` (1,464), `0x010000` (428), `0x144000` (328). Inside the L2AR and
L3AR ranges but not covered by the header's indexed macros — so either non-indexed registers, or
macros whose bounds `regmap.py` could not resolve.

**Action: extend regmap.py** to handle the unbounded/scalar macros it currently skips. Cheap, and
it may reclassify most of these into groups 2 or 4.

## 4. Collapsible tables — ~1,400 writes  ← DO THESE NEXT

| register | writes | pattern |
|---|---:|---|
| PARSER_INIT_FIELDS | 970 | idempotent repeats |
| ESCHED_DRR_Q | 444 | idempotent repeats |

Same shape as the three already lifted by `fm6000_tbl3init`. PARSER_INIT_FIELDS is the safer of the
two; ESCHED is delicate — reading `0x2000` in that block off-buses the chip, so treat scheduler
state carefully even though writing it is routine.

## 5. CRM command triplet — 1,158 writes

`CRM_COMMAND` 386, `CRM_REGISTER` 386, `CRM_PERIOD` 386 — identical counts, which is the tell: this
is a three-register command interface driven 386 times, not three tables.

⚠ The transition-rate heuristic labelled `CRM_PERIOD` "collapsible". **It is not** — it is one field
of a command sequence. This is the same trap as every other statistical shortcut in this project:
the numbers suggest, the register name decides. We replaced the CRM engine with `fm6000_memfill`
already, so these may be droppable outright — worth testing before generating.

## 6. MAPPER_SRC_PORT_TABLE — 634 writes

Changing values, interleaved through the loop. Needs per-case work; lowest priority.

---

## Realistic ceiling

Excluding SBus, **16,962 MMIO writes remain.** Groups 4 and 5 (~2,600) are near-term. Group 2
(5,364) needs the strobe-pairing generator. Group 3 (~3,700) needs a better decoder. Even clearing
all of them leaves the SPICO firmware, which is not generatable — only droppable, and only for
fibre.

So the honest ceiling for `fwd4.txt` is **elimination for a fibre-only build**, and *never* for
copper unless someone reimplements the SerDes firmware.
