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
      et1, 640/640 readback, routes 34. Scope is **slice 0 only**.
      ⛔ **The description of slices 1-4 that used to sit here — "csGlort, policers, storm control
      and L3 QoS" — is UNCITED and contradicted by the datasheet.** §5.10.1 gives L3AR "5 serial
      application stages" whose "changes accumulate serially from one stage to the next": they are
      stages of one resolution, not separate features. Deleting slices 1-4 killed forwarding
      (see `BLOB-REMOVAL-PLAN.md`). Slices 1-4 are left in the replay because they are **not yet
      authored**, not because they are unused.
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
      - ✅ **UNBLOCKED since 2026-08-15** by option (b): Et2 links, and the transit rig captures the
        switch's egress on the peer's swp6. The "no egress capture point" text above is stale.
      - ✅ **2026-08-17: `0xe0` = `DECREMENT`**, identified positively — `PARSER-CONVENTIONS.md`.
        Also measured: the firing entry is **slot 9 in slices 14, 15 and 16** (14 of 15 live entries
        in bank 15 are inert), which is what "needs the frame's 48-bit key" was blocking on. ⚠ not
        uniform across banks — bank 1's slot 9 is empty.
      - ✅ **The full step list is mapped**: a routed IPv4 frame is **9 commands**, with the firing
        entry located in each slice (9 of 18 slices are entirely inert). `0x85` x2 = MAC rewrite,
        `0x01`/`0x05` = skips, `0xe0` = decrement, `0x20` = increment. Only `0xBE` and `0xD0` remain.
      - ✅ **The `operand+1` premise is refuted as a global rule** — it holds only for `SKIP` and the
        write opcode; opcodes 1, 5, 6 and 7 ignore their operand entirely. This was the assumption
        under A4's "five converging lines of evidence", and it is the thing to fix first.
      - ✅ **RESOLVED**: `0x20` is a **conditional** increment that fires only after a decrement, so
        the two `0x20` entries differ because slice 12's follows a `SKIP`. The encoding is therefore
        **stateful across slices** — the generator must model the command *stream*, not emit
        independent per-slice bytes. `0xe0`+`0x20` are a fused decrement/checksum-fixup pair.
      - ✅ **The encoding model is GENERATIVE and hardware-verified**: two novel edits predicted
        byte-exact before running. Any edit expressible as `skip -> decrement -> conditional
        increment` can be generated *and verified against hardware* now. The remaining unknowns no
        longer gate this part. Also: the program is **not L4-dependent** (UDP == ICMP).
      - ✅ **MAC content path COMPLETE** (2026-08-17): src `44 4c` ← value bank 6 slot 8, src
        `a8 31 5d ab` ← bank 8 slot 8, dst ← `NEXTHOP`; all confirmed on live silicon. Only **2 of
        14 data slices** feed a routed IPv4 frame, so a generator needs two value entries here, not
        a populated table.
      - (superseded) **MAC content path essentially FOUND.** dst MAC ← `NEXTHOP` adjacency (`0x160000`); src
        MAC low 4 bytes ← `MOD_VALUE_RAM` **bank 8 slot 8** (`0x159508`) — both confirmed by
        modifying live silicon and watching the emitted frame. ⛔ Only the leading `44 4c` remains;
        it is plausibly parser-channel-sourced rather than constant. Also settles B-item "bank 4's
        MAC association" as a **negative** (bank 4 holds a copy, feeds nothing).
      - ⛔ **`0xBE` / `0xD0` unidentified**, all substitution levers exhausted. Next step is to run
        the same bisection against an L2-switched / VLAN / IPv6 frame — 9 of 18 slices were inert for
        the ICMP flow, so a second frame type is what separates constant structure from flow-specific.
      - ✅ **Value-entry content model decoded**: per byte, `Type 1` = constant / `Type 2` = parser
        channel (`DataSelect`), mixable inside one 2-word entry. Confirmed on hardware. A generator
        must emit both forms — constants alone are wrong for any flow but the one captured.
      - ✅ **The opcode map is closed for a routed IPv4 frame**: 0=SKIP, 1=CHECKSUM, 4=REPLACE,
        5=REPLACE_MASKED (dybble mask, three held-out predictions), 7=DECREMENT. Opcode 6 is
        mandatory in the final slice with **no observed frame effect** — a terminator by elimination,
        deliberately NOT recorded as confirmed. Opcodes 2/3 are unused by this flow and must hold
        INSERT / DELETE / DECREMENT_INSERT / DECREMENT_REPLACE; reaching them needs a second frame type.
      - (superseded) **opcode 5 partially cracked**: it can zero the byte at the cursor, its operand selects
        between zero / no-write / frame-drop, and it is context-dependent like `0x20`. The earlier
        "operand ignored" reading is RETRACTED — it was measured on the TOS byte, which is zero in
        normal traffic and so cannot show a zeroing write.
      - ⛔ Still open for the generator: what `0x20` is (it increments the byte after the cursor,
        which *sits* on the IP checksum but is not proven to be the checksum step), and the fact that
        **operands are unused** for `0xe0` and `0x20` — so the `operand+1` length rule does **not**
        hold for every opcode. A generator written on that rule would be wrong for these two.
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
      - ✅ **2026-08-12: that prerequisite is now satisfiable.** The reason there was only one
        connected front port was a *configuration* fact, not a wiring one: `Et3` was an access
        port in VLAN 1 while `Et1` was routed, so they were never in one forwarding domain.
        `Ethernet3` is now configured routed (`10.99.99.1/24`, in OSPF area 0) with the test host
        on it, and **transit is demonstrated on EOS** — the test host reaches the switch loopback
        `10.101.255.1`, which is traffic ingressing Et3 and routed to a non-connected destination.
        Capturing a replay from *this* EOS configuration is what unblocks both A4 and B1. See
        `ROUTED-PORT-ANATOMY.md`; the five-register recipe that makes a port routed is implemented
        in `fm6000_rport` and verified on both operating systems.
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

- [x] ~~**E0. Dataplane watchdog.**~~ **Written, validated, and PROVEN IN THE FIELD**
      (`asic/fm6000/fm6000_wdog.c`).
      ⚠ **CORRECTION 2026-08-21: the kernel cmdline does NOT carry `nmi_watchdog=panic` or
      `reboot=p`.** Measured on the running alpha42 image, it is exactly:
      `pnpacpi=off pci=nocrs,lastbus=0 intel_iommu=off nr_cpus=1 tsc=reliable console=ttyS0,9600`.
      So the claim that "the box already survives a CPU hard lockup" is **false for this image** —
      with no `/dev/watchdog` either, a genuine CPU lockup hangs indefinitely and needs physical
      access. Whatever rebooted the box on 2026-08-11, it was not this mechanism.
      **Adding `nmi_watchdog=panic reboot=p` to the cmdline is now the open item**, and it is the
      only thing standing between us and self-recovery from a CPU wedge.

      **It fired for real on 2026-08-21T14:28:22** and did exactly the right thing:
      `FIRING: PIN_STRAP=0xffffffff (3 strikes) routes=43 (0 strikes)` — the FM6000 dropped off
      the PCI bus while the control plane stayed healthy at 43 routes, and the watchdog rebooted
      the box unattended. That is 2 firings in the log's whole history (the other, 2026-08-12, was
      the route floor). `/mnt/flash/wdog.log` is on flash and survives the reboot, which is what
      made the event diagnosable at all — EOS's own `show reload cause` reports nothing.
      ⚠ **The log does not record WHY the ASIC dropped.** Capturing PCI config space / AER state
      at FIRING time is what would make the next occurrence root-causable. What was missing is the EdgeNOS failure mode:
      **Linux healthy, dataplane dead**, which nothing watched (there is no `/dev/watchdog`; the
      `scd` driver exposes only `interrupt_mask_watchdog5/6/7`).
      Checks `PIN_STRAP == 0x208` (unambiguous: a downed device reads `0xffffffff`) on a 3-strike
      fuse, and kernel route count on a fuse 4× longer. **Ping is deliberately not a signal** — D5
      would reboot a healthy switch. `/mnt/flash/wdog.off` disables it; create that file before any
      experiment that deliberately downs the dataplane.
      Validated on hardware in dry-run: healthy → exit 0; forced route floor → `BELOW FLOOR`,
      exit 1; disable file honoured; logs to `/mnt/flash/wdog.log`.
      - [x] ~~**E0a. Start it at boot.**~~ **DONE** — `init-m1:245` starts it detached when a
            replay is present (`setsid /usr/bin/fm6000_wdog &`). Confirmed running on alpha42 as
            pid 1609, and it is what caught the 14:28 ASIC drop. This entry previously said it
            "must be launched by hand"; that was stale.


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

## 2026-08-12: ONE BLOCKER GATES THREE ITEMS

The list below has accumulated "blocked on X" notes that all turn out to be the same X. Stating it
plainly:

```
        port 3 forwarding  ──gates──>  transit traffic  ──gates──>  A4 (MOD split)
                                                          └───────>  B1 (FFU ByteMux)
                     ^
                     └── needs: the port-attribute that programs L2F_TABLE_256
                                (route found 2026-08-12, ID not yet extracted)
```

**A4 and B1 are not independently blocked.** Both need to observe how a frame is transformed, which
needs traffic that *transits* the switch, which needs a second forwarding port. The test host is
physically connected to port 3 and the link is up; what is missing is putting port 41 into a
forwarding domain.

**And that now has a known route.** `docs/SDK-TRACING.md` decodes how the FocalPoint SDK writes
registers (function pointers at switch-struct offsets `0x3cc54` write32 / `0x3cc58` read32 /
`0x3cc6c` multi-word), verified against a known answer. Applying it:

- `fm6000SetVlanMembership` / `AddVlanPortList` / `CreateVlan` touch **no hardware at all** — which
  is why the port-3 work never found a "VLAN membership register".
- `fm6000UpdatePortMask` writes **`L2F_TABLE_256`** (one 3-word entry) **and `LBS_BASE`** (twice).
- It is reached from `fm6000SetPortAttributeInt` via `fmBitArrayToPortMask` — i.e. **the operation
  is "set a port attribute whose value is a port list"**.

**Next concrete step:** `fm6000SetPortAttributeInt` is ~68 KB and dispatches on attribute ID through
a jump table. Locate `jmp *table(,%reg,4)`, read the table, map the three `UpdatePortMask` call-site
offsets (`+0x796`, `+0x9ed`, `+0x1c77`) back to case indices. That yields the attribute IDs, hence
what to program.

⚠ Two corrections to earlier entries in this file, both from the same session:
- "the 7150 has one connected front port" (A4, B1) is **out of date** — there are two, and port 3's
  link is up. The blocker moved from *topology* to *forwarding-domain membership*.
- `LBS_CAM` was set on port 41 by copying port 40's value. LBS is **per-port and positional**
  (`0x0001fffe` vs `0x0003fffc`, a one-bit shift); the SDK computes it per port. That copy was wrong.

### Also completed since the last update

- **Three memfill defects**, all the same class (reconstructed fill lengths running short), all
  found by diffing a live EdgeNOS chip against a live EOS chip, all fixed:
  MOD (8,192 words), MAPPER (2,253), L2F (1,024). MOD went from 6,143 differing words against EOS
  to **0 of 65,536**.
- **The F64 frame type decoded** — bit 15 of tag word 0, with all three types enumerated — and the
  special-delivery rule added to `parser_program.py` and verified live.
- **`edgenos-up.sh` documented** as the supported bring-up (`docs/M1-BRINGUP-SEQUENCE.md`); a
  forwarding outage was root-caused to a hand-rolled sequence omitting `ip link set et1 address`.

## 2026-08-11 status: forwarding restored, port 3 up

**Forwarding was never broken** — the fault was a hand-reconstructed bring-up that omitted
`ip link set et1 address`. `edgenos-up.sh` ships in the image at
`/usr/lib/edgenos/platform/edgenos-up.sh` and does it correctly; run it, do not reconstruct it.
Verified: OSPF adjacency in 8s, `routes=34`, hardware FIB sync programming 14 routes.

**Front-panel port 3 is now up** (`PORT_STATUS` lane1 `0x15` → `0x8c0`, stable), brought up from
EOS's captured lane-1 configuration — see `PORT3-BRINGUP.md`. That is the second connected port
A4 and B1 have been waiting on.

Two real defects were found and fixed on the way, by diffing the live chip against a working EOS
boot: **MOD memfill** short by 8,192 words (MOD vs EOS: 6,143 differing → **0 of 65,536**) and
**MAPPER memfill** holes totalling 2,253 words. Both cause intermittent boot-dependent failures.

## Historical: the forwarding hunt, and how it was approached

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

## 2026-08-15: the A4/B1 blocker is gone, and E0a was worse than recorded

### ✅ The egress capture point exists — A4 and B1 are no longer blocked on topology

A4 says *"the 7150 currently has no egress capture point: one front port (et1), no tcpdump, peer
`10.101.101.25` refuses SSH"*, and B1 inherits the same blocker. **All three clauses are now false:**

- **Two front ports come up under EdgeNOS**, Et1 and Et2. Not one.
- They land on **two different /29s of the same AS5610** — `swp6 10.101.101.25/29` and
  `swp7 10.101.101.33/29` — so a frame in one and out the other genuinely transits the switch.
- That peer is `10.1.1.238`, **it answers SSH** (root/`as5610`), and it has **`tcpdump 4.99.4`**.

`tools/transit-test.sh` drives it: a `/32` route on the peer forces the hairpin (the peer is both
ends, so without it the traffic never touches the switch), then `tcpdump` on `swp6` captures what
the 7150 **emitted**. That is exactly the observation A4 needs to choose between the surviving
readings of the command split, and what B1 needs to see which frame bytes affect a match.

⚠ **Gated on a coin flip, not on a fault.** Et2 does not come up on every boot — see
`PORT3-BRINGUP.md`, where a single-boot reading of exactly this produced two days of wrong
conclusions. `transit-test.sh` refuses to run without an et2 carrier rather than returning a
confusing null; if it refuses, reboot and retry, and do not diagnose it.

**2026-08-17 — when Et2 does come up, it is a real link, and the cause is not in the replay.**
Measured on a fresh good boot: Et2 held `LANE_STATUS=0x940` for **60 of 60 samples over 5 minutes**,
`PORT_STATUS=0x08C0` with `HiBer` clear, against an Et1 control at 60/60 — still locked ten minutes
in. A dark boot is equally unambiguous at 0/20. **So `transit-test.sh` gating on Et2 is sound: reboot
until it comes up (~1 in 2) and the link you get is trustworthy for the whole session.**

⚠ Do **not** substitute `fm6000_lanelink 2` for a good boot. It reaches `0x940` but produces a third,
worse state — a `HiBer` lock that oscillates (13 of 22 samples) — and **re-running it on a lane that
is already up tears the link down**. Check `tools/fm6000-status.sh` first and never drive a locked
port. See `tools/README-7150-harnesses.md` for the three silent reboot traps.

The replay itself is exonerated as the cause: **its last write to any EPL is op 157,123**, after
which 29% of the replay executes without touching a port. Good and dark boots run byte-identical
streams and differ only in how long autonomous SerDes training takes to converge. This closes the
"Et2 replay race" line with a negative — there is no write to find.

### ⛔ E0a was understated: the watchdog was not in the image at all

The entry says the watchdog "must be launched by hand". In fact `build-release-swi.sh`'s tool list
never named `fm6000_wdog`, so **no image has ever contained it** — the only copy is one that was
hand-placed on flash. Fixed: it is built into the image and started from `init-m1` after the
dataplane comes up, honouring `/mnt/flash/wdog.off`.

This mattered the same day: three consecutive mid-FULLSEQ resets, with nothing watching.

⚠ `/mnt/flash/wdog.off` has been present since 2026-08-13 and is still there. **Remove it when the
current experiments finish**, or the newly-started watchdog will disable itself on every boot.

### ⚠ Every image on flash predates the `MGMT_PEER` pin

`init-m1` was fixed on 2026-08-13 to pin the admin subnet to `eth0`. **alpha9 was built before that
and does not carry it**, so `edgenos-up.sh` still black-holes management the moment ospfd forms an
adjacency — observed live today, recovered over serial. Every initrd on flash has the same hole.
A rebuild is the fix and the build tree is ready for one.

### On the numbers in this file

`D5` says route count, not ping, is the reliable signal. That is right and does not go far enough:
**on this platform a single boot measures nothing at all.** Et2's link state is intermittent, so any
claim of the form "X makes it work" needs n boots per arm and a reported count. Several conclusions
in this file and in `PORT3-BRINGUP.md` were single observations and should be re-taken before they
are built on.

### C1 / C2 update — the copper question is being measured, not argued

C1 reads *"Et2 is intermittent with **and** without SPICO. Until settled, 'zero proprietary files'
is honest only for a fibre-only build."* That sentence contains the answer and misreads it: the
intermittency is a property of **the boot**, not of copper. Et2 comes up on **5 of 10 identical
boots** with the firmware fully present (`PORT3-BRINGUP.md`).

So the claim C2 depends on — *SPICO is required for 10GBASE-CR* — rests on one boot per condition on
a coin, and does not stand as evidence. **In progress:** five boots with exactly the 30,002 IMEM
transactions stripped and nothing else (`fwd4-nospico.txt`, md5 `cac05757…`; the 477 SPICO
control/interrupt commands are preserved, so the micro-controller is still reset, enabled and told
to run — it simply has no firmware).

- **any boot brings Et2 up** → "required" is refuted outright, and **C2 disappears entirely**.
  "Zero proprietary files" stops being a fibre-only claim.
- **0 of 5** → "required" is supported at p ≈ 0.03, and C2 stays on the list as real work.

⚠ Note for whoever reads this next: **do not re-run this as a rate comparison.** Separating two
arms near 50% needs ~32 boots each. This test is affordable only because the hypothesis predicts a
*zero*, and a predicted zero is cheap to falsify. Design future copper experiments the same way.

## ★★★ 2026-08-15: TRANSIT WORKS — A4 and B1 are unblocked

The prerequisite both items have been blocked on since 2026-08-12 is met. Frames now enter Et2, are
routed, and leave Et1, and we can see what the switch emitted.

Captured on the peer's `swp6` — the 7150's **egress** — while pinging in via `swp7`:

```
44:4c:a8:31:5d:ab > 80:a2:35:81:ca:b4, IPv4, length 102:
    10.101.101.33 > 10.102.1.1: ICMP echo request

0x0000:  4500 0054 4daa 4000 3f01 7312 0a65 6521
0x0010:  0a66 0101 0800 188e ...
```

- **Source MAC is the switch's router MAC**, destination is the peer's swp6 — the L2 header was
  rewritten, so this is a routed frame, not a bridged one.
- **TTL = `0x3f` = 63.** The peer sent 64. **The MOD engine's DECREMENT is visible on the wire.**
- **Checksum `0x7312` recomputed** for the new TTL — MOD's CHECKSUM step, also visible.

(The 100% ping loss is only `10.102.1.1` not answering; transit is proven by the egress capture, not
by the reply.)

### What unblocked it

Not topology, and not Et2. **One field.** `fm6000_portd.c` demuxed punted frames using tag word 2;
the source glort is in **word 1**, which this repo's own `PORT3-BRINGUP.md` recorded months ago.
With `src_word = 1`:

```
10.101.101.33 dev et2 lladdr 80:a2:35:81:ca:b5 REACHABLE     (was FAILED)
ping 10.101.101.34 from the peer: 6/6, 0% loss
et2 rx: 0 -> 15
```

Per-port RX had never worked, and could not have: every punted frame fell through to port 0. The
single-port setup masked it perfectly, which is why it survived so long.

### A4 is now a two-hour job, not a blocked one

A4 needed to "observe emitted bytes" to settle the MOD command split. It can now do so on demand.
The candidates are `0x20` (op1/operand 0) and `0xe0` (op7/operand 0) — the two most route-enriched
length-less commands (p ≈ 1.2e-04 each) — though ⚠ they do **not** co-occur across EOS's programs,
which argues against them being the DECREMENT/CHECKSUM pair (see `PARSER-CONVENTIONS.md`). The bench
test is now direct: program a candidate, send a frame through the hairpin, and read the TTL and
checksum off `swp6`.

### B1 likewise

B1 needed to see which frame bytes affect an FFU match. Transit traffic through a programmable slice
is exactly that experiment, and it now has both a path and a capture point.
