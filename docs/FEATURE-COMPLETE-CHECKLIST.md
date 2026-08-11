# What EdgeNOS still needs to be feature-complete on our own code

Scope: running the 7150 with **no file and no table derived from EOS** — not in the image, not in
the replay, not transcribed into our source. Status as of 2026-08-10.

The percentages people quote (93.5% of the replay eliminated) measure the wrong thing on their
own, because a block can leave `fwd4.txt` and still be Intel's program — it just moves into a `.c`
file. This list counts what is *ours*.

---

## A. Microcode transcribed into our source — the licence blocker

`docs/PROVENANCE.md` §1 forbids "a verbatim transcription of a proprietary program's data tables".
§2.5 records that 18,332 non-trivial microcode pairs are in tracked source anyway. Per file:

| file | pairs | status |
|---|---:|---|
| `fm6000_parserinit.c` | 16,960 | ✅ **replaceable now** — `parser_program.py` cold-boot validated |
| `fm6000_l2arpre.c` | 12,473 | ❌ not started |
| `fm6000_l2arseq.c` | 12,473 | ❌ not started (same rules, different placement) |
| `fm6000_l3arinit.c` | 3,928 | ❌ not started |
| `fm6000_modinit.c` | 3,626 | ❌ not started |
| `fm6000_l2arinit.c` | 1,878 | ❌ not started |
| 9 smaller files | ~1,600 | ❌ not started |

- [ ] **A1. Delete `fm6000_parserinit.c`** and its `fm6000.mk` entry; make the boot path use the
      generated program. Everything for this is done and validated — this is bookkeeping.
- [x] ~~**A2a. L2AR decode.**~~ Done. Geometry, action RAM and the 384-bit key layout all from
      `FM6000_L2AR_CAM_*` in the register header; decoder and encoder in
      `asic/fm6000/tools/l2ar_{decode,gen}.py`, encoder round-trips EOS's 2,442 segments and 407
      actions bit-identically.
- [ ] **A2b. L2AR generator.** Blocked on a second decode, not on format: the actions reference
      `DMT_PROFILE`, `SetCpuCode` and `SetMirror`, which are indices into tables configured
      elsewhere. Authoring "trap to CPU" means knowing which CPU-code entry that is.
      `--keymap` shows the shape: `ACTION_FLAGS` in 346 of 407 rules, `SMASK` in 103, and ten key
      fields never constrained at all.
- [x] ~~**A3a. L3AR decode.**~~ Geometry and key confirmed. `L3AR_CAM(slice, rule, seg, word)` at
      **`0x10000`** — 5 slices × 32 rules × 4 segments, 256-bit key; `RAM1` at `0x11200`, `RAM2` at
      `0x11400`. Key layout matches datasheet Table 5-30 exactly. Validated: RAM words are 2× the
      declared rule counts on every slice, 64/64/64/64/50 against 32/32/32/32/25.
- [ ] **A3b. L3AR decoder/generator.** Ordinary work now; no unknowns in the format.
- [x] ~~**A4a. MOD decode.**~~ Done. 32 profiles × 32 steps at `0x158000`, `COMMAND_RAM` at
      `0x159000`, `VALUE_RAM` at `0x159400`; 48-bit key from `FM6000_MOD_CAM_KEYS`. Decoder in
      `asic/fm6000/tools/mod_decode.py`; 684 of 1024 steps populated.
- [ ] **A4b. MOD generator.** The smallest block and the least entangled — the best next target.
- [ ] **A5. The 9 smaller files.** Mostly incidental overlap; likely fall out of A2–A4.

## B. `fwd4.txt` — the last operator-supplied file

`fm6000-fullseq.sh` reads exactly one EOS file now (the microcode load was dropped 2026-08-08).

- [ ] **B1. Group 2 — CAMs, 5,364 writes.** Entry↔strobe pairing. The partition is solved: every
      FFU write belongs to exactly one of 59 `0x3f0000` strobe groups, nothing dangles.
      Generator not written.
- [ ] **B2. Group 3 — ~3,700 "unnamed" writes.** Identified as microcode (they are the L2AR pages),
      so this closes with A2, not separately.
- [ ] **B3. Group 6 — `MAPPER_SRC_PORT_TABLE`, 634 writes.** Per-case work, lowest priority.
- [ ] **B4. The rest of the MMIO residue** — ~17k writes of control that is load-bearing where it
      sits. May not be liftable without reimplementing bring-up rather than relocating its trace.

## C. SPICO — the one piece that cannot be generated

- [x] Proven droppable for **fibre**: Et1 (10GBASE-SR) trains and forwards with all 30,002 IMEM
      transactions stripped.
- [ ] **C1. Decide the copper answer.** Et2 is intermittent with *and* without SPICO, so its role
      is unproven either way. Until that is settled, "zero proprietary files" is honest only for a
      fibre-only build.
- [ ] **C2. If copper needs it** — an equaliser control loop of our own over SBus, not Intel's
      firmware. Large, and nobody has scoped it.

## D. Correctness gaps in what we have already shipped

- [ ] **D1. IPv6 on hardware.** Parser handles it and simulates clean, but no IPv6 has ever crossed
      the link. Needs an address on et1 and a peer.
- [ ] **D2. VLAN-tagged traffic on hardware.** Same — simulated only.
- [ ] **D3. ARP path diffs.** `L3_Mcst`/`L3_Bcst` set by us and not by EOS on a broadcast DMAC.
      Ours looks *more* correct, which usually means something is misunderstood. Outside the FFU
      scenario key, so not urgent, but not understood either.
- [ ] **D4. L4 port extraction on protocols that have none** (ch24/ch25 written as `0x0000` where
      EOS leaves the init value). Benign as far as anyone has shown.
- [ ] **D5. The ping defect.** Ping collapses to 100% loss within ~3 minutes **on EOS's own
      parser and the stock replay too**. Predates all of this. It is the largest open defect on the
      platform and nobody has root-caused it — suspicion is the portd DMA ring, not the ASIC.

## E. Build and packaging

- [ ] **E1. Ship an image that omits the microcode generators.** Proven by hand today: a SWI is a
      zip containing a gzip cpio initrd, so removing a binary and repacking takes about a minute.
      Needs folding into `build-m1-rootfs.sh` rather than done manually.
- [ ] **E2. Boot without an operator replay at all.** Currently `init-m1` gates dataplane bring-up
      on `/mnt/flash/fwd4.txt` existing. That gate is the honest marker of B being unfinished.
- [ ] **E3. Provenance audit in CI.** §2.5 was found by hand and had been wrong for months. Diff
      every generated table against `fm6000Microcode.raw` before it can be committed.

---

## The honest summary

| | |
|---|---|
| **done** | parser — decoded, generated, cold-boot validated, no EOS table involved |
| **decoded, generator outstanding** | L2AR, L3AR, MOD — all four blocks are now structurally understood |
| **best next target** | MOD (A4b): smallest, least entangled with the rest of the pipeline |
| **blocked on a second decode** | L2AR (A2b) — needs DMT profile and CPU-code table semantics |
| **needs a decision, not code** | copper/SPICO (C1) |
| **not ours, still broken** | the ping defect (D5) |

Every remaining block's format came out of `fm6000_api_regs_int.h` in minutes once we stopped
inferring it. The structural phase is finished; what is left is authoring and testing.

One block of five is done. It took the whole toolchain — decoder, encoder, simulator, differential
oracle against EOS, and a one-minute hardware loop — and that toolchain transfers directly to L2AR,
L3AR and MOD. The second block should be much faster than the first; the first paid for the tools.
