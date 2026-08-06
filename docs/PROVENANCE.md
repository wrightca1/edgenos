# Provenance and Distribution — FM6000 (Arista DCS-7150S-52) port

**Status: 2026-08-06.** This document records, for every artifact the FM6000 cold bring-up
depends on, where it came from and whether EdgeNOS may distribute it — and what we must write
ourselves to replace the parts we may not.

Short version: **one artifact blocks distribution** — the 299,803-write register replay set
(`fwd5.txt`), because it embeds Intel's FM6000 microcode verbatim. Everything else is either
already ours, now removed, or proven unnecessary.

---

## 1. Rule

EdgeNOS is our own NOS. EOS and the FocalPoint SDK are used **only as oracles** — to observe what
the silicon needs — never as a runtime dependency and never as a source of shipped bytes.
Observing a system to learn how to interoperate with hardware is one thing; **redistributing the
bytes we observed is another**, and it is the second one that this document polices.

Accordingly:

> No file in this repository may contain third-party firmware, microcode, or a verbatim
> transcription of a proprietary program's data tables. Register *addresses*, *field layouts*, and
> *hardware behaviour* are facts about the chip and are fine. Somebody else's *code* is not.

---

## 1.1 Was the FocalPoint SDK used?

Worth stating precisely, because the answer is "no" and "yes" depending on what is being asked.

**At runtime: no.** The working cold sequence is five binaries we wrote — `fm6000_coldreplay`,
`fm6000_initsbus`, `fm6000_memfill`, `fm6000_ucode_dbg`, `fm6000_fullreplay`. Nothing links or
`dlopen`s `libFocalpointSDK.so`. With the SPICO firmware now dropped (§3.1), the only third-party
code that executes anywhere is the FM6000 microcode on the chip itself.

**As a reverse-engineering source: yes.** The SDK was disassembled to learn what the silicon
needs. Files whose *behaviour* was recovered that way:

| file | SDK symbol |
|---|---|
| `fm6000_initsbus.c` *(working path)* | `fm6000InitSBus @0x478a1f`, txn engine `@0x477c54` |
| `fm6000_memfill.c` *(working path)* | CRM fill sequence, SDK-mined |
| `fm6000_spico.c` | `fm6000LoadSpicoCode @0x4793a1`, `fm6000InterruptSpico @0x47935a` |
| `fm6000_ucode.c` | `fmPlatformLoadMicrocode`, `fm6000WriteSBus @0x479e09` |
| `fm6000_bist.c` | `fm6000BistMemoryInit @0x34bb94` |
| `fm6000_mrl.c` | `fm6000MrlRegisterFix @0x47a4bc` |
| `fm6000_boot.c`, `fm6000_sched.c`, `fm6000_serdes_enable.c`, `fm6000_regs.h` | various |

**Nothing in this repository loads the SDK.** `tools/fpdshim/` used to — it `dlopen`ed
`libFocalpointSDK.so` to drive the vendor SDK from our own userspace, as an alternative to
hand-replaying the forwarding plane. **It has been removed (2026-08-06).** It was never on a
runtime path, never referenced by any Makefile, and never actually worked: it stalls at
`fmPlatformConfig`, a 6,708-byte zero-filled `.bss` object, so `numSwitches=0` and the CSR
accessors fault. The replay route solved the problem instead, and EdgeNOS's direction is now to
*generate* its own configuration rather than delegate to the vendor SDK — so the SDK-delegation
route is a dead end by choice as well as by outcome. Its RE findings are preserved privately in
`arista/notes/analysis/fm6000-fpdshim-sdk-route-ABANDONED.md`.

**Terminology.** These files previously described themselves as "clean-room". That was inaccurate
and has been corrected throughout. Clean-room implies a two-team separation in which the
implementer never sees the original; that is not what happened. What happened is
disassembly-derived reimplementation for hardware interoperability — a well-established practice,
but it should be labelled as what it is. The headers now say so.

## 2. Inventory

### 2.1 Blocking — cannot be distributed

| Artifact | Size | Origin | Required? |
|---|---|---|---|
| `fwd5.txt` replay set | 299,803 writes | transcription of EOS's cold boot | **yes — the datapath does not work without it** |
| `ucode_l2.raw`, `ucode_tail.raw` | 710,406 B | FM6000 microcode, from the EOS image | yes (also embedded inside `fwd5.txt`) |

These live on the private GitLab reference repo and on the switch's flash. They are **not** in this
tree and are blocked by `.gitignore`.

**What is actually inside the replay** (measured, not estimated):

| content | writes | % | distributable? |
|---|---:|---:|---|
| config, non-zero values | 184,005 | 47.2% | yes — facts about how to configure the chip |
| config, zero-fill (memory clear) | 84,999 | 21.8% | yes — we already generate this (`fm6000_memfill`) |
| **microcode: parser + L2AR + MOD** | **53,108** | **13.6%** | **no — Intel's program** |
| ~~SerDes SPICO firmware~~ | ~~90,006~~ | ~~23.1%~~ | **removed — see §3.1** |

So after removing the SPICO firmware, **17.7% of what remains (`fwd5.txt`, 299,803 writes) is
genuinely somebody else's code**; the other 82% is configuration we can generate ourselves.

Measured against `fwd5.txt` directly:

| content | writes | % |
|---|---:|---:|
| configuration, non-zero values | 169,537 | 56.5% |
| zero-fill (clearing uninitialised SRAM) | 73,439 | 24.5% |
| **microcode (parser/L2AR/MOD)** | **53,108** | **17.7%** |
| SBus SerDes tuning (firmware stripped) | 3,719 | 1.2% |

*(An earlier revision of this document put the microcode at 6.9%. That figure came from a
range-based filter that missed the L2AR block: `ucode_l2.raw` spans `0x100000`–`0x147100`, so L2AR
is part of the microcode load. Classifying by the exact address set of the microcode files gives
the correct figure above.)*

### 2.2 Removed from this repository (2026-08-06)

| File | Why |
|---|---|
| `asic/fm6000/fm6000_mrl_table.h` | 6,287-entry table lifted verbatim from `libFocalpointSDK.so`. Replaced with our own runtime loader (below); **not used by the working sequence at all.** |
| `asic/fm6000/fm6000_i2c_bringup`, `fm6000_crm`, `fm6000_wr128` + 8 more in `build/.../payload/` | compiled ELF binaries committed by accident; all have tracked source and build from a clean checkout |
| `tools/fpdshim/` | SDK-delegation route: `dlopen`ed `libFocalpointSDK.so`. Never worked, never on a runtime path, strategically superseded. Findings preserved privately. |

None of these had ever been pushed, so they never entered public history. `.gitignore` now blocks
the whole class (`*_spico_code.bin`, `ucode_*.raw`, `fwd*.txt`, `*mrl_table.h`, the ELF tools).

**Done (2026-08-06).** The 45 unpushed commits were filtered with `git filter-branch` to strip 31
paths from every commit — ~90 MB of build artifacts (20 initramfs images, `m1-warm-bzImage`, 10
payload ELF tools) plus `fm6000_mrl_table.h`. History is otherwise intact. Verified: zero stripped
paths reachable, zero tracked binaries, and a fresh `--no-hardlinks` clone is **1.1 MB** across all
202 commits. The pushed base was already clean, so no blob ever reached GitHub. Original history is
kept locally on `backup/pre-cleanup-20260806`.

### 2.3 Retained — reviewed and judged distributable

| File | Basis |
|---|---|
| `asic/fm6000/fm6000_regs.h` | Register block bases and offsets. These are *facts about the hardware*, independently confirmed against the public FM5000/FM6000 datasheet and live BAR reads. Not a copy of Intel's header. |
| `asic/fm6000/fm6000_coldreplay.c` | ~272 ops. This is a *bring-up procedure* (clock enables, `BOOT_CTRL` commands 1/2/3, BIST, scheduler init) that follows datasheet Table 4-1. Ours. |
| `asic/fm6000/fm6000_spico.c` | Our own implementation of the SBus/SPICO **protocol**. Contains no firmware. Now off the critical path but still valid code. |
| all other `asic/fm6000/*.c` | our own tools |

### 2.4 Board data — needs its own derivation

`asic/fm6000/fm6000_serdes_ports.h` (52 ports × polarity/drive/pre/post) was transcribed from
Arista's `CotatiP4.fdl`. Lane polarity and pre-emphasis are properties of *this board's PCB
traces*, so the values are facts — but the table as a whole is Arista's engineering work.

It is **not on the critical path** (only `fm6000_serdes.c` uses it; the working sequence gets its
SerDes settings from the replay). Plan: derive polarity empirically (invert, observe whether the
lane trains) and keep drive/pre/post at datasheet defaults, tuning only where a link fails. That
yields a table that is ours by measurement.

---

## 3. What we proved, so we don't have to replace it

### 3.1 The Intel SPICO SerDes firmware is not required — dropped

The single largest blob was the 12,000-byte SPICO SerDes-microcontroller firmware. It is **gone
from the runtime**, on evidence rather than assumption.

Method: the SBus command word encodes `receiver` and `register`, so the firmware upload separates
cleanly from real SerDes tuning. Of 31,241 SBus transactions in the replay, **30,002 (96%) are
IMEM upload** (receiver `0xFD`, registers `0x04`–`0x07`; 6,000 words). Stripping exactly those and
cold-booting three times gave:

| replay variant | link | datapath |
|---|---|---|
| SPICO firmware **and** microcode stripped | up — `0x8c0`, `pcsRx=1` | **dead: 0 TX, 0 RX, ping 100% loss** |
| SPICO firmware stripped, microcode kept | up — `0x8c0`, `pcsRx=1` | **39 frames TX, 29 RX, ICMP 8/8, 0% loss** |

So the SerDes trains and forwards with no Intel firmware on the chip. That is 90,006 writes and a
whole firmware blob eliminated, and it removes the one dependency we could not realistically have
reimplemented.

**Caveat, stated honestly:** validated on one short SR fibre link to an AS5610. The SPICO drives RX
equaliser adaptation, so longer or lossier media may still need an adaptation strategy. If that
day comes the answer is our own equaliser control loop over SBus, not Intel's firmware.

### 3.2 The microcode *is* required

The same experiment proves the converse: with the microcode stripped the link still trains but the
chip forwards nothing. Frames are dropped before egress — TX descriptors complete (`DONE=30`) and
zero frames reach the wire. The microcode is the real dependency.

---

## 4. Replacing the microcode — the one remaining job

This is tractable, and much more so than "reimplement a microcode blob" suggests, because the
FM6000 "microcode" is **not an opaque instruction stream**. It is a set of TCAM + action-SRAM
tables whose structure and encoding are publicly documented.

Datasheet §5.5.1: the parser is an iterative state machine, unrolled into slices. Each slice takes
the next 4-byte frame word plus the previous slice's 32-bit state, forms a 64-bit key, looks it up
in a TCAM, and applies the resulting action (update state, set flags, map frame bytes to the
88-byte output bus).

The register layout matches that description exactly, and matches the addresses we measured:

```
PARSER_CAM(slice, entry, word) = 0x100000 + 0x400*slice + 4*entry        128 entries x 28 slices
PARSER_RAM(slice, entry, word) = 0x100000 + 0x400*slice + 4*entry + 0x200   (the action SRAM)
PARSER_INIT_STATE              = 0x108000    76 entries (per logical port)
PARSER_INIT_FIELDS             = 0x108200
```

Our measured microcode footprint — 34,089 words in 747 regions — decomposes as:

| block | base | words | what it is |
|---|---|---:|---|
| block | base | writes | distinct words | what it is |
|---|---|---:|---:|---|
| PARSER | `0x100000` | 22,246 | 17,616 | per-slice CAM/RAM pairs, `0x200` apart — paired regions growing 180 → 468 entries as parsing deepens. **Only 2,117 populated CAM entries.** |
| L2AR | `0x140000` | 26,376 | 12,467 | L2 action-resolution TCAM/action — **the largest block by write count** |
| MOD | `0x150000` | 4,204 | 3,768 | ~50 small per-modification routines (64–460 words each), e.g. egress tag strip |

And the action encoding is documented: **Table 5-3 "Parser Action SRAM Encoding"**, with
Table 5-4 (header flags), Table 5-5 (fixed field mapping) and Table 5-6 (action flags). The CAM
entry format (`KeyInvert[63:0]`, `Key[127:64]`) is confirmed by live reads.

So the work is: **write a parser-program generator** — a small assembler that takes a declarative
protocol description (Ethernet / VLAN / IPv4 / IPv6 / TCP / UDP / our F64 tag) and emits CAM+RAM
entries per slice, plus MOD routines for egress tag strip. That is our own code, from the public
datasheet, and it replaces the last non-distributable dependency.

### 4.1 The RE is already working — the parser is fully legible

We decoded Intel's parser tables directly, which confirms both the format and the feasibility.

The CAM is a standard ternary match over the 64-bit key: a bit must be 1 where `Key=1, KeyInvert=0`,
must be 0 where `Key=0, KeyInvert=1`, and is don't-care where both are 1. Applying that to real
entries:

```
slice 4, entry 3:  Key=0xff00ff3a00010800   KeyInvert=0xffffffc5fffef7ff
                   low 16 bits fully specified -> EtherType == 0x0800   (IPv4)
slice 3, entry 3:  Key=0xffffff1288f7ffff   KeyInvert=0xfdffffed7708ffff
                                            -> EtherType == 0x88F7      (PTP/1588)
```

Sweeping all 28 slices for fully-specified 16-bit matches recovers the whole protocol set — and
the slice spread is exactly what an unrolled state machine should look like, since a given protocol
lands at different slices depending on how many VLAN tags precede it:

| EtherType | protocol | entries | slices |
|---|---|---:|---|
| `0x8100` | VLAN C-tag | 84 | 3–12 |
| `0x88a8` | VLAN S-tag | 84 | 3–12 |
| `0x0800` | IPv4 | 39 | 3–20 |
| `0x86dd` | IPv6 | 20 | 3–20 |
| `0x0806` | ARP | 20 | 3–20 |
| `0x8906` | FCoE | 18 | 3–20 |
| `0x88f7` | PTP/1588 | 12 | 3–14 |

**The entire parser is only 2,117 populated CAM entries.** It is not 34,089 words of dense code —
that figure counts the action SRAM and init tables too. A generator that walks slices and emits
"match protocol P at depth D" rules reproduces this from a few dozen lines of declarative input.

This is the definition of a clean rewrite: we understand the format from the public datasheet, we
can read what the existing tables do, and we emit our own tables that implement the protocols *we*
choose to support — which for EdgeNOS is a much shorter list than Intel's (no FCoE, no PTP
initially).

### Priority

| # | Task | Effect |
|---|---|---|
| 1 | Parser program generator (CAM/RAM from a protocol description) | 22,246 writes; best-understood block, 2,117 CAM entries |
| 2 | L2AR action-table generator | 26,376 writes — the largest block; do it alongside the parser |
| 3 | MOD routine generator — start with egress F64 tag strip | 4,204 writes; smallest, and the tag strip is the piece we already need |
| 4 | Config generator to replace the remaining 93% of the replay | removes `fwd5.txt` entirely; we already understand GLORT/dmask/SAF/CM/EPL/`EPL_CFG_B` |
| 5 | Derive `fm6000_serdes_ports.h` by measurement | removes the last Arista-derived data |

Items 1–3 are the licence-blocking ones. Item 4 is a large but purely mechanical job and is not a
distribution blocker in the same way — configuration values are facts about how the chip must be
set up, not a copied work — but generating it removes the last need to ship a 5 MB trace.

---

## 5. Reference material (private, never in this repo)

Held in the private GitLab repo `arista/notes/reference/`:

- `fm6000_api_regs_int.h` — marked INTEL CONFIDENTIAL. Used for **name/field confirmation only**;
  every offset we rely on is independently derivable from the datasheet and live reads.
- `fm5000-fm6000-datasheet.txt` — Intel document 331496-002. Public-ish but not ours to bundle.
- `scd-dumps/*.txt.gz` — our register traces of EOS.
- `private-data/fm6000-mrl-table.txt` — the MRL table, now runtime-loaded rather than compiled in.
