# What EdgeNOS still needs to be feature-complete on our own code

Scope: running the 7150 with **no file and no table derived from EOS** — not in the image, not in
the replay, not transcribed into our source. Status 2026-08-10.

The percentages quoted elsewhere (93.5% of the replay eliminated) measure the wrong thing on their
own: a block can leave `fwd4.txt` and still be Intel's program — it just moves into a `.c` file.
This list counts what is **ours**.

---

## Where the four microcode blocks stand

| block | file(s) | pairs | decoder | encoder | generator |
|---|---|---:|:--:|:--:|---|
| **parser** | `fm6000_parserinit.c` | 16,960 | ✓ | ✓ | ✅ **shipped, cold-boot validated** |
| **L2AR** | `fm6000_l2ar{pre,seq,init}.c` | 26,824 | ✓ | ✓ | blocked — see A2 |
| **MOD** | `fm6000_modinit.c` | 3,626 | ✓ | ✓ | one unknown — see A4 |
| **L3AR** | `fm6000_l3arinit.c` | 3,928 | — | — | format known |

All four formats came out of `fm6000_api_regs_int.h`. The structural phase is finished.

### Tooling built (all read images at runtime, embed nothing)

```
parser_decode.py   decode the parser TCAM + action SRAM
gen_parser.py      parser encoder, --verify round-trips EOS's 2,117 entries
parser_program.py  author a parser; --c emits fm6000_parserinit.c, --splice a replay
parser_sim.py      execute a program against a frame; --diff two programs
l2ar_decode.py     decode L2AR rules with names and actions
l2ar_gen.py        L2AR encoder, --verify 2,442 segments + 407 actions; --keymap
mod_decode.py      decode MOD command/value slices
mod_gen.py         MOD encoder, --verify 684 CAM + 369 cmd + 329 value; --keymap
```

---

## A. Microcode transcribed into our source — the licence blocker

- [x] ~~**A1. parser.**~~ **Done.** `fm6000_parserinit.c` is generated from
      `parser_program.py`: 1,568 writes replacing 16,960 transcribed pairs. Of 1,568, only one
      non-trivial pair coincides with EOS's microcode — a collision on a common value, not a
      transcription. Cold-boot validated from an image with the old generator deleted.
- [ ] **A2. L2AR generator.** Format fully known; blocked on a **second decode**, not on encoding.
      Its actions index `DMT_PROFILE`, `SetCpuCode` and `SetMirror` — tables configured elsewhere,
      so authoring "trap to CPU" means knowing which CPU-code entry that is. `--keymap` shows the
      shape: `ACTION_FLAGS` in 346 of 407 rules, `SMASK` in 103, ten key fields never constrained.
- [x] ~~**A3. L3AR generator.**~~ **Done, hardware-validated.** `l3ar_program.py` authors 13 rules
      as named intents; `--c` emits 640 writes (242 non-zero) replacing 3,928 transcribed pairs.
      All 13 match EOS exactly. On the 7150: full slice-0 replacement forwards at 0% loss over
      et1, 640/640 readback, routes 34. Scope is **slice 0 only** — slices 1-4 are csGlort,
      policers, storm control and L3 QoS, left in the replay.
      ⚠ 242 of 242 non-zero writes coincide with EOS, against 1 of 1,568 for the parser. L3AR has
      almost no encoding freedom, so agreement is expected and is NOT the evidence of
      independence it was for the parser. Audit the process, not the diff.
- [ ] ~~**A3-old. L3AR decoder + generator.**~~ Mechanical now: `L3AR_CAM(slice, rule, seg, word)` at
      **`0x10000`**, 5 slices × 32 rules × 4 segments, 256-bit key matching Table 5-30; `RAM1`
      `0x11200`, `RAM2` `0x11400`. Validated — RAM words are 2× the declared rule counts on every
      slice (64/64/64/64/50 vs 32/32/32/32/25).
- [ ] **A4. MOD generator.** One unknown left: the 8-bit `Command` packs an opcode **and** its
      operand, and that split is not in the datasheet sections read. EOS uses 47 distinct values.
      Each command's required value-byte count is documented, which constrains the split — a step's
      value words must match its opcode's arity. Tractable without more archaeology.
- [ ] **A5. The 9 smaller files** (~1,600 pairs). Incidental overlap; should fall out of A2–A4.

## B. `fwd4.txt` — the last operator-supplied file

- [ ] **B1. FFU CAM strobe pairing** (5,364 writes). Partition solved: every FFU write belongs to
      exactly one of 59 `0x3f0000` strobe groups, nothing dangles. Generator not written.
- [ ] **B2. ~~Group 3~~** — closed. Those "unnamed" writes at `0x010000` are **L3AR**, so this
      completes with A3.
- [ ] **B3. `MAPPER_SRC_PORT_TABLE`** (634 writes). Per-case, lowest priority.
- [ ] **B4. MMIO residue** (~17k writes) — control that is load-bearing where it sits. May not be
      liftable without reimplementing bring-up rather than relocating its trace.

## C. SPICO — the one piece that cannot be generated

- [x] Droppable for **fibre**: Et1 trains and forwards with all 30,002 IMEM transactions stripped.
- [ ] **C1. Decide copper.** Et2 is intermittent with *and* without SPICO. Until settled, "zero
      proprietary files" is honest only for a fibre-only build.
- [ ] **C2. If copper needs it** — our own equaliser loop over SBus. Large, unscoped.

## D. Correctness gaps in what already ships

- [ ] **D1. IPv6 on hardware.** Parser handles it, simulates clean against EOS, never crossed the
      wire. Needs an address on et1 and a peer.
- [ ] **D2. VLAN-tagged traffic on hardware.** Same — simulated only.
- [ ] **D3. ARP path diffs.** We set `L3_Mcst`/`L3_Bcst` on a broadcast DMAC where EOS does not.
      Ours looks *more* correct, which usually means something is misunderstood. Outside the FFU
      scenario key.
- [ ] **D4. L4 ports extracted for protocols that have none** (`ch24/25` written `0x0000`).
- [ ] **D5. The ping defect.** Ping collapses to 100% loss within ~3 minutes **on EOS's own parser
      and the stock replay too**. Predates all of this, still unroot-caused; suspicion is the portd
      DMA ring. **Route count, not ping, is the reliable signal** — a lesson learned the hard way.

## E. Build and packaging

- [ ] **E1. Fold the image edit into the build.** Proven by hand: a SWI is a zip containing a gzip
      cpio initrd, so removing a binary and repacking takes about a minute (`zip -X -0`). Needs to
      live in `build-m1-rootfs.sh`.
- [ ] **E2. Boot with no operator replay.** `init-m1` gates dataplane bring-up on
      `/mnt/flash/fwd4.txt` existing — the honest marker that B is unfinished.
- [ ] **E3. Provenance check in CI.** §2.5 was found by hand after months. Diff every generated
      table against `fm6000Microcode.raw` before it can be committed.

---

## Hardware state as left

```
box            10.1.1.77, cold-booted on OUR parser, forwarding
parser         0x100c01 = 0x94ffffeb (ours)      routes 34, ARP resolved
/mnt/flash     fwd4.txt = our spliced replay (373,345 writes)
               fwd4-stock.txt = stock (389,809), md5 0c31f84de104f9e10dce12cddb4d5540
               edgenos-ourparser.swi = image with fm6000_parserinit removed
boot-config    SWI=flash:/EOS-4.16.8M.swi
```

⚠ `boot-config` has self-reverted to EOS. That is `init-m1`'s deliberate brick-proof net, not a
fault: **every EdgeNOS boot must be re-armed.** To boot our parser again, write
`SWI=flash:/edgenos-ourparser.swi` and reboot.

Console is the gateway `smiley@10.22.1.56`, `/dev/ttyUSB2` @ 9600 8N1 — **but the USB numbering
moves whenever that box reboots. Probe, do not trust the number.** `ttyUSB0` is the AS5610,
`ttyUSB1` the 7050SX2.

---

## The method, which is the transferable part

Six times this session a plausible inference about bit or field order was wrong, and six times
`fm6000_api_regs_int.h` had the answer in minutes:

| inferred | truth |
|---|---|
| parser action layout = Table 5-3 order, LSB-first | refuted |
| parser action layout = scan for a 6-bit field → bit 45 | bit 38 |
| CAM match priority = first match wins | last match wins — *and the datasheet says so, in the MOD section* |
| L2AR key = Table 5-71 order | refuted |
| L2AR key anchored on `DMASK_A` at 0–75 | that is `SMASK`; `DMASK_A` is a separate CAM |
| L3AR lives at `0x158000` | that is MOD; L3AR is `0x10000` |

**Check the header first.** And three times a passing test hid a real defect, each time by
asserting the wrong invariant:

- two different wrong parser action layouts both round-tripped 2,117/2,117
- a truncated `VAL_LAYOUT` passed a self-test that only checked the *key* layout
- a wrong value-bank mapping passed a round-trip that only checked word *contents*

A round-trip proves self-consistency, never interpretation. Only an external fact settles
interpretation — the register header, EOS's own program, or the silicon.

**The fourth wrong invariant, and the worst.** L3AR's `read_rule` treated an all-`0xFFFFFFFF`
CAM as empty. All-ones is Key=1/KeyInvert=1 on every bit — *don't-care everywhere*, the
**universal match**. Rule 0 of slices 0-3 is exactly that: the default rule carrying the baseline
flag resolution and `LoopbackSuppress`. We decoded the most important rule in every slice as
blank. It survived because:

- the declared count is 32 rules/slice and we decoded 31 — and the "RAM words are 2× the rule
  count" check was **recorded as passing** while 64 RAM words sat against 31 rules
- the mask baseline looked like a statistical mode over 149 rules; it is not a mode, it is rule
  0's action applied to every frame
- it made the LoopbackSuppress naming asymmetry look backwards: the default sets it ON, so
  `With…` rules need do nothing and `Without…` rules must actively clear it

Only hardware found it: replacing slice 0 without rule 0 gave 100% packet loss, and leave-one-out
over all 20 unauthored rules showed rule 0 was the only one that mattered.

**And a blind spot in `--diff` itself.** It compares the rules we author. It cannot see the rules
we *delete*, so it read 12/12 green while the program did not forward. A generator that replaces
a table must be tested against what it removes, not only against what it writes.

**Ping is not useless — but it needs a control.** D5 makes ping fail for unrelated reasons, so a
bare failure proves nothing. Alternating our program against EOS's in the same minute (A/B/C/A)
turned it into a reliable signal: EOS 0%, ours-overlaid 0%, ours-with-deletions 100%, EOS 0%.
