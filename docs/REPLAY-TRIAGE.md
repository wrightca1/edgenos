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

## 4. Collapsible tables — ~1,400 writes  ← TRIED, BOTH BACKED OUT

| register | writes | result |
|---|---:|---|
| ESCHED_DRR_Q | 444 | **breaks forwarding** — OSPF up, RX alive, 100% loss on 8/8 rounds |
| PARSER_INIT_FIELDS | 970 | **degrades reliability** — 2 of 3 boots clean vs TBL3's 3 of 3 |

Both looked as collapsible as the three that worked: idempotent repeats, low transition rates, plain
table names. ESCHED_DRR_Q was bisected to itself by dropping it and re-testing. PARSER_INIT_FIELDS
passed its first boot at 7/8 rounds and only failed under soak — it would have shipped on a single
run.

**Group 4 is closed.** 1,414 writes is not worth degrading a platform that already fails one boot in
six. Scheduler state joins EPL and the CRM triplet in the category where statistics mislead.

## 5. CRM command triplet — 1,158 writes  ← DONE, DROPPED

`CRM_COMMAND` 386, `CRM_REGISTER` 386, `CRM_PERIOD` 386 — identical counts, which is the tell: this
is a three-register command interface driven 386 times, not three tables.

⚠ The transition-rate heuristic labelled `CRM_PERIOD` "collapsible". **It is not** — it is one field
of a command sequence. This is the same trap as every other statistical shortcut in this project:
the numbers suggest, the register name decides. We replaced the CRM engine with `fm6000_memfill`
already, so these may be droppable outright — worth testing before generating.

**Tested: they are.** Simply deleting all 1,158 works — 3 cold boots, routes 34–35, 17 of 18 ping
rounds at zero loss. `fm6000_crmdrop` lists the addresses for `gen_list` to filter and emits
nothing. Unlike the group-4 attempts this is a pure deletion, which cannot introduce a code path
that costs reliability.

## 6. MAPPER_SRC_PORT_TABLE — 634 writes

Changing values, interleaved through the loop. Needs per-case work; lowest priority.

---

## Realistic ceiling

Excluding SBus, **16,962 MMIO writes remain.** Group 4 is now closed as not-worth-it, and group 5
is a command interface rather than state, so the near-term list is shorter than it looked: Group 2
(5,364) needs the strobe-pairing generator. Group 3 (~3,700) needs a better decoder. Even clearing
all of them leaves the SPICO firmware, which is not generatable — only droppable, and only for
fibre.

So the honest ceiling for `fwd4.txt` is **elimination for a fibre-only build**, and *never* for
copper unless someone reimplements the SerDes firmware.
