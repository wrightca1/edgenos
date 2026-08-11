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
- [ ] **A4. MOD generator — BLOCKED ON A CAPTURE PATH, not on analysis.** The command split has
      five converging lines of evidence for `opcode[7:5]:operand[4:0]`, length = operand+1
      (see `mod_decode.py`), but 3 opcode bits give 8 slots for 9 documented commands and EOS
      never uses `0x60-0x7F`, so the data cannot choose between the remaining readings.
      **Settling it needs to observe emitted bytes, and the 7150 currently has no egress capture
      point:** one front port (et1), no tcpdump, peer `10.101.101.25` refuses SSH, no routed
      destination answers. A binary ping pass/fail cannot discriminate — every wrong encoding
      corrupts the frame, so "it broke" identifies nothing. Unblock by either (a) TX-mirror to
      the CPU port, which needs the mirror table and therefore lands in A2, (b) a second
      connected port, or (c) a capture host on the et1 segment. **Do not write the generator on
      the strength of the five clues.**
- [ ] ~~**A4-old. MOD generator.**~~ One unknown left: the 8-bit `Command` packs an opcode **and** its
      operand, and that split is not in the datasheet sections read. EOS uses 47 distinct values.
      Each command's required value-byte count is documented, which constrains the split — a step's
      value words must match its opcode's arity. Tractable without more archaeology.
- [ ] **A5. The 9 smaller files** (~1,600 pairs). Incidental overlap; should fall out of A2–A4.

## B. `fwd4.txt` — the last operator-supplied file

- [ ] **B1. FFU.** Decoder written (`ffu_decode.py`, `--verify` round-trips 127 CAM entries + 113
      action words). Measured from the replay — the FFU is **not** in `fm6000Microcode.raw` at all:
      14,490 writes in the region (5,960 CAM half, 8,450 BST half, 80 scenario), 59 `0x3f0000`
      strobes of which **43 commit CAM and 16 commit BST** — two independent commit domains.
      - The **BST is the route table**: bare 32-bit keys + `LPM` + `Route` is a sorted-prefix
        search, i.e. the same table `ROUTING-FIB.md` decoded and `fm6000_route` drives.
      - The **38-bit CAM key** is composed per slice by `SLICE_SCENARIO_CFG`: `ByteMux_0..3`
        (4×8 = 32 bits) + `Top4Mux` (6) = exactly 38. Sources are the parser's halfword channels
        — slices select `L3_SIP`, `L4_SRC`, `L3_LENGTH`, `L3_DIP`, which is what an ACL matches.
      - ⛔ **THE HARDWARE EXPERIMENT WAS RUN AND CANNOT WORK ON THIS BOX.** Attempted: program an
        unused slice-2 CAM entry, match-all, `ActionData=0x10`, strobe `ATOMIC_APPLY` bit 0.
        Writes land, `MASTER_VALID=0x3`, scenario decodes correctly, and L3AR rule 31 genuinely
        matches `FFU_DATA_W8A` bit 4 — yet nothing observable changes, at entry 1000 or entry 100,
        with or without L3AR rule 31's action zeroed. **The reason is topological: the 7150 has
        one connected front port (et1), so no traffic transits it.** Every frame we can generate
        terminates on the CPU, and CPU-terminated frames never exercise the FFU→L2AR/L3AR→MOD
        forwarding decision. A second connected port (or transit traffic) is required — the same
        prerequisite as A4, for the same underlying reason.
      - Also learned: **`ffuFlagDropFrame` does not drop.** Its L3AR action is the baseline mask
        with no set bits, on EOS's image and ours. The drop lives downstream in L2AR (A2).
      - ⚠ Generator also blocked on naming ByteMux sources. ByteMux 53/58/60 are not
        parser channels — a scan of all 2,145 of EOS's parser actions shows it writes channels
        **0..42 only**. Two readings survive and the shipped configurations refute neither:
        (a) direct channel index into a 64-channel space where 44..63 come from another block,
        or (b) a byte address into the 32 halfword channels (`v//2`, byte `v%2`). Settling it
        needs to observe which frame bytes affect a match. Muxes and key must be programmed
        together — reading either alone is meaningless.
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

- [x] ~~**E0. Dataplane watchdog.**~~ **Written and validated** (`asic/fm6000/fm6000_wdog.c`).
      The box already survives a CPU hard lockup — the kernel cmdline carries `nmi_watchdog=panic`
      and `reboot=p`, which is why an ASIC wedge on 2026-08-11 rebooted the switch onto EOS by
      itself rather than needing physical access. What was missing is the EdgeNOS failure mode:
      **Linux healthy, dataplane dead**, which nothing watched (there is no `/dev/watchdog`; the
      `scd` driver exposes only `interrupt_mask_watchdog5/6/7`).
      Checks `PIN_STRAP == 0x208` (unambiguous: a downed device reads `0xffffffff`) on a 3-strike
      fuse, and kernel route count on a fuse 4× longer. **Ping is deliberately not a signal** — D5
      would reboot a healthy switch. `/mnt/flash/wdog.off` disables it; create that file before any
      experiment that deliberately downs the dataplane.
      Validated on hardware in dry-run: healthy → exit 0; forced route floor → `BELOW FLOOR`,
      exit 1; disable file honoured; logs to `/mnt/flash/wdog.log`.
      - [ ] **E0a. Start it at boot.** Currently must be launched by hand; needs to go in the SWI
            alongside E1. Until then a wedge is only self-healing if the watchdog was started.


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

## Current blocker: EdgeNOS forwarding, and how it is being approached

**2026-08-11.** After a reboot EdgeNOS came up with tables correct and link healthy but no
forwarding. Long direct debugging (see `M1-BRINGUP-SEQUENCE.md`) narrowed it to CPU→ASIC→wire
egress and turned up two real defects, then stalled.

What broke the stall was **diffing the live chip under EOS against the live chip under EdgeNOS**
(`EOS-VS-EDGENOS-DIFF.md`). EOS forwards on this exact hardware, so the candidate set is exactly
what differs. It immediately killed the leading hypothesis — CM `0x114000`, which I had spent an
hour on, is **zero on EOS too** — and produced three concrete leads instead: 1,746 words of
uninitialised memory (the measured "no CRM fill" gap), three config bits in EPL14's
`EPL_CFG_A`/`EPL_CFG_B`/`EPL_IP`, and EOS's complete working configuration for **port 3**, which
is up under EOS as `Et3 connected 10GBASE-SR`.

That last one also unblocks A4 and B1: the transit traffic they need no longer requires deriving a
SerDes bring-up from scratch, because a working lane-1 configuration is now captured.

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

**One halfword-channel bus, three blocks.** The parser *writes* 16-bit channels, the FFU
*selects* them (`ByteMux` values come in pairs, one per byte of a halfword), and MOD *reads* them
(`DataSelect` feeds two consecutive bytes). Naming a channel once names it everywhere — which is
why `L3_SIP`/`L4_SRC` fell out of the FFU scenario config for free.

**Run the control BEFORE the experiment, and let it fail loudly.** The FFU ByteMux test had a
clean discriminating design — reading (a) predicts a match on `L3_SIP` bytes, reading (b) does
not — and it would have produced a confident wrong answer. The trial's null result would have
read as "reading (b) confirmed" when in fact the apparatus was inert. The control (match
everything, same action) caught that in one run. A discriminating experiment is only
discriminating if a positive result is reachable.

**Ping is not useless — but it needs a control.** D5 makes ping fail for unrelated reasons, so a
bare failure proves nothing. Alternating our program against EOS's in the same minute (A/B/C/A)
turned it into a reliable signal: EOS 0%, ours-overlaid 0%, ours-with-deletions 100%, EOS 0%.
