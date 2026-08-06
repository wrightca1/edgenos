# Making the release whole: replacing the replay set and the microcode

**2026-08-06.** Two operator-supplied files stand between the alpha and a self-contained release:
`fwd4.txt` (the register replay) and the FM6000 microcode. This is what is actually in them, and
what it takes to generate each part ourselves.

Measured with `asic/fm6000/tools/replay_classify.py`, not estimated.

---

## What is actually in the replay

`fwd4.txt` — 389,809 writes, classified with the fill-vs-config split described below:

| category | writes | share | replaced by | status |
|---|---:|---:|---|---|
| **CONFIG** | 237,024 | 60.8% | generator from a declarative platform description | to build |
| **SPICO_FW** | 90,006 | 23.1% | operator-supplied, or dropped | third-party |
| **MICROCODE** | 53,108 | 13.6% | generator from a protocol description | to build |
| **ZEROFILL** | 5,954 | 1.5% | `fm6000_memfill` | ✅ done |
| **SBUS_TUNE** | 3,717 | 1.0% | per-lane board measurement | to build |

**Only 1.5% is already covered by our own code** — not the 18.8% first assumed. 23.1% is Intel's
firmware. The remaining **75.4% is configuration and microcode**: facts about how to set the chip
up, not somebody else's program, and therefore generatable.

### ⚠ How that 18.8% became 1.5% — an assumption tested and killed

The obvious first win looked like dropping the 73,440 zero-writes, on the assumption that
`fm6000_memfill` already covers them. Built exactly that replay and cold-booted it:

| replay | link | packets to the far end |
|---|---|---|
| `fwd4` (full), same boot | up `0x8c0` | **+60** |
| all zero-writes removed | up `0x8c0` | **+0 — no forwarding** |

The link still trains, so this is not SerDes: something in the forwarding path depends on those
writes. The flaw was in the classification, not the chip — **a write of `0` to a *control register*
is configuration, not table fill.**

Splitting on run length settles it. A zero-write is fill only if it is part of a run of consecutive
addresses cleared back-to-back (`RUN_MIN = 8`):

```
zero writes                          73,440
  in runs >= 8 (genuine table fill)   5,954   (8%)
  isolated / short runs (real config) 67,486  (92%)
  44,682 runs total, longest 448
```

**92% of the zero-writes are configuration.** They cluster in exactly the blocks you would expect a
forwarding path to need: MA table, SAF, L2L, FFU, CM. The classifier now resolves them
automatically, which is why CONFIG is 60.8% rather than 43.5%.

### Where the CONFIG actually lives

| block | writes | notes |
|---|---:|---|
| MA_TABLE | 55,803 | L2 MAC table — large but highly regular |
| CM | 46,110 | congestion watermarks, mostly uniform values |
| SAF | 34,668 | store-and-forward matrix; 18-bit fields, format known |
| L2L | 24,620 | L2 lookup |
| EPL | 22,051 | per-port SerDes/PCS; `EPL_CFG_B` PcsSel decoded |
| LBS | 18,547 | load balancing |
| FFU | 14,549 | the FIB — **fully decoded** (`ROUTING-FIB.md`) |
| MAPPER / L3AR / HASH / CMM | ~14,400 | partly understood |

None of it is a program. The two largest blocks (MA table, CM) are regular table fill with
structure, which is the easiest kind of thing to generate.

## The three pieces, hardest last

### 1. SPICO firmware — 23.1%, and possibly deletable

The one piece we genuinely cannot regenerate: it is Intel's compiled code for the SerDes
microcontroller. But we may not need it.

Proven by cold-boot bisect: with all 30,002 IMEM transactions stripped, **Et1 (10GBASE-SR) trains
and forwards normally** — link `0x8c0`, 39 frames TX, 28 RX, ICMP 8/8. The open question is copper:
Et2 is intermittent *with or without* it, so its role there is unproven either way (see
`ET2-COPPER-LINK.md`).

**Action:** keep it operator-supplied and optional. If the Et2 investigation shows SPICO is
irrelevant to copper too, this 23.1% simply disappears from the problem. `spico_extract.py` already
reconstructs the image from any trace, so an operator with a licensed EOS can always produce it.

### 2. Microcode — 13.6%, and the interesting one

**This is not opaque code.** The FM6000 parser is a TCAM + action SRAM whose encoding is published
(datasheet §5.5.1 and Table 5-3), and we have already decoded real entries from Intel's own tables:

```
slice 4, entry 3:  Key=0xff00ff3a00010800  KeyInvert=0xffffffc5fffef7ff
                   -> exact ternary match on EtherType 0x0800 (IPv4)
```

Sweeping all 28 slices recovers the whole protocol set — IPv4, IPv6, ARP, VLAN C-tag/S-tag, FCoE,
PTP — and the spread across slices 3–20 is exactly what an unrolled state machine should look like,
since a protocol lands at a different slice depending on how many tags precede it.

| block | writes | distinct words | what it is |
|---|---:|---:|---|
| L2AR | 26,376 | 12,467 | L2 action resolution |
| PARSER | 22,246 | 17,616 | **only 2,117 populated CAM entries** |
| MOD | 4,204 | 3,768 | ~50 small egress-edit routines |

**The parser is 2,117 CAM entries, not 22,246 words of dense code** — the larger figure counts the
action SRAM and init tables. A generator taking a declarative protocol list and emitting
CAM/RAM pairs per slice reproduces that from a few dozen lines of input. And EdgeNOS needs a
*shorter* list than Intel's: no FCoE, no PTP initially.

Layout is known:
```
PARSER_CAM(slice,entry,word) = 0x100000 + 0x400*slice + 4*entry     128 entries x 28 slices
PARSER_RAM(slice,entry,word) = same + 0x200                          action SRAM
PARSER_INIT_STATE 0x108000 (76, per logical port) · PARSER_INIT_FIELDS 0x108200
```

**Action:** build `fm6000_ucode_gen`. Start with the parser (best understood), then MOD (smallest,
and the egress tag-strip is the piece we already depend on), then L2AR.

### 3. Configuration — 60.8%, the biggest but the least mysterious

The per-block breakdown is in *Where the CONFIG actually lives* above. Nothing there is a program:
it is table fill and register setup, much of it repetitive. MA_TABLE and CM alone are 102k writes,
and both are regular — a MAC table and a set of largely uniform watermark values.

The share is 60.8% rather than the 43.5% first measured because two thirds of the zero-writes turned
out to belong here (see the ⚠ section). That is a reclassification, not new work appearing: those
writes were always in the replay, they were merely filed under the wrong heading.

**Action:** build `fm6000_cfg_gen` emitting from a platform description (port map, GLORT
allocation, MTU, buffer profile). Do it **one block at a time**, replacing that block's writes in
the replay and re-testing — never all at once.

## The order that de-risks it

The value of this approach is that each step is independently testable on real hardware, and any
step that fails leaves the previous working state intact.

1. **Split the replay by category** — `replay_classify.py --dump CONFIG` already emits a per-category
   file. Rebuild `fwd4` from the parts and confirm the switch still comes up. This proves the
   classification is faithful before anything is generated.
2. ~~**Drop all zero-writes.**~~ **TESTED — breaks forwarding** (above). What *is* safe to drop is
   the 5,954 in long runs; the other 67,486 are config and must be generated like anything else.
3. **Generate one CONFIG block**, starting with FFU (fully decoded) or SAF (simple format).
   Replace, test, keep.
4. **Generate the parser microcode.** The big RE win.
5. **Generate MOD, then L2AR.**
6. **Decide SPICO** on the back of the Et2 investigation.

At each step the replay shrinks and the generated fraction grows, and the switch keeps working.

## What "whole" would mean

A release where the only inputs are our own source: no microcode, no replay, no clock data (already
achieved in alpha4), and no SPICO — or SPICO as an optional extra for media that needs it. At that
point the image is genuinely self-contained and the licensing question closes entirely.

## ★ A shortcut worth taking first: Intel published a BSD-licensed switch SDK

Before writing generators from scratch, note that **Intel Ethernet Switch (IES) software is public
and BSD-3-Clause licensed**: <https://github.com/andriymoroz/IES>. The headers carry
`Copyright (c) 2007-2014, Intel Corporation` — a range that spans the FM6000 era, not just its
FM10000 successor.

`include/api/fm_api_ffu.h` alone documents, under a permissive licence, exactly the things we could
not decode from register traces:

| IES structure | what it gives us |
|---|---|
| `fm_ffuSliceInfo` | slice config: `keyStart`, `keyEnd`, `actionEnd`, key selectors |
| `fm_ffuAction` / `fm_ffuActionType` | **the action encoding** — `ROUTE_ARP`, `ROUTE_LOGICAL_PORT`, `SET_FLAGS`, `SET_TRIGGER`, `SET_FIELDS` |
| `FM_FFU_MUX_SELECT_*` | 40+ packet-field → TCAM-key mappings |
| `FM_FFU_SCN_*` | scenario encodings (packet type × routing context) |
| `fm_policerState` | token-bucket rate limiting |

The **action array at `0x337xxx`** — two words per entry, listed as "fields not decoded" in
`ROUTING-FIB.md`, and the single thing blocking `fm6000_fibd` from being a general FIB — is very
likely described by `fm_ffuAction`. Also present: `fm_api_acl.h`, `fm_api_routing.h`,
`fm_api_regs.h`.

**Two caveats, both important.** IES targets the **FM10000**, a different chip: the *concepts and
structures* are shared Fulcrum architecture, but **register addresses will not transfer** and must
still be confirmed against our own traces. And BSD-3-Clause permits use with attribution — it does
not make Intel's *compiled firmware* redistributable, so SPICO and the microcode images are
unaffected.

**Why this matters beyond convenience:** today our register knowledge is cross-checked against
`fm6000_api_regs_int.h`, which is marked INTEL CONFIDENTIAL and lives only in the private notes
repo. A BSD-licensed reference covering the same architecture would let us re-derive that knowledge
from a source we can actually cite — improving the provenance story in `PROVENANCE.md`, not just
saving effort.

**Do this before building generators.** An afternoon reading IES could turn the FFU action array
from "undecoded" into "documented", which is the difference between slot-reuse and a real FIB.

## Honest assessment

- **ZEROFILL removal** — done as an experiment, and it only buys 1.5%. Not worth revisiting.
- **CONFIG generation** — the largest at 60.8%, but mostly mechanical. Weeks, incremental, low risk.
- **Parser microcode** — the genuinely interesting RE. Days to weeks, and the format is published.
- **L2AR/MOD microcode** — same technique, less documented. Unknown.
- **SPICO** — not regenerable, only droppable. Depends on the Et2 answer.
- **Reading IES first** — hours, and it may collapse several of the above.

Also public and worth checking: Intel's FM6000 documentation collection (the datasheet we already
have came from there), and Silicon Labs' ClockBuilder, which generates `.si5338` register maps from
input/output frequencies — so even the clock data could be produced from first principles rather
than copied, if we ever needed it again (alpha4 no longer does).

None of it is blocked on new discoveries. Every piece has a known format or a known experiment.
