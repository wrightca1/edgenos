# Removing `fwd4.txt` — a separate project from the microcode generators

Status 2026-08-18. **Measured on the running alpha16 image**, not carried forward from earlier notes.

## Two goals that have been tracked as one

They have different blockers, different end states, and different definitions of done. Conflating
them has made both look closer than they are.

| | **Goal A — licence** | **Goal B — the blob** |
|---|---|---|
| statement | no EOS-derived microcode **in our source tree** | no EOS-derived **file executed at boot** |
| enemy | `fm6000_*init.c` transcribed pairs | `/mnt/flash/fwd4.txt` |
| done when | every block is authored by a generator | FULLSEQ runs with no operator-supplied file |
| tracked in | `FEATURE-COMPLETE-CHECKLIST.md` section A | this document |
| status | parser ✅, L3AR ✅, MOD ~1 step, L2AR 1 decode | **52.4% of executed writes are generator-sourced** (measured, see below) |

Goal A is nearly finished. Goal B is not, and the reason is in the inventory below.

## What actually executes today

`fm6000-fullseq.sh` STEP5 deliberately runs the **full** two-port replay, because that is what brings
Et2 and ECMP up. Twenty generated blocks filter it down, and what is left is:

```
/mnt/flash/fwd4.txt (working)   373,345 writes   md5 71bffafe...
executed after 20 generators    220,972 writes   (40.8% removed)
  verbatim from fwd4.txt        220,560   99.8%
  ours / new                        412    0.2%
fwd4 pairs no longer executed    72,575   19.4%
```

⚠ **Use the box's `/mnt/flash/fwd4.txt`, not the repo's `fwd4.txt`.** They differ: the repo copy is
the stock 389,809-line capture; the box runs a 373,345-line spliced working file with a different
md5. Measuring against the wrong one overstated "ours" by 4x on the first pass here.

⚠ **The "93.5% of the replay eliminated" figure in `SELF-CONTAINED-PLAN.md` does not describe this
image.** Whether it refers to a leaner single-port configuration or counts differently is unresolved;
do not quote it without reconciling it against the numbers above.

### The remaining 220,972 writes, by block

| block | writes | share |
|---|---:|---|
| **JSS (SBus master)** | 93,728 | 42.4% |
| L2AR | 29,110 | 13.2% |
| L2L | 24,620 | 11.1% |
| EPL | 22,051 | 10.0% |
| FFU | 13,525 | 6.1% |
| CM | 9,451 | 4.3% |
| MAPPER | 6,644 | 3.0% |
| MOD | 4,834 | 2.2% |
| L3AR | 4,481 | 2.0% |
| PARSER | 2,892 | 1.3% |
| HASH, L2L_SWEEPER, MONITOR, STATS_AR, … | ~10,600 | 4.4% |

## ★ 41% of the blob is a firmware image, not a table — and on fibre it is deletable

The JSS block is the SBus master, and its five addresses carry a three-write transaction
(`REQ`=data, `CMD`=0, `CMD`=trigger — `fm6000_initsbus.c`). Decoding all of them:

```
31,241 SBus transactions
  op 0x21 WRITE   30,327
  op 0x22 READ       912
  op 0x20 RESET        2

device 0xfd (SPICO broadcast)   30,479   97.6%
  reg 0x06  12,002
  reg 0x04   6,000
  reg 0x05   6,000
  reg 0x07   6,000          <- these four sum to exactly 30,002
```

**30,002 transactions = 90,006 MMIO writes = 40.7% of everything still replayed** are the **SPICO
SerDes firmware upload**. That figure independently reproduces the `30,002` already named in
`fm6000-fullseq.sh`'s comment, derived here from the transaction stream rather than from the comment.

This is categorically different from the rest of the list. It is **Intel's SerDes microcode**, a
binary image. No amount of decoding turns it into something we author — decoding a table lets you
*regenerate* it; decoding a firmware image just tells you what the firmware is.

### ...but it is load-bearing for COPPER ONLY — and that is the opening

Stripping SPICO measured **0/7 links on Et2 (10GBASE-CR copper) against 5/10 with it**
(Fisher p = 0.041). But **fibre is unaffected: `Et1 = 0x00000cc0` on all 17 stripped boots**
(`SPICO-RE.md`). The firmware is needed for the DAC port, not for the chip.

> **So on a fibre-only configuration, 90,006 writes — 41% of the blob — can be deleted outright, and
> that has already been validated across 17 boots.** This is the single largest available reduction
> in the project and it needs no new decoding.

⚠ It costs Et2, and therefore the transit rig (in Et2, routed, out Et1) that A4/B1 depend on. So it
is a *release* configuration, not a lab one — which means the two builds diverge and both need
soaking. Do not strip it from the lab image.

## Consequences for Goal B

Sorting the 220,972 writes by what it would actually take:

| category | writes | share | what removal requires |
|---|---:|---|---|
| **SPICO firmware** | 90,006 | 41% | **on fibre: delete it** (validated, 17 boots). On copper: cache-and-replay by our own loader, or a from-scratch SerDes firmware. **Not a decoding problem either way.** |
| **generatable tables** | ~95,000 | 43% | finish the Goal-A generators and widen their coverage — L2AR, FFU, L2L, MOD, PARSER, L3AR tails |
| **SBus lane config** | ~3,700 | 2% | board-measured SerDes tuning; reimplementation, not generation |
| **control / sequencing** | ~32,000 | 14% | bring-up logic that is load-bearing where it sits |

> **Goal B is not reachable by finishing Goal A alone** — but the gap is smaller than it looks.
> Generators take ~43%; stripping SPICO on fibre takes another 41% and is already validated. The
> genuinely hard residue is the ~14% of control/sequencing plus copper support.

## Ordered plan

**Phase 1 — take the generatable 43%.** This is the work already in flight and it also serves Goal A.
Order by writes per unit of effort: L2AR (29,110, one decode away), FFU (13,525), L2L (24,620),
then the MOD/L3AR/PARSER tails. Expect ~43% → near zero.

**Phase 2 — take the 41% on fibre.** ✅ Question 1 is already answered: fibre does not need SPICO
(17 boots). The work is to build a **fibre-only image with the 30,002 IMEM transactions stripped**,
soak it, and make it the release configuration. That is 41% of the blob for no new decoding.
Remaining questions, in order:
1. ~~do the fibre ports need SPICO~~ — **answered: no.**
2. can a single upload be cached to flash and re-pushed by our own loader? That removes the *file*
   dependency for copper without reimplementing the firmware.
   ⚠ This satisfies Goal B's letter while still shipping Intel's firmware. **Decide explicitly
   whether that counts** rather than discovering the ambiguity at the end.
3. what the firmware actually is — `SPICO-RE.md` has the entry points. Only worth it if 2 is rejected.

**Phase 3 — the control remainder.** ~32,000 writes of bring-up sequencing. The existing position is
that these "may not be liftable at all without reimplementing the bring-up logic rather than
relocating its trace", and nothing measured since contradicts that.

## Method rules carried into this work

- Count what **executes**, not what a document says was eliminated. This plan exists because those
  two numbers differed by 50 points.
- ⚠ **The two goals pull in opposite directions here, and I got this backwards on the first pass.**
  A block leaving `fwd4.txt` and moving into a `.c` file **serves Goal B and violates Goal A** — the
  file is gone, but Intel's program is now in our source. Only a block that is *authored* serves
  both. When a block can be re-encoded but not yet authored, say which goal the work is buying.
- Verify every removal on hardware across **multiple boots** — Et2 links on ~1 boot in 2, so a single
  clean boot proves nothing (`tools/README-7150-harnesses.md`).

---

## ✅ Phase 2 first step DONE — 90,006 writes removed and verified (2026-08-18)

`tools/strip-spico.py` removes the firmware upload from a replay file, dropping **complete 3-write
SBus transactions** (dropping only the trigger would strand a `REQUEST` that the next transaction
inherits as its data — a smaller file that silently corrupts).

```
/mnt/flash/fwd4.txt   373,345 lines   md5 71bffafe...
stripped              283,339 lines   md5 cac05757...
dropped                90,006 writes  = 30,002 transactions, exactly as predicted
```

⚠ **The stripper independently reproduced `cac05757…`** — the md5 of the arm used in the 2026-08-15
five-boot experiment (`SPICO-RE.md`). Written from the SBus transaction shape without reference to
that file, and byte-identical to it. That validates both the stripper and the earlier arm.

### Booted and measured on the current image

| | |
|---|---|
| executed replay | **220,972 → 130,966** (−90,006, −41%) |
| Et1 | `PORT_STATUS=0x0EC0` `LANE_STATUS=0x940` `pcsRx=1` — linked, tx **and** rx active |
| Et2 | `0x0815 / 0x0000` — dark, as expected on copper |
| peer → switch (Et1) | 3/3, 0% loss |
| switch → peer | 3/3, 0% loss |
| OSPF | adjacency up, **33 routes**, hardware FIB mirroring |

⚠ **Do not read the first ping as a result.** Run immediately after `edgenos-up.sh` it showed 100%
loss and 2 routes, which looks exactly like "stripping SPICO broke Et1". It was OSPF and ARP not yet
settled; the port counters and a `REACHABLE` neighbour said otherwise, and a retry was clean. Give
the adjacency time before concluding anything.

### Status against the goal

| | writes | of the original 373,345 |
|---|---:|---|
| removed by the 20 generators | 152,373 | 40.8% |
| removed by stripping SPICO | 90,006 | 24.1% |
| **still replayed** | **130,966** | **35.1%** |

**Just under two-thirds of the working replay is now gone**, on a fibre-only configuration that
passes traffic. The remaining 130,966 writes are the generatable tables (L2AR, L2L, EPL, FFU, CM…)
plus the control/sequencing residue — i.e. Phase 1 and Phase 3.

⚠ The lab image must keep SPICO: Et2 is the transit rig's ingress, and A4/B1 need it. Keep
`/mnt/flash/fwd4-spico.txt` (md5 `71bffafe…`) as the lab replay and treat the stripped file as the
release configuration. **Two builds, both needing soak.**

## Phase 1 first target: L2AR, characterised (2026-08-18)

The 29,110 L2AR writes split cleanly, and only 9% carry names from the register header:

| | writes | |
|---|---:|---|
| **rule array** `0x140000`–`0x1433a0` | 20,980 | 413 rules x 24 words, stride `0x20` |
| config pages above `0x1433a0` | 8,130 | `L2AR_*_PROFILE_TABLE` etc., the named 9% |

Measured against the documented geometry and matching it exactly: offsets **0..23** used within each
`0x20` stride, **413 distinct rule indices, max 412**. So the bulk of the block is the microcode rule
array that `l2ar_gen.py` already round-trips (`--verify`: 2,442 segments + 407 actions).

413 x 24 = **9,912 distinct words written 20,980 times** — the repeats carry changed values rather
than strobes, as with `TBL3`, so a final-value filter alone would halve the block.

### ⚠ Which goal does this buy?

`l2ar_gen.py` can **re-encode** EOS's 413 rules today. That removes 20,980 writes from the replay —
**Goal B progress, Goal A regression**: Intel's program moves from a data file into our source.

**Authoring** the rules needs semantics that are only partial: `L2AR-MICROCODE-STRUCTURE.md` warns
"semantics do NOT reduce to one bit per concept", and its "Where regeneration stands" table is stale
(it predates the authoritative key-layout section above it and still says the key layout is missing).

So the decision to take before writing code: **re-encode now and accept a Goal-A regression that a
later authoring pass reverses, or hold L2AR until the semantics are done and take FFU/L2L first.**
Recommend the latter — FFU (13,525) and L2L (24,620) are worth more than L2AR's 20,980 and do not
carry the same regression.

## Phase 1: FFU — first generator written and verified offline (2026-08-18)

FFU's 13,525 writes break down against `ffu_decode.py`'s address map:

| region | writes | share |
|---|---:|---|
| **`BST_ACTION`** (route table) | 8,346 | 61.7% |
| `SLICE_CAM` | 2,480 | 18.3% |
| above the documented map (`0x3fc000`) | 1,096 | 8.1% |
| `SLICE_SCENARIO_CAM` | 1,064 | 7.9% |
| `SLICE_ACTION` | 296 | 2.2% |
| `BST_KEY`, `BST_SCENARIO_CAM` | 184 | 1.4% |
| `ATOMIC_APPLY` strobes | 59 | 0.4% |

The commit-strobe split reproduces the documented figures exactly: **43 with `CAM_Slices`, 16 with
`BST_Slices`**.

### The BST is 63% of FFU and most of it is a memset

Of the 8,346 `BST_ACTION` writes, **8,144 carry just two values** — `0x00700000` on every even word
and `0x00000000` on every odd, 8144 of 8144 with no exceptions, in four contiguous runs. An entry is
two words and that pair is the empty/default action.

> Writing a table's default value over its own range is a **memset, not Intel's program**. This is
> the rare block that serves **both** goals: it removes replay lines *and* is genuinely ours.
> Re-encoding EOS's rules would only have bought Goal B.

`asic/fm6000/fm6000_ffubstinit.c` generates it; wired into `fm6000-fullseq.sh` as `FFU-BST` and into
the image tool list. **8,046 writes**, verified offline as set-identical to what the replay filter
removes.

### ⛔ The trim, and why simulating the filter mattered

The first version generated all 8,144 addresses. Simulating `gen_list` offline showed it removing
**8,276** lines to replace 8,144 — because **`gen_list` filters by ADDRESS, not by (address,value)**,
and 98 addresses inside the default-fill ranges *also* receive a real route-content write later
(`0x8000006`, `0x14000`, `0x3000008`, …). Those content writes would have been stripped and replaced
by empty entries: routes would come up blank, on hardware, for a reason invisible in the diff.

Trimming the runs to addresses that receive **only** the two default values gives an exact 1:1:
8,046 generated, 8,046 removed, and the removed set is **equal** to the generated set.

⚠ **Simulate the actual filter, not the intent.** "My generator emits exactly what the replay
contains" was true and insufficient — the filter is address-granular and the generator is
value-aware, and that mismatch is only visible if you run the filter.

### ✅ Validated on hardware (alpha17), and the accounting corrected

Built `edgenos-7150-0.3.0-alpha17.swi` (57 tools) and booted it:

```
[fs]   FFU-BST generated by us (296175 writes remain)
[fs]   final et1=000008c0/00000940  et2=000008c0/00000940
```

Both ports linked, `edgenos-up.sh` clean, **14 routes programmed**, and the transit rig forwards:
`ttl=0x3f`, dst `80:a2:35:81:ca:b4`, src `44:4c:a8:31:5d:ab`. Before the image build the tool was
also run directly on the box: 8,046 writes, readback identical, dataplane unaffected.

⛔ **I predicted the executed replay would drop to 212,926. It did not — it is still 220,972.**
`gen_list` **replaces** the lines it removes with the generator's output in the same file, so a
generator that emits as many writes as it removes leaves the line count flat. Only generators that
emit *fewer* writes than they remove (TBL3: 6,370 → 1,812) or none at all (CRM-drop) shrink it.

⛔ **And the pair-diff cannot see this change either.** The executed file is composition-identical
before and after — 220,560 pairs matching `fwd4.txt`, 412 not. Our generator reproduces EOS's values
*exactly*, because a table's default value is not a design choice. This is the same warning the
checklist records for L3AR: *"agreement is expected and is NOT the evidence of independence it was
for the parser. Audit the process, not the diff."*

**So what did this buy?** 8,046 writes are now **sourced from our tool instead of from `fwd4.txt`**.
That is real for both goals — the file is no longer their origin — but it is invisible to both the
line count and the diff.

⚠ **The project needs a metric that tracks this.** "Writes remain" measures file length, not
provenance, and it will read flat through most of Phase 1. The honest measure is *writes in the
executed file whose source is a generator*, which nothing currently records. Until it exists, Phase 1
progress cannot be quoted as a number.

## ★★★ Provenance accounting — and the project was measuring itself wrong

`fm6000-fullseq.sh` now counts writes emitted by generators (`GENW`, via a single `gen_emit` helper
all five `gen_*` variants route through) and reports at the end. Built as alpha18 and booted:

```
[fs]   final et1=000008c0/00000940  et2=000008c0/00000940
[fs]   provenance: 115874 of 220972 executed writes come from our generators
```

> **52.4% of every write the chip receives at boot is already ours.**

Both ports linked, 14 routes programmed, transit forwards (`ttl=0x3f`, correct MAC rewrite).

### Why this was invisible

Three different numbers were being quoted, and none of them measured provenance:

| number | what it actually measures | reads |
|---|---|---|
| "writes remain" | **file length** after filtering | flat — `gen_list` replaces what it removes |
| pair-diff vs `fwd4.txt` | **value agreement** | 99.8% EOS — but a default value is the same whoever writes it |
| **`GENW`** | **who emitted the write** | **52.4% ours** |

The pair-diff is the dangerous one: it said 0.2% ours and it is *arithmetically correct*. It cannot
be otherwise, because a generator that reproduces EOS's table values — which a correct generator
must — is indistinguishable from a copy by value alone. Same warning the checklist records for L3AR:
*audit the process, not the diff.*

⚠ The first version of this reported `115874 of 0`: `$CUR` is deleted at line 402, right after the
executed file is copied to flash. Reading the preserved copy fixes it. **A metric that reports a
plainly impossible total is the good failure — the one to fear is a plausible wrong number.**

### What this changes about the plan

Goal B was assessed at "~4% done on this image" from the pair-diff. On the provenance measure it is
**52.4%**, and the remaining 105,098 generator-less writes are the real target list. That does not
make the SPICO firmware or the control residue any easier — but it does mean Phase 1 is much further
along than the earlier numbers implied, and that finishing it is worth more than they suggested.

⚠ Still one boot per image. The harness rules require multiple before trusting anything Et2-dependent.

## The real Phase 1 remainder is 11,370 writes, not ~95,000

With `GENW` measuring provenance and JSS confirmed to contain **zero** generator-emitted addresses:

```
executed writes            220,972
generator-sourced (GENW)   115,874   52.4%
JSS/SBus, none generated    93,728   42.4%    <- SPICO firmware + lane config
un-generated, NOT JSS       11,370    5.1%    <- the real Phase 1 remainder
```

**Excluding JSS, 91.1% of the writes the chip receives are already generated by us**
(115,874 of 127,244). The earlier "~95,000 generatable" estimate in this document counted blocks
whose generators already exist and run.

Per-generator output, measured by running each tool's `-n`:

```
sweepinit 74,447   l2arpre 25,426   l2linit 24,568   ffuinit 8,680   cminit 8,180
ffubstinit 8,046   mapperpre 5,662  modinit 3,855    hashinit 2,048  tbl3init 1,812
parserinit 1,576   eplinit 1,027    l3arinit 640     eaclinit 356    laginit 316
```

⛔ **A method that failed, recorded so it is not repeated.** I first computed coverage by collecting
every generator's `-a` address list and testing each executed write against it. That gave 19.9%
covered and a per-block "un-generated" table — **both wrong**. `-a` is only implemented by the
`gen_list` generators; the `gen_split` ones (`sweepinit`, `l2arpre`, `cminit`, …) filter by address
*prefix* and return nothing for `-a`, so 108,053 generated writes were counted as un-generated. The
table looked entirely plausible. `GENW` is the trustworthy figure because it counts lines actually
emitted at boot rather than inferring from an interface not every tool implements.

### What this means for the plan

Phase 1 is nearly finished. The remaining 11,370 writes are worth taking for completeness, but they
are no longer the lever — **Goal B is now dominated almost entirely by the 93,728 JSS/SBus writes**,
of which 90,006 are the SPICO firmware. The order in this document should be read accordingly:
Phase 2 (SPICO) is the project, not Phase 1.

### Soak: alpha18 over 4 boots

| boot | FFU-BST ran | Et1 | Et2 | provenance | routes | transit |
|---|---|---|---|---|---|---|
| 1 | ✅ | `0x8c0/0x940` | `0x8c0/0x940` | 115,874 | 14 | **ok**, ttl `0x3f` |
| 2 | ✅ | `0x8c0/0x940` | dark | 115,874 | 14 | n/a (Et2 dark) |
| 3 | ✅ | `0x8c0/0x940` | dark | 115,874 | 14 | n/a (Et2 dark) |
| 4 | ✅ | `0x8c0/0x940` | `0x8c0/0x940` | 115,874 | 14 | **ok**, ttl `0x3f` |

- The generator ran on **4 of 4** boots and emitted an **identical 115,874** every time — its output
  is deterministic, which is what a table fill should be.
- Et1 linked 4/4; Et2 2/4, consistent with the documented ~50% copper coin and *not* attributable to
  this change.
- Routes programmed 4/4; transit forwarded on **both** boots where Et2 was up.

That is the multi-boot evidence the harness rules require. `FFU-BST` is validated.

⚠ `provenance: … of 0` in these logs is the alpha18 display bug (`$CUR` already deleted); the count
itself is correct and the fix is in the tree for the next build.

## ★★★ Phase 2 ANSWERED: the firmware can leave the replay entirely (2026-08-18)

The question was whether SPICO could be pushed by our own loader instead of inline in `fwd4.txt`.
**It can, and copper still works.**

```
replay          /mnt/flash/fwd4.txt = the stripped file, 283,339 lines (-90,006)
firmware        /mnt/flash/fm6000_spico_code.bin, 12,000 bytes = 6,000 words
loader          fm6000_spico 0000:02:00.0 <blob>      (already in the tree)
retrain         fm6000_lanelink 2
```

Measured on that boot:

| | |
|---|---|
| after FULLSEQ | Et1 `0x8c0/0x940`, **Et2 `0x815/0x0000` dark** |
| `fm6000_spico` | 6,000 words uploaded, **alive check ALIVE**, **CRC self-check OK** |
| after `lanelink 2` | **Et2 duty 15/16** over 80s (Et1 16/16) |
| dataplane | 14 routes, **transit forwards**: ttl `0x3f`, correct MAC rewrite |

For comparison, `lanelink` on a *full-SPICO* image gave a flapping 13/22. 15/16 here is materially
better, though it is one boot and not yet a rate.

### ⛔ This overturns a standing instruction in `fm6000-fullseq.sh`

The script says, in a comment that has been load-bearing for months:

> *"Do NOT load it with a separate `fm6000_spico` step before the replay: the replay later resets and
> starts the SPICO, wiping an early upload, and the SPICO then runs with an empty IMEM."*

That is **correct as written and does not apply here**. The failure it describes is loading *before*
a replay that still contains the upload. Two things changed:

1. The upload transactions are **stripped** from the replay, so nothing overwrites ours.
2. `fm6000_spico` is run **after** the replay, so it is the last thing to touch the SPICO — it does
   its own reset → upload → run, and the replay's leftover control ops cannot wipe it.

⚠ Note my stripper removes only regs `0x04-0x07`. The **108 SPICO control writes at reg `0x0C`
remain in the replay**, which is exactly why the stripped image still resets and runs an *empty*
SPICO and Et2 came up dark. Running our loader afterwards is what repairs that. A cleaner build would
strip the control ops too and let the loader own the whole sequence.

### What this means for Goal B

90,006 writes — **41% of the replay** — can leave `fwd4.txt` permanently. The firmware becomes a
standalone third-party file alongside `ucode_l2.raw`, which is the pattern `build-release-swi.sh`
already documents ("bring your own, from a licensed EOS"). It does **not** make the firmware ours,
and Goal A is unaffected; it removes the *replay's* dependency on it.

Combined with the provenance figure, the remaining replay would be:

```
283,339 executed   (fwd4.txt stripped)
115,874 generator-sourced
 11,370 un-generated, non-JSS
  3,722 JSS/SBus lane config that is NOT firmware
```

### Next, in order

1. **Soak it.** One boot. Et2 is a coin and `lanelink` has produced flapping locks before — this
   needs the 4-boot treatment `FFU-BST` got, measuring Et2 as a duty cycle each time.
2. Wire it into FULLSEQ: strip at build time, add a post-replay `fm6000_spico` + retrain step,
   gated so a missing blob degrades to "copper down" rather than a failed boot.
3. Strip the reg `0x0C` control ops too, so the loader owns the whole SPICO sequence.

### Soak: stripped replay + our SPICO loader, 4 boots

| boot | after FULLSEQ | `fm6000_spico` | Et2 duty after retrain | routes | transit |
|---|---|---|---|---|---|
| 1 | Et2 dark | ALIVE, CRC OK | **15/16** (Et1 16/16) | 14 | **ok** |
| 2 | Et2 dark | ALIVE, CRC OK | **16/16** (Et1 16/16) | 14 | **ok** |
| 3 | Et2 dark | ALIVE, CRC OK | **15/16** (Et1 16/16) | 14 | **ok** |
| 4 | Et2 dark | ALIVE, CRC OK | **15/16** (Et1 16/16) | 14 | **ok** |

**4 of 4 boots: firmware loaded, Et2 came up, transit forwarded.** Et1 16/16 throughout. Every boot
reached this point with Et2 dark, exactly as predicted — the replay still runs the reg-`0x0C` control
ops against an empty IMEM, and our loader repairs it.

So the firmware can leave `fwd4.txt`: **90,006 writes, 41% of the replay**, with copper working.

### ⚠ It also looks *better* than the stock configuration — but that is not established

Et2 came up on 4/4 boots here, against the documented **5/10** with the firmware inline. Tempting to
conclude this fixes the long-standing Et2 coin. It does not follow yet:

- Fisher exact, 4/4 vs 5/10: **p = 0.221**. Not significant.
- P(4/4 | true rate 50%) = **0.062** — one boot in sixteen produces this by chance.
- The two arms differ in **two** ways, not one: the firmware source *and* an added
  `fm6000_lanelink 2` retrain. Either could account for it, and this soak cannot separate them.

To settle it: run the same 4-boot soak with the retrain removed, and separately on a full-SPICO image
with the retrain added. That is ~8 boots and it would answer a question that has been open since
2026-08-12. **Do not record "SPICO loading fixes Et2" until then.**

### Soak: alpha19, with STEP5b wired into FULLSEQ — 2 of 4

Built alpha19 (STEP5b loads the blob and retrains inside FULLSEQ, no manual steps) and soaked it:

| boot | STEP5b | Et2 duty | transit |
|---|---|---|---|
| 1 | spico rc=0 | **16/16** | ok |
| 2 | spico rc=0 | **0/16** | none |
| 3 | spico rc=0 | **16/16** | ok |
| 4 | spico rc=0 | **0/16** | none |

**The firmware load itself is 4/4 reliable** — `spico rc=0`, ALIVE and CRC OK on every boot of both
soaks, and Et1 is 16/16 across all 8 boots. What is not reliable is Et2 coming up afterwards.

| arm | Et2 up | placement of load + retrain |
|---|---|---|
| manual | **4/4** | after FULLSEQ finished, i.e. after the settle loop |
| **STEP5b** | **2/4** | immediately after the replay, before the settle loop |
| stock (inline) | 5/10 | n/a |

⚠ **None of these differences is statistically established.** manual vs STEP5b **p = 0.429**;
STEP5b vs stock **p = 1.000**; manual vs stock **p = 0.221**. Four boots per arm cannot separate a
50% process from a 100% one — that needs ~5 clean boots to reach p<0.05 against a coin, and more to
compare two arms.

So the honest position:

- ✅ **The firmware can be loaded from a standalone blob by our own tool.** 8/8 boots, both
  placements. `fwd4.txt` no longer has to carry it — the 41% reduction stands.
- ⛔ **Where to put the load+retrain is unresolved.** The manual placement looked perfect and the
  wired one looks like a coin, but the samples do not support preferring one.
- 📌 One real observation, independent of the rates: when Et2 comes up it is **16/16**, and when it
  does not it is **0/16**. Binary, never the flapping `HiBer` lock `fm6000_lanelink` produced on a
  full-SPICO image. Whatever this configuration does to the lane, it does cleanly.

**Next experiment, and it is now well-posed:** move STEP5b's retrain to after the settle loop and
soak 5+ boots. If the manual placement's advantage is real, that is where it comes from.

### Soak: alpha20, STEP5b moved to AFTER the settle loop — 4 of 5

`spico_step()` is now a function with its call site chosen by `SPICO_AFTER_SETTLE` (default 1 =
after the settle loop, reproducing the manual sequence; 0 = immediately after the replay). A 12s
settle was added after the retrain, because training is asynchronous and reading immediately always
reports `0x815/0x0000` and says nothing.

| boot | Et2 duty | transit |
|---|---|---|
| 1 | 2/16 | none |
| 2 | 16/16 | ok |
| 3 | 16/16 | ok |
| 4 | 16/16 | ok |
| 5 | **16/16** | **none** ⚠ — explained below: a `HiBer` lock, not a link |

Et1 16/16 on all five. Log ordering verified on the box: STEP5b runs at line 92, after `final et1`
at line 91.

### All four arms, and what is actually established

| arm | Et2 up | placement |
|---|---|---|
| manual | 4/4 | after FULLSEQ |
| alpha19 | 2/4 | after replay, before settle |
| alpha20 | 4/5 | after settle loop |
| stock (inline) | 5/10 | n/a |

⛔ **Still nothing separates them.** alpha20 vs stock **p = 0.580**; alpha20 vs alpha19 **p = 0.524**;
pooling every our-loader boot, 10/13 vs 5/10, **p = 0.221**. The 4/4 that motivated this experiment
did not reproduce as 5/5, which is what it would have taken.

**What 13 boots does establish:** the firmware load is **13/13** reliable (`rc=0`, ALIVE, CRC OK) and
Et1 is **13/13**. Taking SPICO out of `fwd4.txt` is safe. Making copper *more* reliable than stock is
not demonstrated, and on this evidence should not be claimed.

⚠ **An anomaly to chase, not to explain away.** Boot 5 had Et2 at a solid 16/16 and still no transit.
Every other boot with Et2 up forwarded. That breaks the assumption that "Et2 links ⇒ transit works",
which the whole rig leans on. Candidates: the peer's permanent neighbour entry, `edgenos-up.sh`
timing, or the punt path. It needs its own look — a green link with no traffic is exactly the failure
this project has been fooled by before (TAP carrier, `Link detected: yes` on the peer).

## ⛔ THE ANOMALY EXPLAINED — and it invalidates how I scored the soaks

Boot 5 had Et2 at `LANE_STATUS=0x940` for 16 of 16 samples and forwarded nothing. Reading the rest
of the port state:

```
Et1  PORT_STATUS=0x0EC0  LANE_STATUS=0x940  pcsRx=0x1     rx=354   clean
Et2  PORT_STATUS=0x09D5  LANE_STATUS=0x940  pcsRx=0x67    rx=0     HiBer
```

`0x09D5` has **bit 8, `HiBer`** set, and `pcsRx` is `0x67` rather than `0x1`. The lane is locked and
the data is garbage — a high-bit-error lock that forwards nothing.

> **`LANE_STATUS == 0x940` is NECESSARY BUT NOT SUFFICIENT.** It says the PCS achieved block lock. It
> does not say the link carries traffic.

### This is the same mistake twice

`tools/README-7150-harnesses.md` already records *"A TAP's `carrier` is not a link signal — read
`PORT_STATUS`"*. I replaced one insufficient signal with another: `LANE_STATUS`. The document even
notes, in the `lanelink` section, that its locks are marginal with `HiBer` set — and then I built a
duty-cycle metric that cannot see `HiBer`.

**And STEP5b's retrain calls `fm6000_lanelink`**, the tool already documented to produce exactly this
state. So the mechanism is not mysterious: the retrain locks the lane badly.

### What this does to the numbers

Every "Et2 up" count in the alpha19 and alpha20 soaks was scored on `LANE_STATUS` alone, so an unknown
number of them were `HiBer` locks that could not forward:

| arm | scored Et2 up | scored on | trustworthy? |
|---|---|---|---|
| manual | 4/4 | LANE_STATUS | ⛔ overcounts |
| alpha19 | 2/4 | LANE_STATUS | ⛔ overcounts |
| alpha20 | 4/5 | LANE_STATUS | ⛔ overcounts |

The **transit** column is the one that was right all along — it disagreed with the duty metric on
boot 5, and transit was correct. alpha20's honest score is **3 of 5 forwarding**, not 4 of 5.

### Fixed

`tools/fm6000-status.sh` now reports `LOCKED-HiBer(no traffic)` when bit 8 is set, so the state cannot
be mistaken for a link again.

**Score link health on transit, or on `LANE_STATUS==0x940` AND `HiBer` clear AND `pcsRx==1` — never on
`LANE_STATUS` alone.** Any future soak must use that, and the arms above need re-running before any
of them is compared.

## ★★★ alpha21: 5 of 5, on the CORRECT metric — and the retrain was never needed

The HiBer finding changed both the code and the scoring:

- **STEP5b now retrains until the lock is CLEAN**, testing `LANE_STATUS==0x940` **and** `HiBer` clear
  **and** `pcsRx==1`, treating a HiBer lock as a failure to retry rather than a success to report.
- **The soak scores on that same three-part test**, plus transit.

| boot | retrain attempts | Et2 clean-lock duty | transit |
|---|---|---|---|
| 1-5 | **0** | **16/16** | **ok** |

**5 of 5 boots: Et2 clean-locked and forwarding.** Et1 16/16 throughout.

### The retrain turned out to be unnecessary — and was the thing causing the damage

`attempts=0` on every boot: the loop tested the lane *before* calling `fm6000_lanelink` and found it
already clean. **Loading the firmware is sufficient; the retrain never ran.**

That inverts the earlier reading. alpha19/alpha20 called `lanelink` unconditionally, and `lanelink` is
the documented producer of HiBer locks — so the retrain was not fixing copper, it was *breaking* it.
The alpha20 boot-5 anomaly was `lanelink` leaving a locked-but-garbage lane, and the fix was to stop
calling it unless the lane actually needs it.

⚠ This is why the placement experiment kept failing to separate: both arms called `lanelink` every
time, so both were injecting the same damage at different points. The variable that mattered was not
*when* to retrain but *whether* to.

### Statistics

5/5 vs stock 5/10: **p = 0.101**, still not significant on its own — Fisher cannot do better with
n=5 against a 50% baseline. But **P(5/5 | true rate 50%) = 0.031**, so against the specific null "this
is the same coin as stock", 5/5 clears p<0.05.

⚠ Stated carefully: this is evidence the configuration is better than a coin, **not** an established
comparison against stock. The honest claim is "5 consecutive clean boots with a metric that would
have caught the failure", which is the strongest result copper has had.

### Status

```
replay          283,339 lines   (fwd4.txt stripped of the firmware, -90,006 = -41%)
firmware        fm6000_spico_code.bin, loaded by fm6000_spico from FULLSEQ STEP5b
copper          5/5 boots clean-locked and forwarding, 0 retrains
fibre           13/13 boots across every soak
```

## Status after the firmware came out — 88% generated, 15,092 writes left

The provenance line now displays correctly:

```
provenance: 115874 of 130966 executed writes come from our generators (88%)
```

| block | writes | covered by |
|---|---:|---|
| L2AR | 29,110 | `l2arpre` 25,426 — leaves the **3,684 in-loop bursts** |
| L2L | 24,620 | `l2linit` 24,568 |
| EPL | 22,051 | `eplseq` **22,051 — all of it** |
| FFU | 13,525 | `ffuinit` 8,680 + `ffubstinit` 8,046 |
| CM | 9,451 | `cminit` 8,180 |
| MAPPER | 6,644 | `mapperpre` 5,662 |
| **JSS** | **3,722** | **nothing** — SerDes lane config, not firmware |

**15,092 writes (12%) are not generator-sourced.** The two structural pieces in it are the
**L2AR in-loop bursts (3,684)** and the **JSS lane config (3,722)**, and both are documented as hard:

- ⛔ `L2ARSEQ=1` lifts the whole 29,110-write L2AR sequence and **was tried on 2026-08-08 and failed**
  — links up, OSPF fine (35 routes), ARP resolving, but **unicast ping 100% loss over 14 rounds**.
  L2AR is L2 *action resolution*; its interleaving with the loop matters in a way EPL's does not.
  Being a sequence made it a candidate; it did not make it relocatable. Do not flip that flag as a
  shortcut — the failure is specific and reproducible.
- JSS lane config is board-measured SerDes tuning, the same category as the firmware.

### The architectural point this exposes

Getting to 100% generated would still leave a *file*: FULLSEQ's generators **splice into a replay**
that `fm6000_fullreplay` executes, so the artifact exists even when every line in it is ours. Goal B
as written ("no EOS-derived file executed at boot") is already satisfiable at 88% — the remaining
12% is what makes the file still *EOS-derived*.

The endgame is therefore not only "generate the last 12%" but **"stop needing a replay file"**: every
generator already has a direct-MMIO write mode (running it without `-n`/`-a` writes the chip), so
FULLSEQ could invoke them in order and replay only the un-generated remainder. ⚠ Ordering is the
obstacle, and it is load-bearing — `gen_split`/`gen_list` exist precisely because *where* a block's
writes land in the sequence changes the outcome, as L2ARSEQ and L2F+LBS both demonstrate.

### Honest position

```
373,345  the working replay before any of this
130,966  executed today   (-65%)
115,874  of which ours    (88%)
 15,092  not ours, and ~7,400 of that is documented-hard
```

## ★★★ `gen_drop`: the replay actually SHRINKS — alpha22, 5 of 5

Every generator until now **spliced** its output back into the replay, so the file kept the same
length and only its authorship changed. `gen_drop` removes a generator's addresses and does **not**
splice: the tool is run in direct-MMIO mode instead, and those writes never live in a file at all.

```
[fs]   FFU-BST dropped from the replay (198123 writes remain, applied directly later)
[fs]   FFU-BST applied directly (pre-replay) rc=0
[fs]   provenance: 107828 of 122920 executed writes come from our generators (87%)
```

**The executed replay went 130,966 → 122,920** — 8,046 lines gone, not re-authored. The un-generated
count is unchanged at 15,092 either way, which is the consistency check that the accounting is right.

| alpha22, 5 boots | |
|---|---|
| Et2 clean-lock 16/16 | **5/5** |
| transit forwarding | **5/5** |
| retrain attempts | 0 on every boot |

### ⚠ Two constraints this pattern carries

**1. It must run BEFORE the replay, not after.** FFU tables are committed by the `ATOMIC_APPLY`
strobes at `0x3f0000`, and all 16 BST commits happen *during* the replay. Applying the defaults
afterwards would leave them uncommitted with the BST holding uninitialised SRAM for the whole replay.
This was caught by reasoning about the strobes before building, not by a failed boot.

**2. It is only valid for ORDER-INSENSITIVE blocks.** `gen_split`/`gen_list` exist because placement
changes the outcome — L2ARSEQ lifts the whole L2AR sequence and gives links up, ARP fine, **unicast
100% loss**; L2F+LBS must be last or the port map lands before the ports are configured. FFU-BST
qualifies only because its runs were trimmed to addresses that receive nothing but the default pair,
so the value is the same whenever it is written.

**Do not apply `gen_drop` to a block without establishing that property first.** The helper carries
that warning in its own comment.

### Where this leaves the endgame

`gen_drop` is the mechanism that can actually retire the file. Each block moved onto it shrinks the
replay by its own size. The candidates are blocks that are (a) generated and (b) order-insensitive —
which has to be established per block, and for most of the big ones the existing evidence says the
opposite.

```
373,345  working replay before this session
122,920  executed now      (-67%)
107,828  of which ours     (87%)
 15,092  not ours
```

## Taking more pieces out: 11 write-once blocks moved to `gen_drop`

Eligibility is measurable. A block is order-insensitive if **every address it writes receives exactly
one distinct value in the replay** — then writing it early lands identical state. Measured against
`fwd4.txt` for every generator exposing `-a`:

| ELIGIBLE (write-once) | writes | | ORDER-SENSITIVE | multi-valued addrs |
|---|---:|---|---|---:|
| `l2linit` | 24,568 | | `l2arseq` | 10,341 |
| `ffuinit` | 8,680 | | `tbl3init` | 1,079 |
| `ffubstinit` | 8,046 | | `eplseq` | 487 |
| `hashinit` | 2,048 | | `l3arinit` | 4 |
| 7 small blocks | 2,525 | | | |

All eleven eligible blocks now use `gen_drop`: their lines leave the replay and the tools write the
chip directly, pre-replay.

```
executed replay   130,966 -> 93,145     (-37,821 lines)
```

Verified by address: `ffubstinit`, `l2linit`, `ffuinit`, `hashinit` — **0 of their addresses remain**
in the executed file. Soaked over 5 boots: 4/5 clean-lock and forwarding, Et1 5/5, which matches the
copper rate seen on every other build.

### ⛔ Two bugs this exposed

**1. HASH was being generated twice.** A second loop, `for _g in l3ar hash`, builds the tool name as
`fm6000_${_g}init` — so grepping for `fm6000_hashinit` does not find it. My `gen_drop` removed HASH's
2,048 lines and that loop re-spliced them: the replay *grew* by 2,048 mid-chain. Dynamic invocation
hides call sites from grep; the log's per-step "writes remain" counter is what made it visible.

**2. The provenance metric did not count direct-applied writes.** `GENW` only counted `gen_emit`
output, so moving a block from splice to direct removed it from *both* numerator and file, and the
un-generated figure appeared to **grow** (15,092 → 23,138). Fixed: `gen_drop` now adds to `GENW` and
to a new `DIRECTW`, and the report totals `executed + DIRECTW` — the chip receives both.

```
provenance: 115874 of 139012 executed writes come from our generators (83%)
```

"Ours" is **identical** to the pre-`gen_drop` build's 115,874, which is the check that the accounting
is right: the same generators, delivered differently.

⚠ **Open:** the true total rose 130,966 → 139,012 (+8,046) across this change. HASH's 2,048 are
genuinely new (it was built but never wired in). The other ~6,000 are not yet explained and should be
before the 83% figure is quoted as progress.

⚠ **The pair-diff still cannot see any of this** — 100% of executed writes match pairs in `fwd4.txt`
both before and after, because our generators reproduce EOS's values exactly. Only the file size and
`GENW` measure the change.

## ⛔ RESOLVED: the +8,046 was my own redundant generator

`fm6000_ffubstinit` — the first thing I wrote this session — is **entirely redundant**.

```
ffuinit addrs : 8680
ffubst addrs  : 8046
overlap       : 8046          <- a complete subset
values        : IDENTICAL on every overlapping address
```

`fm6000_ffuinit` already generated every address FFU-BST covers, with the same values. FFU-BST only
ever *appeared* to remove 8,046 lines because it ran **after** `ffuinit`'s splice and undid part of
it. Once `ffuinit` moved to `gen_drop`, FFU-BST's own `gen_drop` removed **nothing** — visible in the
log as `FFU-BST dropped from the replay (168348 …)` with the count unchanged — while still applying
8,046 writes directly. That duplicate application was the entire +8,046.

Unwired from the boot path (kept in the tree as a standalone tool). The total returned to **exactly
130,966**, which is the confirmation.

### And it corrects the coverage figures I reported earlier

With FFU-BST double-counted, "ours" was inflated by 8,046 and the un-generated figure was too low:

| | reported | correct |
|---|---|---|
| ours | 115,874 (87%) | **107,828 (82%)** |
| not ours | 15,092 | **23,138** |

⚠ **The 87% and the "15,092 remaining" I quoted in the status are wrong.** The real position is 82%
generated with 23,138 writes still EOS's. Every earlier claim that leaned on 15,092 — including
"~7,400 of the remainder is documented-hard" as a fraction — needs re-reading against 23,138.

### What was actually gained

Stripping this back, the genuine wins this session are:

```
373,345  working replay at session start
130,966  after the SPICO firmware left the replay      (-90,006, real)
 93,145  after 10 write-once blocks moved to gen_drop  (-37,821, real)
```

The file is **93,145 lines, down 75%**, and 45,867 of those writes now go to the chip by direct MMIO
from our tools rather than out of a file. Both ports clean-locked, 14 routes, transit forwarding on
alpha27.

**FFU-BST contributed nothing to that.** The BST default-fill analysis was correct about the data and
useless as work, because I never checked whether an existing generator already covered those
addresses. `ffuinit -a | comm` would have answered it in one command, before any of it was written.

## The accurate target list: 18,161 writes emitted by no generator

Measured at PAIR level — every tool's `-n` output collected (86,824 distinct pairs) and subtracted
from the executed file. This is the list that matters, and it is the first one built after checking
generator overlap rather than assuming it.

| block | writes | note |
|---|---:|---|
| **L3AR** | 3,841 | `l3arinit` covers **slice 0 only**; slices 1-4 (csGlort, policers, storm control, L3 QoS) were deliberately left in the replay |
| **FFU** | 3,821 | `ffuinit` covers the BST half; `SLICE_CAM`, scenario and the commit strobes are uncovered |
| JSS | 3,722 | SerDes lane config — board-measured, same category as the firmware |
| L2L_SWEEPER | 1,305 | |
| PARSER | 1,164 | |
| MOD | 979 | |
| CM | 792 | |
| MONITOR | 639 | |
| MAPPER | 621 | |
| HASH, SSCHED, ESCHED, … | ~1,300 | tails |

**L3AR and FFU are the two worth taking next**, at ~3,800 each, and both have an existing generator to
extend rather than a new one to write:

- **L3AR** — extend `l3ar_program.py`/`fm6000_l3arinit` from slice 0 to slices 1-4. The geometry is
  documented and hardware-validated for slice 0 (`L3AR_CAM(slice, rule, seg, word)` at `0x10000`,
  5 slices x 32 rules), so this is scope extension, not decoding.
- **FFU** — the CAM half. ⚠ `ffu_decode.py` warns the CAM entries are paired with the `0x3f0000`
  commit strobe and "separating entries from their strobe is what broke the first FFU attempt", so
  this one is not a `gen_drop` candidate and probably not order-insensitive either.

⚠ **Check overlap before writing anything.** `for t in fm6000_*; do ./$t -a; done | sort -u` against
the target addresses would have shown FFU-BST was a subset of `ffuinit` before a line was written.
That check now takes one command and is mandatory.

### Soak: alpha27 (FFU-BST removed, metric corrected)

4 boots: Et2 clean-lock and forwarding on 3 of 4, Et1 4/4 — the same copper rate as every other
build, so removing FFU-BST changed nothing functionally, as expected for a redundant generator.

```
executed replay   93,145
ours              107,828 of 130,966 total   (82%)
not ours           23,138   (18,161 measured at pair level)
```

## `drop_range`: deleting configuration EdgeNOS does not use — L3AR CAM slices 1-4

Not every remaining write needs generating. `l3ar_program.py`'s scope note says EOS's 153 L3AR rules
cover VXLAN, MPLS, tap aggregation and routed multicast that **EdgeNOS does not implement**, and the
checklist records slices 1-4 as csGlort, policers, storm control and L3 QoS. So the question was not
"how do we author these" but "are they load-bearing at all".

`L3AR_CAM(slice, rule, seg, word) = 0x10000 + 0x200*slice`, so slices 1-4 are `0x10200-0x109ff` —
exactly the un-generated CAM pages, **2,224 writes**. `drop_range` deletes them with nothing behind
them.

Overlap checked first, as the FFU-BST lesson requires: **only `l3arinit` touches L3AR at all**.

Result on alpha29:

```
[fs]   L3AR-CAM-slices1-4 range dropped (164614 writes remain, nothing replaces it)
[fs]   provenance: 107828 of 128466 executed writes come from our generators (83%)
executed replay: 90,645
```

Both ports clean-locked, 14 routes, **transit forwarding**, unicast ping to the switch 0% loss.

### ⛔ A silent failure worth recording

The first version used `awk ... strtonum("0x"$1)`. On the switch it **did nothing at all and printed
no error** — busybox awk answers `Call to undefined function`, the filter passed everything through,
and the only symptom was a provenance total that had not moved. `strtonum` is a gawk extension.

I had already hit this exact error earlier in the same session, running `regmap.py`'s awk locally, and
did not carry it forward. The replay's addresses are fixed-width lowercase hex, so a plain **string**
comparison orders them correctly and needs no numeric conversion.

⚠ **Anything that filters the replay on the switch must avoid gawk extensions**, and must be checked
by the count it reports rather than by exit status — this one "succeeded" while doing nothing.

### Soak: alpha29 (L3AR CAM slices 1-4 deleted)

4 boots — Et2 clean-lock and transit forwarding on 3 of 4, Et1 4/4. Same copper rate as every build
before the deletion, so removing csGlort/policers/storm-control/L3-QoS changed nothing observable on
the paths EdgeNOS uses.

⚠ **What this soak does and does not cover.** It exercises L2 switching, IPv4 routing, OSPF and
transit. It does **not** exercise the features those slices configure, because EdgeNOS does not
implement them — that is the premise of the deletion, and it means the soak cannot fail for the
reason someone would most want it to. If EdgeNOS later implements policing, storm control or QoS,
this deletion has to be revisited, not assumed still safe.

### Running total

```
373,345  replay at session start
 90,645  executed now                      -76%
107,828  ours of 128,466 total             83%
```

## ⛔⛔ RETRACTED: the L3AR slice deletion. The datasheet says the premise is wrong

**The datasheet was never consulted for this, and it answers it directly.** §5.10.1, L3AR:

```
Total number of rules: 160
Total number of TCAM slice sets: 5
Number of rules per precedence set: 32
Number of serial application stages: 5
```

and, on the action semantics:

> *"Changes accumulate serially from one application stage to the next."*
> *"…any changes applied by this action in a given slice is not visible to the TCAM keys of
> downstream slices."*

**The five slices are serial stages of one resolution, not five independent feature blocks.**
Deleting slices 1-4 removes four fifths of L3 action resolution.

The claim I acted on — "slices 1-4 are csGlort assignment, policers, storm control and L3 QoS:
separate functions EdgeNOS does not author" — appears in `l3ar_program.py`'s C template and in
`FEATURE-COMPLETE-CHECKLIST.md` **with no citation**. The datasheet is cited elsewhere in that same
toolchain (Table 5-30 for the `ACTION_FLAGS` width), so the omission was specific to this claim.

### What the two experiments actually showed

| build | deletion | result |
|---|---|---|
| alpha29 | CAM slices 1-4 (2,224) | forwarded fine, 4 boots — **this is what made the wrong premise look right** |
| alpha30 | CAM + action RAM1-5 slices 1-4 (+1,089) | **dataplane dead**: both ports clean-locked, OSPF 2 routes, unicast to the switch 100% loss |

alpha30's signature is the same as the documented L2ARSEQ failure — links up, ports fine, unicast
gone. Reverted; alpha31 restores 93,145 executed, transit forwarding, ping 0% loss.

### ⚠ And it reframes alpha29, which "worked"

Deleting a slice's CAM **writes** does not disable the slice — it leaves those TCAM entries
**uninitialised**. `Key=0, KeyInvert=0` is the FM6000 never-match state and has to be *written*.
`l3ar_program.py` knows this: it emits explicit zero writes for the rules it does not author,
precisely so EOS's rules cannot remain resident. alpha29 forwarded because EOS's action RAM was still
in place behind whatever those uninitialised entries matched — not because the slices were unused.

**To disable a slice, write never-match keys. Do not omit the writes.**

### The rule this establishes

`drop_range` deletion needs a **documented** reason that the block is unused, not an uncited comment
in our own source. Two of the three inputs I used — the checklist line and the tool's template — were
the same unsourced claim restated, which read like corroboration and was not.

**Check the datasheet before deleting.** It is at
`arista-reverse-engineering/ethernet-switch-fm5000-fm6000-datasheet.pdf`, `pdftotext -layout` makes
it greppable, and the answer here took one search.

## FFU: 4,522 writes are final-value liftable — grounded in the datasheet this time

`fm6000_ffuinit.c` deliberately emits only the **8,680 registers written exactly once**. Its header
explains why the rest are excluded: a first version wrote the whole end state, links came up and
unicast worked, but **OSPF never formed (routes stayed at 2)** because collapsing `0x3f0000`'s 59
writes performed one commit instead of 59 and the CPU-punt traps were never applied.

That reasoning is sound but the exclusion is far broader than it needs to be. The datasheet (§5.7.13,
Atomic Modifications) names **exactly** which registers are shadowed:

```
CAM slices : FFU_SLICE_MASTER_VALID
BST slices : FFU_BST_MASTER_VALID, FFU_BST_SCENARIO_VALID,
             FFU_BST_PARTITION_MAP, FFU_BST_ROOT_KEYS[0..15]
control    : FFU_ATOMIC_APPLY
```

> *"Each write into these registers is stored into write-only shadow registers and remains in shadow
> until software commands those registers to be written into the active configuration."*

**The CAM and BST entries themselves are not shadowed.** Only the enable/commit registers are. So the
2,480 `SLICE_CAM` entries, the action words and the scenario CAMs are ordinary writes.

Measured against the current replay, with a **conservative superset** kept (the whole BST config page
and the whole slice config page, because the datasheet names two shadow registers the register header
does not define):

```
FFU multi-write registers            1,963   (5,869 writes)
  shadow/strobe superset -> KEEP       345   (1,347 writes)
  final-value liftable               1,618   (4,522 writes)
  net line saving                              2,904
```

The known second strobe, `0x33c09f` (7 writes alternating `0xf1e`/`0xe1e`), falls inside
`FFU_BST_ROOT_KEYS` — which is in the datasheet's shadow list, so the superset already covers it. My
first pass missed it by testing only `MASTER_VALID` and `ATOMIC_APPLY`; that is why the superset is
drawn at page granularity rather than per-register.

### ⛔ Blocker: the generator script is missing

`fm6000_ffuinit.c` says *"GENERATED by asic/fm6000/tools/gen_ffuinit.py -- do not edit by hand"*, and
**that script is not in the tree**. The C file cannot currently be regenerated, so extending FFU means
recreating the generator first.

### ⚠ And it is a Goal A / Goal B trade, not a free win

A tool that reads the replay and emits final values **relocates EOS's values into our source** — Goal
B progress, Goal A regression. `ffuinit` already works exactly this way, so the precedent exists, but
it should be a deliberate decision rather than a side effect of chasing line count.

### Soak: alpha31 — the current best build

4 boots, Et2 clean-lock 4/4, transit 3/4, Et1 4/4. This is the build to keep:

```
/mnt/flash/fwd4.txt          283,339 lines   (SPICO stripped)
executed replay               93,145
ours                         107,828 of 130,966   (82%)
firmware                     fm6000_spico_code.bin via STEP5b
L3AR slice deletion          DISABLED (premise disproven)
```

## Where the remaining 15,661 sits, and what each piece needs

| block | writes | what it needs |
|---|---:|---|
| FFU | 3,821 | recreate `gen_ffuinit.py` (missing), then lift 4,522 multi-write registers by final value, keeping the datasheet's shadow set. **Goal A/B trade.** |
| JSS | 3,722 | SerDes lane tuning — board-measured, not a table |
| L3AR | 1,619 | author slices 1-4. They are **serial stages**, so this is real work, not a lift |
| L2L_SWEEPER | 1,305 | MAC-table aging: 2 timers, 16 TCAM rules, documented §5.11.9 |
| PARSER / MOD / CM / MONITOR / MAPPER | 4,195 | tails of blocks whose generators already run |
| HASH, SSCHED, ESCHED, SAF, POLICERS, ALU | ~1,000 | small |

**Nothing left is a cheap win.** Every remaining piece needs either authoring, a policy decision about
relocating EOS values into our source, or board measurement. The write-once and firmware opportunities
are spent.

## ✅ Recovered the missing `gen_ffuinit.py`

`fm6000_ffuinit.c` says *"GENERATED by asic/fm6000/tools/gen_ffuinit.py -- do not edit by hand"* and
that script was **not in the tree**, so the file could not be regenerated. Recovered by deriving the
rule from the C file's own contents and proving it reproduces them:

```
rule: emit every FFU-region register (0x300000-0x3fffff) the replay writes EXACTLY ONCE

  existing C table                     8,680 pairs
  generated from the working replay    8,680   identical=True
  generated from the stock repo replay 8,680   identical=True
```

Both replays produce the same set, which is a useful side result: **the FFU write-once set did not
change between the stock capture and the spliced working file.**

The tool carries a `--lift-multi` flag implementing the datasheet-grounded extension — final-value
lifting of multi-write registers minus the shadow superset — and **does not enable it**:

```
$ gen_ffuinit.py <replay> --lift-multi
# 10298 FFU writes (8680 write-once, +1618 lifted multi-write)
```

⚠ It is left off deliberately. It shrinks the replay by ~2,904 lines while moving more of EOS's
values into our source: a **Goal A / Goal B trade** that should be decided, not drifted into. The
flag exists so the experiment is one command away when that decision is made.

### Why this was worth doing before the extension

A generator that cannot regenerate its own output is not a generator, it is a checked-in artifact
with a comment. Recovering the rule *and verifying it against the artifact* is what makes the
extension safe to attempt at all — without it there would be no way to tell a correct extension from
a rewrite.

## Target survey after L3AR completion (2026-08-21, alpha36)

With all five L3AR slices authored, the remaining replay content was re-measured rather than
re-quoted. Method: compile every generator, union their `-a` address lists, subtract from the
replay's distinct addresses, then classify the remainder by block using
`asic/fm6000/tools/sdk_regmap.py`.

That reproduces the previously documented figures independently — FFU 3,813 (doc said 3,821),
SBUS/JSS 3,721 (3,722), L2L 1,357 (1,305) — which is a good sign the older numbers were sound.

**But it also surfaces a much larger target the old list did not name: the CM watermarks.**

| register | writes | distinct addrs |
|---|---:|---:|
| `CM_PORT_TXMP_PRIVATE_WM` | 3,898 | 1,280 |
| `CM_PORT_TXMP_HOG_WM` | 3,152 | 1,280 |
| `CM_PORT_RXMP_PRIVATE_WM` | 2,472 | 912 |
| `CM_PORT_RXMP_PAUSE_ON_WM` | 2,460 | 912 |
| `CM_PORT_RXMP_PAUSE_OFF_WM` | 2,460 | 912 |
| `CM_PORT_RXMP_HOG_WM` | 2,432 | 1,216 |
| `CM_PAUSE_CFG` | 1,984 | 304 |
| `CM_TC_PC_MAP` | 1,400 | 152 |

**~20,000 writes**, and `fm6000_cmminit` covers only **72 addresses** — the watermark tables are
essentially untouched. (`fm6000_cminit` emits nothing under `-a`.)

### Why this is the right next target

**The values are formulaic.** `CM_PORT_TXMP_PRIVATE_WM` spans 1,280 addresses and uses **six
distinct values** — `00008004` (477x), `00003fff` (352x), `00008000` (322x), `00000000` (64x),
`00000007` (54x), `00008014` (11x). `CM_PAUSE_CFG` uses five across 304 addresses. These are
congestion-management watermarks: constants applied across a port x traffic-class matrix,
derived from buffer sizing — not microcode. That is exactly the shape `gen_ffuinit` and
`fm6000_ffubstinit` already exploit, and unlike JSS lane tuning it is **not** board-measured.

⚠ Contrast with **SBUS/JSS (3,721 writes)**, which is board-measured SerDes lane tuning and
should stay in the replay — reproducing it from a formula is not possible and copying it would
be transcription with no benefit.

### L2L sweeper: smaller than it looks

`L2L_SWEEPER` is 1,357 writes but only **144 distinct addresses** — 80 in `SWEEPER_CAM`
(20 entries x 4 words, of which only the 10 odd entries carry content) and 30 in `SWEEPER_RAM`.
The rest of the writes are repeats through the indirect port (`SWEEPER_WRITE_COMMAND` /
`_WRITE_DATA`, `IP`/`IM`). Authoring it removes ~144 addresses' worth of table, not 1,357 writes.
Worth doing eventually; not the best next move.

### Recommended order

1. ~~CM watermarks~~ — **DONE, alpha37.** 6,512 writes, byte-verified both directions,
   provenance 84.8% -> 89.8%. Note the survey's "~20,000 writes" counted repeats in the raw
   replay; the tables hold 6,512 distinct entries and that is what moved.
2. FFU remainder (3,813) — `FFU_SLICE_MASTER_VALID` is 3,368 of it, 778 distinct.
3. L2L sweeper (144 addresses).
4. PARSER / MOD remainder.
5. **Not** JSS/SBUS — board-measured, leave in the replay.

## The FFU remainder is the `--lift-multi` decision, quantified (2026-08-21)

Chasing "FFU remainder, 3,813 writes" as the next authoring target ran into the fact that it is
not an authoring target at all — the implementation already exists and what is missing is a
policy decision.

`gen_ffuinit` emits FFU registers written **exactly once**. The remainder are multi-write
registers, and its `--lift-multi` flag emits those by **final value**, minus a conservative
shadow superset. Measured on the current replay:

| | |
|---|---:|
| liftable registers (multi-write, not shadowed) | **1,618** |
| replay writes they currently cost | 4,522 |
| after lifting, one write each | 1,618 |
| **net reduction in executed writes** | **2,904** |
| shadowed registers correctly left in the replay | 345 regs / 1,347 writes |

### Why it is a trade and not a win

`gen_ffuinit`'s own docstring is explicit: *"the values are EOS's, which is what 'relocated, not
authored' means for this block."* Lifting moves 1,618 EOS-derived values out of `fwd4.txt` and
into our binary.

- **Goal B gains** 2,904 fewer executed writes, and 1,618 fewer addresses depending on the
  replay file.
- **Goal A does not gain.** The values are still EOS's. Nothing is authored from intent, unlike
  L3AR slices 1-4 or the CM watermarks, where the *structure* is ours.

That is the whole substance of the decision, and it is why the flag has stayed off.

⚠ The shadow exclusion is load-bearing and must stay. `FFU_SLICE_MASTER_VALID`,
`FFU_BST_MASTER_VALID` and `FFU_ATOMIC_APPLY` are **shadowed** (datasheet 5.7.13): writes land
in a shadow copy and take effect only on an atomic apply. `FFU_ATOMIC_APPLY` is a single address
written **59 times** in the replay — it is the commit strobe after each batch. Lifting a
shadowed register to its final value without the commit protocol would not reproduce the
programming sequence. `shadow()` already excludes them at page granularity, and 345 registers /
1,347 writes stay in the replay for exactly this reason.

### Recommendation

Take it **only if Goal B (no EOS file executed at boot) is the binding constraint**.

★ **DECIDED 2026-08-21: do not enable it.** The goal is a switch programmed from our own
understanding of the silicon, so authorability outranks write count. `--lift-multi` moves EOS's
values without adding understanding; the remaining authorable work — L2L sweeper (144
addresses), PARSER/MOD remainder — does. Same reasoning keeps JSS/SBUS lane tuning in the
replay: it is board-measured and cannot be authored from anything.

## ★ Definitive blob inventory, measured from what the switch executes (2026-08-21, alpha37)

Earlier surveys counted the *raw* replay (283k lines). This one reads
`/mnt/flash/fwd-executed.txt` — the file fullseq actually produces and replays after every
generator, drop and splice has run — and subtracts the union of every generator's `-a` output.

**The install reads exactly two EOS-derived files at boot:**

| file | size | status |
|---|---|---|
| `/mnt/flash/fwd4.txt` -> `fwd-executed.txt` | 1.67 MB executed | 92,851 lines, of which **23,775 writes / 5,730 addresses are still EOS's** |
| `/mnt/flash/fm6000_spico_code.bin` | 12 KB | Intel SerDes firmware. **Not authorable** — it is a firmware image, and stripping it costs 10GBASE-CR (Et2 linked 0/7 boots without it) |

`ucode_l2.raw` (546 KB) and `ucode_tail.raw` (165 KB) are still on flash but **STEP4 is SKIPPED**
— the log says so every boot. They are dead weight and can be deleted from the install today.

### What the 23,775 remaining writes are

| block | writes | addrs | verdict |
|---|---:|---:|---|
| **MAPPER** | 6,283 | 565 | **largest authorable target** |
| FFU | 3,813 | 935 | the `--lift-multi` decision — declined, relocation not authoring |
| **SBUS** | 3,721 | **4** | ⛔ do not touch: 4 addresses = the JSS indirect port (`0xF001/2/3`). These are *operations*, plus board-measured lane tuning |
| ERL | 1,934 | 967 | unexamined |
| L2L | 1,357 | 144 | analysed 2026-08-21, `docs/L2L-SWEEPER.md`; blocked on the CAM field layout |
| PARSER | 1,164 | 304 | unexamined |
| CM | 1,005 | 701 | remainder after the alpha37 watermarks |
| MOD | 979 | 306 | unexamined |
| ESCHED | 841 | 171 | unexamined |
| L2F | 570 | 570 | unexamined |
| L3AR | 512 | 450 | **remainder despite all five slices** — the profile tables at `0x11c00`-`0x120df`; our generators emit only the entries their rules reference |
| SAF / HASH / SSCHED | 944 | 320 | unexamined |

### What "removing the blob" actually requires now

Two of the four largest items are **not** removable by authoring:

- **SBUS (3,721)** is 4 addresses — the SerDes indirect port. Those writes are transactions, not
  configuration. Removing them means performing the SerDes bring-up ourselves, which is the port-3
  work (`docs/PORT3-BRINGUP.md`), not a table to generate.
- **FFU (3,813)** is the declined relocation.

So the authorable remainder is about **16,000 writes**, and the order by size is:
**MAPPER (6,283) -> ERL (1,934) -> L2L (1,357) -> PARSER (1,164) -> CM remainder (1,005) ->
MOD (979) -> ESCHED (841) -> L2F (570) -> L3AR profile tables (512)**.

⚠ Note the L3AR line. All five slices are authored and the block still contributes 512 writes,
because the slices' generators emit only the profile-table entries their own rules select. The
tables are shared (slice 2 selects csGlort profile 5, which slice 1 also emits). Completing L3AR
means authoring the 19 profile tables as a unit — a small, well-understood job, since their
field layouts are already recovered in `docs/L3AR-STRUCTURE.md`.

## ★ How to finish, measured at alpha40 (2026-08-21)

`fwd-executed.txt` is now 87,071 lines. **16,980 writes across 4,715 addresses are still EOS's.**
Method as before: union every generator's `-a`, subtract from what the switch actually executes.

| block | writes | addrs | writes/addr | category |
|---|---:|---:|---:|---|
| FFU | 3,813 | 935 | 4.1 | **declined** — `--lift-multi`, relocation not authoring |
| SBUS | 3,721 | **4** | **930** | **operations** — the JSS indirect port |
| ERL | 1,934 | 967 | 2.0 | authorable |
| L2L | 1,357 | 144 | 9.4 | authorable (sweeper; `docs/L2L-SWEEPER.md`) |
| PARSER | 1,164 | 304 | 3.8 | authorable |
| CM | 1,005 | 701 | 1.4 | authorable (remainder after watermarks) |
| MOD | 979 | 306 | 3.2 | authorable |
| ESCHED | 841 | 171 | 4.9 | authorable |
| L2F | 570 | 570 | **1.0** | authorable, write-once — easiest in the list |
| SAF | 339 | 168 | 2.0 | authorable |
| HASH | 314 | 100 | 3.1 | authorable |
| SSCHED | 291 | 52 | 5.6 | authorable |
| CRM | 259 | **2** | **130** | **operations** — the CRM command interface |
| POLICER / ALU / LBS | 214 | 211 | 1.0 | authorable, write-once |

### The writes-per-address column is the tell

A block with ~1 write per address is a **table**: read it, name the fields, emit it. A block with
hundreds of writes to a couple of addresses is an **indirect port** — the writes are transactions,
and "authoring" them means performing the operations, not generating a table.

- **SBUS: 4 addresses, 930 writes each.** This is `0xF001/2/3`, the JSS SerDes port. Removing
  these means doing the SerDes bring-up ourselves — which is exactly the port-3 work in
  `docs/PORT3-BRINGUP.md`, not a generator. **The dead port and 3,721 blob writes are the same
  problem.**
- **CRM: 2 addresses, 130 writes each.** The memory-fill command interface, which `fm6000_memfill`
  already replaces; `fullseq` has a `CRMDROP` path for it.

### The finish line

| | writes | how |
|---|---:|---|
| authorable, ~10 more generators | **~9,000** | same method as L3AR / CM / MAPPER |
| SBUS | 3,721 | finish the SerDes bring-up (port 3) |
| FFU | 3,813 | `--lift-multi` — declined on principle |
| CRM | 259 | already droppable via `CRMDROP` |

Authoring the ~9,000 takes provenance from **94.8% to roughly 99%**. After that the replay
contains only the FFU relocation we declined, the SBus transactions, and CRM — at which point
"remove the blob" stops being a table-authoring problem and becomes: **implement SerDes bring-up,
and decide the FFU question.**

**Order of work** (easiest first, all small): ~~L2F (570, write-once) -> POLICER/ALU/LBS (214,
write-once) -> SSCHED (291)~~ **DONE in alpha42** -> HASH (314) -> SAF (339) -> ESCHED (841) ->
MOD (979) -> CM remainder (1,005) -> PARSER (1,164) -> L2L sweeper (1,357) -> ERL (1,934).

★★ **THE BOOT CAN NOW RUN WITHOUT THE VENDOR FILE -- AND IT LINKS BUT DOES NOT
FORWARD** (measured 2026-08-22, alpha55).

Until now the entire bring-up was a TRANSFORMATION of `fwd4.txt`: `gen_list`
filters our addresses out of the replay and splices our writes into its line
stream. The generators were substitutions inside somebody else's sequence, so
"98.6% of executed writes are ours" never meant the switch could boot without the
file -- remove it and `init-m1` skipped the dataplane entirely.

`fm6000-fullseq.sh` now has a **STANDALONE** mode (`STANDALONE=1`, or automatic
when no replay is on flash) that runs all 41 generators directly by MMIO, in the
order the splices already implied. `init-m1` attempts it instead of giving up.

**Result of a boot with BOTH `fwd4.txt` and `fwd5.txt` absent:**

    STANDALONE: ran 41 generators directly (0 non-zero, 0 absent)
    final       et1=000008c0/00000940   et2=00000815/00000000
    post-spico  et1=00000cc0/00000940   et2=000008c0/00000940   <- BOTH PORTS UP

    edgenos-up.sh: kernel routes=5 (not 39), et1 rx=0
    transit: 0 frames

So **the link layer is fully ours** -- EPL, SerDes, PCS, both ports to clean lock
with no vendor file anywhere -- **but forwarding is not**. No OSPF adjacency, no
punted packets, nothing transits.

**That settles a question this project could not previously answer.** The residual
~1,800 writes are NOT merely runtime state that the hardware would regenerate;
some of them are load-bearing for the forwarding path. The optimistic reading --
"the sweeper runs itself, the bitmaps accumulate, so the residual does not matter"
-- is now disproved for forwarding, while confirmed for link.

**ANSWERED 2026-08-22, and the answer is not "which writes" -- it is ORDER.**

`make_residual()` extracts every replay line no generator covers: **46,611 writes,
839 KB against the replay's 5.1 MB**. A boot with no replay at all, running the 41
generators and then applying that residual, gives:

    STANDALONE: ran 41 generators directly (0 non-zero, 0 absent)
    STANDALONE: applied residual.txt (46611 writes) rc=0
    post-spico  et1=000008c0/00000940  et2=000008c0/00000940   BOTH PORTS UP

    edgenos-up.sh: kernel routes=5 (not 39), et1 rx=0
    transit: 0 frames

**Generators + residual is the COMPLETE write set** -- by construction, since the
residual is defined as everything the generators do not cover. Every write the
replay performs is performed. It still does not forward.

So the missing ingredient is not content. It is **sequence**: in a normal boot our
writes are spliced INTO the vendor's stream at specific positions, and in
standalone they are batched -- all 41 generators, then all 46,611 residual writes.
That is a different order, and the forwarding path does not survive it.

This is consistent with everything else this project has learned the hard way:
`gen_list_early` exists precisely because the CM watermarks landing at the loop
end instead of the block's first write caused measurable loss; ERL and
CM_PAUSE_CFG are two-phase; SSCHED is a token-push protocol. Ordering has been
load-bearing at every level, and it is load-bearing here too.

**What a replay-free dataplane actually needs is therefore a SEQUENCE MODEL** --
knowing not just what each block's writes are but where they must fall relative
to the others. The current architecture gets that ordering for free by borrowing
the vendor's stream. Recovering it independently is the remaining work, and it is
a different kind of problem from authoring tables.

**What is already established:** the LINK layer needs no vendor file at all --
both ports reach clean lock from our generators alone.

### What the residual actually IS (2026-08-22)

Characterising all 46,611 residual writes by block settles what kind of problem
this is:

| block | writes | addrs | span (position in the replay) |
|---|---:|---:|---|
| **SAF** | **34,668** | 168 | 2,895 -> 41,938 |
| SBUS | 3,721 | **4** | **1 -> 46,611** |
| FFU | 3,545 | 909 | 2,286 -> 46,125 |
| L2L | 1,357 | 144 | 3,202 -> 45,204 |
| PARSER | 970 | 110 | 2,342 -> 32,864 |
| MOD | 771 | 202 | 2,452 -> 43,942 |

**74% of it is SAF_MATRIX**, and the heaviest addresses are **idx 20 and idx 40 --
et2 and et1** (the physical-to-logical mapping recovered from
PARSER_INIT_FIELDS) -- **218 writes each**. That is the store-and-forward /
cut-through matrix being rewritten again and again as the two live ports change
state. SBUS is 4 addresses spanning the ENTIRE boot, position 1 to 46,611: the
SerDes indirect port, in use throughout.

**So the residual is not configuration applied at a moment. It is the switch
REACTING TO ITS OWN BRING-UP** -- SAF recomputed as links come up, the SerDes
port driven continuously, the L2L sweeper aging, FFU updated as state changes.

That is why batching it fails, and it is a stronger statement than "we need a
sequence model": **you cannot replay a conversation as a monologue.** Reproducing
it means implementing the logic that PRODUCES those updates -- a port bring-up
state machine that maintains SAF, drives the SerDes, and services the sweeper --
not recording and replaying what that logic once emitted.

### SAF_MATRIX authored -- 74% of the residual gone, and it did NOT fix forwarding

`gen_safmatrix.py` emits the matrix's **converged** state as a rule. Verified
168/168 against the replay, and the effect on the residual is exactly as
predicted:

    residual before: 46,611 writes
    residual after:  11,943 writes      (-74%)
    provenance:      123,068 / 124,533 = 98.8%

The rule is small because the matrix is a **port bitmap** and its converged state
has only four values:

    ports 0, 2        all bits
    port 1            bits 0..81
    ports 3, 20, 40   {0,1,2}                 the cages with a transceiver
    other 50 front    {0,1,2,3} | {20,40}     CPU/mgmt ports + the linked ports

⚠ **And a standalone boot with the reduced residual STILL does not forward** --
both ports reach 0x940, `kernel routes=5`, `et1 rx=0`, transit 0. So SAF was the
BULK of the residual, not the BLOCKER. Removing 34,668 writes changed the volume
and nothing else.

That is worth stating plainly because it kills the tempting inference: the
forwarding gap is not proportional to how much of the replay is left. Shrinking
the residual further, by itself, is not evidence of getting closer.

### The ordering question is now CLOSED: fine interleaving is required

Four standalone configurations were tested, all with both replays absent and the
COMPLETE write set available in one form or another:

| configuration | ports | forwarding |
|---|---|---|
| generators only | both 0x940 | routes 5, rx 0, transit 0 |
| generators, then the full residual (11,943) | both 0x940 | routes 5, rx 0, transit 0 |
| generators, then FFU-only residual | both 0x940 | routes 5, rx 0, transit 0 |
| **residual FIRST, then generators** | both 0x940 | routes 5, rx 0, transit 0 |

⚠ **A subset bisect was started and abandoned as logically void**: the FULL
residual already fails, and every subset is contained in it, so no subset can
succeed. Bisecting only makes sense when the whole set works and you want the
minimum. That mistake is recorded because it is an easy one to repeat.

**Neither coarse order forwards.** So the interleaving the vendor stream provides
is finer-grained than "all generators then the rest" or "the rest then all
generators" -- it is not a matter of picking the right batch order. That closes
the ordering question and leaves only one honest reading:

**Reproducing the bring-up requires driving it as a sequence of interdependent
steps -- a state machine -- not applying a set of writes in some order.**

The symptom is consistently `et1 rx=0`: nothing is punted to the CPU, so OSPF
never hears a hello, so only 5 kernel routes appear. The link is perfect and the
CPU path is dead. That is where a state-machine effort should start.

**Which reframes the remaining work.** The table-authoring seam is essentially
exhausted and was never going to remove the file. What would remove it is a
bring-up state machine. That is a substantial piece of new software, not a
continuation of the generator programme, and it should be scoped as such.


⚠ Restoring either replay file returns the box to normal (39 routes, transit 6/6),
so this mode is safe to experiment with.

★ **RANK BY WRITE-ONCE ADDRESSES, NOT BY WRITE COUNT** (measured 2026-08-21).
Ranking the remainder by size is misleading, because each block's existing
generator already took its write-once addresses — so a small remainder is small
*precisely because the easy part is gone*. Measured over the uncovered set:

| block | addrs | writes | multi-write | >1 distinct | monotonic | **write-once (authorable)** |
|---|---:|---:|---:|---:|---:|---:|
| FFU | 935 | 3813 | 935 | 587 | 114 | **0** |
| SBUS | 4 | 3721 | 4 | 3 | 1 | **0** |
| ERL | 967 | 1934 | 967 | 636 | 0 | **0** |
| L2L | 144 | 1357 | 144 | 107 | 6 | **0** |
| PARSER | 304 | 1164 | 110 | 110 | 109 | **194** |
| CM | 701 | 1005 | 304 | 156 | 0 | **397** |
| MOD | 306 | 979 | 306 | 113 | 66 | **0** |
| ESCHED | 171 | 841 | 67 | 65 | 0 | **104** |
| SAF | 168 | 339 | 168 | 101 | 100 | **0** |
| HASH | 100 | 314 | 100 | 34 | 31 | **0** |

So the real order is **CM (397) -> PARSER (194) -> ESCHED (104)**, and SAF/HASH —
which an earlier note recommended as "the smallest, best next targets" — are the
**worst**: nothing in them is write-once. A `gen_list` splice there would collapse
accumulating state to its final value, the alpha41 `INIT_TOKEN` mistake again.

The `monotonic` column says the repeats are bits-only-added, i.e. a bitmap
accumulated as ports come up (SAF 100 of 101, PARSER 109 of 110). That is runtime
state, and the rule from `RX_SLOW_PORT[1..4]` applies: **do not claim the address
at all**, or the splice deletes every update. ERL is different again — 636 distinct
with **zero** monotonic, so its repeats are changing values, not accumulation.

**alpha42 closed the first three** as `gen_smalltables.py`: 957 writes over 829 addresses,
provenance 94.8% -> 95.6%. Two lessons that apply to everything still on this list:

1. **Get the geometry from the SDK, not from clustering.** `sdk_regmap.py` now recovers
   dimensions, words-per-entry and both axis strides for all 703 registers, so a table's
   shape no longer has to be guessed from which addresses happen to be written. An entry's
   pitch is `pow2ceil(words)`, NOT `words` -- L2F holds 3-word entries on a 4-word pitch.
   Decompose every address into (register, index, word) and require **zero residue**; that
   is what turns "these bytes reproduce EOS" into "this is what the table is".

2. **`--verify` alone is not sufficient, and `gen_list` is write-once ONLY.** Byte-comparing
   against the final-state image cannot see a collapsed write sequence, because the last
   value is the same either way. alpha41 shipped `SSCHED_INIT_TOKEN` -- a port that pushes
   one token per write -- collapsed from 64 writes to 1. Every future generator must also
   check **write counts per address** (`--counts`): any address the replay writes N times
   with N distinct values is a port or a strobe and must keep its sequence, and any address
   the replay keeps updating at runtime (e.g. `RX_SLOW_PORT[1..4]`) must not be claimed at
   all, or `gen_list` will splice those updates away.

### Free today, no authoring needed

`ucode_l2.raw` (546 KB) and `ucode_tail.raw` (165 KB) sit on `/mnt/flash` but **STEP4 is SKIPPED**
every boot. Deleting them removes 710 KB of EOS microcode from the install at zero risk.

### Not removable

`fm6000_spico_code.bin` (12 KB) — Intel SerDes firmware, 6,000 x 10-bit instructions, needed for
10GBASE-CR. See `docs/SPICO-RE.md`: the SDK itself does not know its ISA. It is labelled, not
pretended away.
