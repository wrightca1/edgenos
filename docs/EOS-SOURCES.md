# What EOS actually ships, and what that changes

Established 2026-08-09 by unpacking `EOS-4.16.8M.swi` (md5 `ed9007a384b93e726b8147b017aba8f1`,
436,042,743 bytes) pulled from the switch's own flash. The image is a zip; the payload is
`rootfs-i386.sqsh`, a 411 MB xz squashfs — 29,430 files.

**The premise this doc rests on:** the values in `fwd4.txt` are not a puzzle to be solved from
their shape. EOS computes them, and EOS ships both the code that computes them and, in two cases,
the exact data being written.

---

## 1. The two microcode files are one EOS file, split

`/mnt/flash/ucode_l2.raw` and `/mnt/flash/ucode_tail.raw` are
`/usr/share/firmware/fm6000Microcode.raw` cut at byte offset **545,778**. Verified by md5, all
three comparisons exact:

| file | bytes | md5 |
|---|---:|---|
| `fm6000Microcode.raw` (EOS) | 710,406 | `e4608340333ce2c01820fd9e1ca097ca` |
| `ucode_l2.raw` == EOS[0:545778] | 545,778 | `90ee0cc8801368a789b39a1c18fec75a` |
| `ucode_tail.raw` == EOS[545778:] | 164,628 | `a502d6e5749a050012f7d94c73925941` |

They are **not binary microcode**. The file is ASCII in exactly the `fwd4.txt` format —
`<addr> <value>`, hex, one write per line — 39,467 writes over 31 pages.

```
00123100 ffffffff
00123101 ffffffff
00123200 00000000
```

### It is documented, by name

`/usr/share/firmware/fm6000MicrocodeRuleNames.txt` (577 lines) names every microcode rule by
slice and index, for both L2AR and L3AR:

```
Slice #0, Rule #0 : SwitchNormalWithLoopbackSuppress
Slice #0, Rule #5 : IslF64FtypeNormal
Slice #0, Rule #6 : IbvAndEbvCheck
```

This is the semantic key to L2AR — the block `SELF-CONTAINED-PLAN.md` calls "the largest block"
at 26,376 writes. A generator emitting a *chosen subset* of named rules is now a tractable job
rather than blind decoding. EdgeNOS needs fewer rules than EOS (no VXLAN, no ISIS initially).

There is a second pair for tap aggregation, `fm6000MicrocodeTapAgg.raw` (523,656 bytes) and
`fm6000MicrocodeTapAggRuleNames.txt` — not on our boot path, but it proves the microcode is
*selected per application*, not fixed.

---

## 2. 15.1% of `fwd4.txt` is the microcode being replayed

Every one of the microcode's **39,415 distinct addresses appears in `fwd4.txt`**. None falls
outside it. Comparing the microcode's values against the replay's last-write-wins end state:

| | count | share |
|---|---:|---|
| microcode addresses whose final value in `fwd4` is **identical** | 38,564 | **97.8%** |
| addresses that differ (runtime-configured) | 851 | 2.2% |
| `fwd4` **lines** writing a microcode address | 58,814 | **15.1% of the replay** |

The 851 differences are configuration written over the template, not a different microcode:

- the system MAC — microcode ships placeholders `0xddccbbaa`, `0x4455ffee`, `0x00112233`;
  `fwd4` holds `0x444ca831` / `0x5dabbbb3` (this board is `44:4c:a8:31:5d:aa`)
- port and VLAN masks — `0xffff0000`, `0xfff80007`, `0x0000003f`, `0x61c70000`

Concentrated on pages `0x144000` (328), `0x108000` (165), `0x010000` (64), `0x141000` (49).

### ⚠ This resolves triage group 3

`REPLAY-TRIAGE.md` group 3 lists ~3,700 "unnamed" writes at `0x140000`/`0x141000`/`0x144000` and
`0x010000`, and commit `aae8a19` recorded that they are absent from the Intel header's macros
entirely and "need a different source". **This is that source: they are microcode.** The pages
line up directly — microcode covers 1,680 / 2,832 / 864 / 2,448 writes on those four pages.

Group 3's action item should be rewritten from "extend regmap.py" (already done, and it did not
help here) to "decode against `fm6000MicrocodeRuleNames.txt`".

---

## 3. Where the computation lives

Both relevant libraries are stripped of local symbols but **retain their dynamic exports**, which
is enough to name the operations even without disassembly.

| library | size | dyn syms | `fm6000*` | what it is |
|---|---:|---:|---:|---|
| `libFocalpointSDK.so` | 4.2 MB | 2,421 | **987** | the FM6000 driver layer — `aristaFm6000*` + `fm6000*` |
| `libFocalpointWhiteAlta.so` | 6.0 MB | 418 | 144 | a **behavioural model** of Alta (`fm6000Model*`), not the driver |
| `libFocalPointV2Agent.so` | **27 MB** | | | the agent itself; largest object, not yet examined |
| `libHwFocalPointV2.so` | 4.3 MB | | | hardware layer |

"FocalPoint" is Fulcrum's SDK name; "Alta" is the FM6000's codename. The `Model` prefix on
WhiteAlta matters — those are simulation entry points (there is a `libFocalPointSimulationTypes.so`
beside it), useful for *semantics* but not the init sequence.

Exports that map onto blocks we are still fighting:

```
fm6000FFUInit           fm6000CacheFfuSliceCam        fm6000AtomicApplyFFU
fm6000BootSwitch        fm6000CacheFfuSliceScenarioCam fm6000AclInit
fm6000HashInit          fm6000CacheMapperDipCam1..3   fm6000BstInit
fm6000InitAddressTable  fm6000CacheMapperDmacCam1..3  fm6000BistMemoryInit
```

`fm6000CacheFfuSliceCam` + `fm6000AtomicApplyFFU` is precisely the entry/strobe pairing our own
FFU analysis derived from the trace — cache the CAM entries, then commit atomically. The naming
independently corroborates that the `0x3f0000` strobe is a commit barrier and must not be
collapsed.

### Still unexamined
`DosFocalPointLib/` holds Arista's own Python for Alta as **2.7 bytecode**: `AltaLib.pyc`
(121 KB), `DosFmApi.pyc` (274 KB), `AltaChip.pyc`, plus per-block test modules
(`DosFpL3Test.pyc`, `DosFpCmTest.pyc`, `DosFpLatencyTest.pyc`). Python 2.7 bytecode decompiles
well. This is the most promising unexplored lead in the image.

---

## 4. The Intel register header ships inside EOS

`/usr/lib/python2.7/site-packages/DosFocalPointLib/fm6000_api_regs_int` — 645,537 bytes of C
source. It is the same header `regmap.py` reads:

- **632** parameterised `FM6000_*(...)` macros — exactly the count `SELF-CONTAINED-PLAN.md` cites
- 750 `_ENTRIES` bounds, 119 `_BASE` constants
- `FM6000_CM_PORT_RXMP_PRIVATE_WM(index1, index0)` resolves to `0x112800`, matching the formula
  quoted in our docs character for character

### ⚠ Provenance: this changes availability, NOT permission

The file carries the full **INTEL CONFIDENTIAL** notice, Copyright 2011 Intel Corporation. It
being present in an EOS image does not make it redistributable, and **the tree's policy is
unchanged**: `regmap.py` reads it at runtime, we never embed it, it is never committed.

What it *does* change is who can satisfy that dependency. Any operator holding a licensed EOS
image already has this header — so `regmap.py` no longer implies access to a private notes repo.
This is the same posture already recorded for SPICO: operator-supplied, reconstructible from
their own licensed image, never shipped by us.

The same caution applies to everything else in this doc. `fm6000MicrocodeRuleNames.txt` is marked
"Arista Networks, Inc. Confidential and Proprietary". Rule *names* and the structural facts above
are interface facts we may act on and cite; the files themselves stay out of the tree, and none
of this may be copied into EdgeNOS source. Derive the facts, implement our own.

---

## 5. What this is worth

| item | before | now |
|---|---|---|
| `ucode_l2.raw` + `ucode_tail.raw` | two opaque blobs "to be generated" | one documented EOS file, split; rules named per slice |
| triage group 3 (~3,700 writes) | unnamed, "needs a different source" | identified as microcode; source in hand |
| L2AR (26,376 writes) | "microcode", undecoded | 577 named rules across L2AR and L3AR |
| FFU strobe semantics | inferred from the trace | corroborated by `fm6000CacheFfuSliceCam` / `fm6000AtomicApplyFFU` |
| `regmap.py`'s header | private notes repo only | in any licensed EOS image |

Nothing here has been tested on hardware. Every reduction still gets a cold boot and a
stock-replay control, per `SELF-CONTAINED-PLAN.md`.

---

## 6. `DosFocalPointLib` decompiled — 16,577 lines recovered

`uncompyle6` 3.9.3 fails on this bytecode (parse error; it emits grammar state instead of source).
**`pycdc` (Decompyle++) works** — built from source, `cmake -B build -DCMAKE_BUILD_TYPE=Release`.
40 of 43 modules decompile cleanly; 3 segfault the decompiler (`DosFpBreakin` produces nothing).

⚠ **Decompiler artifacts are real.** Boolean conditionals come back mangled — `if not value >= 0
or value <= 7:` and `if None == 2:` are both wrong reconstructions. Register numbers, bit fields,
constants and call structure are reliable; **control flow and comparisons are not**. Treat any
conditional as a hint to verify, never as fact.

### The important negative result

`DosFmApi.py` is 332 KB and looks like the prize. It is not — every function is a SWIG shim:

```python
def fm6000ModelPlatformLoadMicrocode(sw):
    return _DosFmApi.fm6000ModelPlatformLoadMicrocode(sw)
```

**The algorithms stay in native code.** What the bindings give us is the API surface — names,
constants and argument shapes — not logic. Six modules carry genuine Python:
`FocalPointLib` (92 defs), `AltaLib` (98), `FocalPointCommon` (46), `SwitchLib` (39),
`PciLib` (12), `AltaChip` (11).

### `fm6000Hal.py` is Arista's own `regmap.py`

It parses the register header at runtime and builds address functions from the macro text —
`self.info.assignIndexes(args); return eval(self.info.function)` — the same technique our
`regmap.py` uses, arrived at independently. It never embeds the header either. Useful
confirmation that the approach is the intended one, not a workaround.

### ★ The SBus lane tuning is decodable after all

`REPLAY-TRIAGE.md` group 1 writes off 3,721 SBus writes as "per-lane board measurement, not
something to generate". `AltaLib.AltaSerdes` gives the **register and bit-field for every knob**,
so those writes are readable rather than opaque:

| knob | SBus ETH register | bits |
|---|---:|---|
| TX pre-emphasis | 65 / 62 | `r65[2]<<2 \| r62[2:4]`, `r62[1]`=update |
| TX post-emphasis | 62 | `[4:8]`, `r62[1]`=update |
| slew | 61 | `[0:2]` |
| SPICO eye score type | 42 | `[3:5]` — 1=height, 2=width |

Also recovered: `M6000_MIN_SBUS_ETH_DEVICE = 5`, so a lane's absolute SBus ID is `5 + serdes
number`; and the SPICO command interface is `fm6000InterruptSpico(sw, cmd, arg, timeout)` with
**cmd 32 = eye score**.

This does not make the tuning *generatable* — the values remain measurements of this board's PCB.
It does mean we can read what each recorded write means, port them deliberately, and stop
treating the block as opaque. Same for the class hierarchy: `AltaEpl` maps lanes via
`switch.mapEplLaneToSerdes(eplNumber, lane)`, which is the geometry `epl_decode.py` recovered
from the trace.

### How microcode is actually loaded

Not by writing registers directly — by handing the SDK a file path and letting it load
(`bz5662.py`):

```python
api.fmSetApiAttribute(api.FM_AAK_API_FM6000_VALIDATE_MICROCODE, True)
api.fmSetApiAttribute(api.FM_AAK_API_FM6000_MICROCODE_IMAGE,
                      api.FM6000_MICROCODE_RAW_FILE_PATH)
```

So `fm6000Microcode.raw` is a first-class SDK input with a validation path, which is consistent
with §1: it is data, versioned and selectable per application, not compiled-in code.

### Where to go next

The remaining logic is native. `libFocalPointV2Agent.so` is **27 MB** and untouched — the largest
object in the image and the agent that actually drives bring-up. `libFocalpointSDK.so` retains
987 `fm6000*` dynamic exports, so its call graph is navigable even stripped.

---

## Reproducing this

```
# image pulled from the switch's own flash, not downloaded
ssh root@10.1.1.77 'cat /mnt/flash/EOS-4.16.8M.swi' > EOS-4.16.8M.swi
unzip -o EOS-4.16.8M.swi rootfs-i386.sqsh
unsquashfs -d rootfs rootfs-i386.sqsh

# decompiler (uncompyle6 does NOT work on these)
git clone --depth 1 https://github.com/zrax/pycdc.git
cmake -B pycdc/build -DCMAKE_BUILD_TYPE=Release pycdc && make -C pycdc/build -j$(nproc)
for f in rootfs/usr/lib/python2.7/site-packages/DosFocalPointLib/*.pyc; do
  pycdc/build/pycdc "$f" > "decomp/$(basename $f .pyc).py" 2>/dev/null
done
```

Working copy lives at `/home/smiley/eos-work/` — deliberately **outside the repo**, so no
EOS-derived file can be committed by accident.

---

## The FocalPoint SDK, and the SerDes state machine read out of it

**2026-08-13.** `EOS-4.16.8M.swi` was pulled off the switch's flash and stored **outside every git
tree** at `../eos-4.16.8M/` (md5 `ed9007a384b93e726b8147b017aba8f1`, verified on both ends; see the
README there). It is a zip: `version`, `boot0`, `initrd-i386`, `linux-i386`, and a 431 MB
`rootfs-i386.sqsh` holding 34,731 files.

`usr/lib/libFocalpointSDK.so` — 4.2 MB, ELF 32-bit, **2537 dynamic symbols** (static ones stripped).
This is the FM6000 SDK, and it is the authority for anything the register header does not cover.

⚠ Nothing from it is copied into EdgeNOS. What we take is **facts about the silicon**, recorded in
our own words — the same rule `PROVENANCE.md` sets out. `fm6000_spico_code` in there is the SerDes
firmware itself, i.e. checklist item C, and it stays where it is.

### The SerDes state machine — enum values proven, not guessed

The SDK drives a lane through a named state machine:

| value | state |
|---:|---|
| 0 | `FM6000_SERDES_STATE_IDLE` |
| 1 | `FM6000_SERDES_STATE_PWRDOWN` |
| 2 | `FM6000_SERDES_STATE_CONFIG` |
| 3 | `FM6000_SERDES_STATE_PWRUP` |
| 4 | `FM6000_SERDES_STATE_WAIT_PWRUP` |
| 5 | `FM6000_SERDES_STATE_WAIT_SIGDETECT` |

The ordering is **not** inferred from where the strings sit in `.rodata` — it is read off an exact
pointer table at vaddr `0x5ab044` whose six entries point at those six strings in that order. The
SDK logs transitions as `port=%d epl=%d lane=%d: setting SerDes state to %s`, so a live EOS box can
be made to narrate its own bring-up.

Entry points: `fm6000SetSerDesState` (a thin wrapper — logs, takes a capture lock, then calls an
unnamed static at `0x360a16`), `fm6000EnableSerDes` (`0x48131e`, 10 KB — the real bring-up),
`fm6000InterruptSpico` → `fm6000InterruptSpicoV2` (`0x478eef`), `fm6000LoadSpicoCode`,
`fm6000SetSpicoState`.

### ⚠ Two things this does NOT establish

- **The SPICO interrupt codes are bare numeric constants.** There is no `FM6000_SPICO_INT_*` enum in
  the binary, and scraping immediates near the call sites returns neighbouring `fmLogMessage`
  arguments (log category `0x1000`/`0x80000`, timeouts `0x30d40`/`0xc350`) rather than interrupt
  numbers, because the real arguments arrive in registers. Getting them means actually
  disassembling `fm6000EnableSerDes` and following its register allocation. Bounded, but real work.
- **This enum is almost certainly NOT what `fm6000_sbus irq` reads back.** Our `resp reg 0x01`
  returns 2 on both working ports, and `2` here is `CONFIG`, which no working port should be sitting
  in. The SDK enum is software state held in the switch struct; the SPICO response is a different
  namespace. Do not map one onto the other without evidence — that is exactly the class of
  assumption this project keeps having to retract.

### ★★ `fm6000EnableSerDes` disassembled — and it cannot be replayed from a trace

**2026-08-13.** The function that brings a lane up (`0x48131e`, 10 KB) **never issues a SPICO
interrupt**. It is built from direct SBus access and blocking waits:

```
12 x fm6000WriteSBus      11 x fm6000ReadSBus       76 x fmLogMessage
fm6000SetSerDesKrTraining(.., 0)      fm6000SetTxConfig
fm6000WaitForSerDesPllLock            fm6000WaitForSignalDetection
fm6000StartSerDesDfeTuning(.., 1)     fm6000CheckSerDesDfeTuningState
fm6000SetSerDesRxDataGate(.., 0)      fm6000SetSerDesNearLoopback(.., 1)
```

in that order: read-modify-write pairs, KR training off, more RMW pairs, **wait for PLL lock**, more
RMW, TX config, **wait for signal detection**, then DFE tuning and its completion check.

The SBus address is computed in the clear:

```asm
mov  0xc(%ebp),%eax        ; the serdes/lane argument
shl  $0x8,%eax             ; << 8
add  $0xd1122,%eax         ; + 0xd11_22
```

repeated four times in the first block with `0xd1122`, `0xd1126`, `0xd1136`, `0xd113b` — i.e. the
low byte is the **SBus register** and the lane shifts into bits [15:8]. So this block touches
registers **`0x22`, `0x26`, `0x36`, `0x3b`**, and every `ReadSBus` result feeds the `WriteSBus` that
follows it.

### ⚠ Why replaying lane 0's SBus ops onto lane 1 was never going to work

This is the explanation for the negative result in `PORT3-BRINGUP.md`, and it is structural, not a
detail:

- **The writes are read-modify-writes.** Each value EOS writes is computed from what that lane's
  register already held. A capture records only the *result*. Replaying lane 0's resulting values
  onto lane 1 writes numbers derived from lane 0's contents into a lane whose contents differ — the
  read half of every RMW is missing, and no amount of faithful replay recovers it.
- **Two of the steps are waits, not writes.** `fm6000WaitForSerDesPllLock` and
  `fm6000WaitForSignalDetection` poll until the hardware reports ready. A trace of a successful run
  contains the polls that happened to succeed; it carries no notion of waiting, so a replay races
  straight past both.

That retires "relocate the sequence" for SerDes bring-up specifically. It worked for EPL and CM/L2F
because those are state, written absolutely. A lane enable is an **algorithm** — read, decide, write,
wait — and the only faithful way to have it is to implement it.

**So the next step is not another capture.** It is `fm6000_serdes_enable`: RMW those registers in
this order, poll for PLL lock, poll for signal detect, then run the DFE sequence we already have.
The pieces are in hand — `fm6000_sbus` can already read and write single SBus registers, and the
per-lane state readout (`resp reg 0x01`, 0/1/2) gives a way to tell whether it worked.

### The lane-enable algorithm, step by step

Recovered by mapping each `fm6000ReadSBus`/`fm6000WriteSBus` call in `fm6000EnableSerDes` back to
the local holding its address, and each address back to the `(serdes << 8) + 0xd11XX` computation
that built it. Low byte = SBus register; the bit operations are between the read and the write.

| # | step | register | operation |
|---:|---|---|---|
| 1 | RMW | `0x22` | clear bits 0 and 1 |
| 2 | — | | `fm6000SetSerDesKrTraining(.., 0)` |
| 3 | RMW | *(unresolved)* | set bit 0, mask `0x3f` |
| 4 | write | `0x1d` | value computed elsewhere |
| 5 | RMW | `0x36` | value computed elsewhere |
| 6 | RMW | `0x3b` | value computed elsewhere |
| 7 | RMW | `0x17` | set bit 4 |
| 8 | RMW | `0x22` | **set bits 0 and 1** — the inverse of step 1 |
| 9 | **wait** | `0x0f` | poll `sbus_rx_rdy_obs` (b0) and b3 — `fm6000WaitForSerDesPllLock` |
| 10 | RMW | `0x06` | set bit 3 |
| 11 | RMW | `0x03` | set bit 0 |
| 12 | RMW | `0x1f` | mask `0x3f` |
| 13 | RMW | `0x26` | set bit 0 |
| 14 | — | | `fm6000SetTxConfig` |
| 15 | RMW | `0x0d` | set bits 4 and 0 |
| 16 | **wait** | `0x14` | poll `sbus_rx_ib_sig_strength_obs` b6 — `fm6000WaitForSignalDetection` |
| 17 | — | | `fm6000StartSerDesDfeTuning(.., 1)`, then `CheckSerDesDfeTuningState` |
| 18 | — | | `fm6000SetSerDesRxDataGate(.., 0)`, `fm6000SetSerDesNearLoopback(.., 1)` |

**The two waits cross-validate the whole decode.** They were derived from the disassembly alone —
register `0x0f` tested against `0x1`/`0x8`, register `0x14` tested against `0x40` — and the register
header, written by neither us nor this analysis, names `0x0f` bit 0 `sbus_rx_rdy_obs` and `0x14`
bits 6-7 `sbus_rx_ib_sig_strength_obs`. A PLL-lock wait that polls "rx ready" and a
signal-detection wait that polls "signal strength", at bit positions that match, is not a
coincidence two independent sources would produce by accident.

### ⚠ Before implementing this: verify that our SBus writes land

Three addressing prefixes appear — `0xd11XX` in the enable path, `0xd21XX` in the waits, `0xc05XX`
alongside it — and our own tools issue a raw `(op, dev, reg)` transaction instead. Those are not
obviously the same thing, and one number says be careful: register `0x14` bit 6, which the SDK waits
on for signal detection, reads **0 on a working port**. Either we are reading a different view, or
the field only means something mid-bring-up.

⚠ **Do not test this with a write/readback on the same register number.** Tried on `0x1e` and it
proves nothing: `WRITE_30` is `sbus_rx_k28_7_comma_det_en_cntl` (one control bit) while `READ_30` is
`sbus_dfe_scratch_obs`. They are different silicon at the same number, exactly as the header says,
so the value written can never read back. A valid test needs a control bit whose *effect* is
observable somewhere else.

Until that is settled, `fm6000_lanelink`'s 44 SBus and 198 SPICO ops are of unverified effect — the
SPICO ones demonstrably work (they return per-lane answers), the plain SBus writes are unproven.

### The enable registers name themselves — and one anomaly dissolves

`SERDES_ETH_WRITE_n` field names for the registers the algorithm touches:

| reg | bits | field |
|---|---|---|
| `0x22` | 0, 1 | **`sbus_rx_en_cntl`, `sbus_tx_en_cntl`** — cleared at step 1, set at step 8 |
| `0x0d` | 4, 0 | **`sbus_tx_output_en_cntl`**, `sbus_tx_pre_emphasis_gate` (b7 = near loopback) |
| `0x06` | 3 | `sbus_rx_ib_sig_strength_en_cntl` |
| `0x03` | 0 | `sbus_rx_data_gate` |
| `0x26` | 0 | `sbus_rx_dfe_gate` |
| `0x17` | 5, 6, 7 | `sbus_analog_to_core_lsb/msb_gate`, `sbus_from_core_msb_gate` |
| `0x1f` | 0-3 | `sbus_dfe_a_adv_cntl_0..3` |

So the eighteen steps read as: disable RX/TX, configure the analog gates, **enable RX/TX**, wait for
the PLL, turn on signal-strength detection, open the RX data gate, configure DFE advance, open the
DFE gate, set TX config, **enable the TX output**, wait for signal detect, tune.

**And the anomaly that stopped the previous entry dissolves.** `0x14` bit 6 reads 0 on a working
port because signal-strength observation is *switched on* by `0x06` bit 3 as step 10 of bring-up —
it is not a resting-state signal, so reading 0 on a settled port is correct, not evidence that we
are reading the wrong view.

### ⚠ Read-modify-write is impossible through our read path

`WRITE_34` is `rx_en`/`tx_en`; `READ_34` is `sbus_rx_prbs_data_obs`. Reading register `0x22` on the
dark lane returns `0x00` — the PRBS data, not the enable bits. **Every register in this algorithm
has this property**, so the SDK's `fm6000ReadSBus` at prefix `0xd11` must reach a *readback of the
write register*, which our raw `(op 0x22, dev, reg)` transaction does not. Either there is another
op code for it, or the prefix selects the space.

Consequence: the algorithm cannot be transcribed as read-modify-write with the tooling as it stands.
Absolute writes are possible where the full field list is known (which it now is, above).

**Tried, no effect:** absolute `0x22 = 0x03` (rx_en|tx_en) then `0x0d = 0x11`
(tx_output_en|pre_emphasis) on the dark lane. `SerXmit` stayed 0, `pcsRx` stayed 0, both working
ports untouched. That is consistent with two different things and does not separate them: the writes
may not be landing, or they may land and be insufficient without the ordered sequence and its waits.

### The experiment that separates them, not yet run

Clear `rx_en`/`tx_en` on a **working** lane and see whether its link drops. If it does, our writes
land and the dark lane needs the full algorithm; if nothing happens, our write path does not reach
these registers and everything built on it needs rethinking.

Et2 is the only candidate — it is linked, has no netdev and carries no traffic, so the blast radius
is one reboot. ⚠ Note Et2 is a **DAC cable, electrical not optical**, so it is an imperfect stand-in
for Et3's fibre lane in general; for this specific question — do writes to `rx_en`/`tx_en` take
effect — the mechanism is mode-independent and it is a fair test. Et1 is the only other optical port
and it carries the OSPF adjacency, so it is not available as a subject.

### ⛔ ANSWERED: our SBus writes do not take effect

**2026-08-13.** The separating experiment, run on Et2 (linked, no netdev, no traffic):

```
Et2 PORT_STATUS                       0x000008c0   RxLinkUp=1 SerXmit=1
after write 0x45 reg 0x22 = 0x00      0x000008c0   unchanged
  (clears sbus_rx_en_cntl + sbus_tx_en_cntl)
after restoring 0x22 = 0x03           0x000008c0   unchanged
```

**Clearing the RX and TX enables on a lane that is up changed nothing.** A write that genuinely
reached `sbus_rx_en_cntl`/`sbus_tx_en_cntl` would drop the link. So the raw
`cmd = (op<<16) | (dev<<8) | reg | (1<<24)` transaction our tools issue **does not reach these
SerDes control registers**.

That is the answer to the question the previous entry left open, and it is the more consequential
of the two possibilities:

- **`fm6000_lanelink`'s 44 SBus ops per run have no effect.** Neither the provisioning table nor the
  link sequence nor the DFE sequence can have done anything at the SerDes level. Their MMIO halves
  landed — readback proves that — which is exactly why the lane moved from SPICO state 0 to 1 and no
  further. The EPL half works; the SerDes half has never worked.
- **The SPICO path is unaffected** — `fm6000_sbus irq` returns per-lane answers (2/2/1/0/0/0), so
  interrupt-style access does reach the micro-controller. Whatever is wrong is specific to plain
  register writes.

⚠ The alternative reading — that the writes land but `PORT_STATUS` does not reflect a mid-flight
SerDes disable — cannot be excluded from one measurement. It is unlikely (killing the serialiser on
a live 10G link should show somewhere) but it is not disproven, and a second observable would settle
it.

### Where the write path probably went wrong

The SDK never issues a bare `(op, dev, reg)`. It computes
`addr = (serdes << 8) + 0xd11XX`, which decomposes as

```
bits [23:16] = 0x0d          a ring / space selector
bits [15:8]  = serdes + 0x11 the device on that ring
bits [7:0]   = register
```

so `0x0d` is a field our transaction never sets, and `serdes + 0x11` says the SDK's serdes index is
not the device number we have been using. Two prefixes exist beside it — `0xd21` in the waits and
`0xc05` alongside — which is consistent with `0x0d`/`0x0c` selecting a space and `0x11`/`0x21`
distinguishing the write-register readback from the observation registers. **All of that is
inference from one arithmetic pattern and must be verified before anything is built on it.**

**Next: disassemble `fm6000WriteSBus` itself.** It is the one function that converts that composite
address into whatever the hardware actually sees, and it is small. Everything else here is guesswork
until it is read.

### ★★ `fm6000SerDesRemapTable` — the addressing, resolved

`fm6000WriteSBus` calls **`fm6000MapSerDesToEplLane`**, which bounds-checks its argument to
**0..0x5f (96 SerDes**, matching the header's `ENTRIES 96`) and then indexes
**`fm6000SerDesRemapTable`** at vaddr `0x609ec0` — 96 entries, stride 8, `{epl, lane}`. Extracted
from `.data`:

| serdes | EPL / lane | our SBus device |
|---:|---|---|
| **68** | EPL14 lane0 — Et1 | `0x49` |
| **69** | EPL14 lane1 — **Et3** | `0x4a` |
| 70, 71 | EPL14 lanes 2, 3 | `0x4b`, `0x4c` |
| **64** | EPL16 lane0 — Et2 | `0x45` |

**`device = serdes + 5`, confirmed on three ports.** Serdes 68 and 69 are exactly the indices the
earlier gdb session passed to `fm6000StartSerDesDfeTuning` — now read off the table rather than
guessed.

And the SDK's "SBus address" is a flat register map, not a wire-level pair:

```
addr = 0xd1100 + 0x100 * serdes + register        (SERDES_ETH_WRITE space)
       0xd2100 + 0x100 * serdes + register        (the observation space the waits poll)
```

i.e. the same `base + stride*index + offset` shape as every MMIO macro in the header. The earlier
reading of `0xd11` as "space `0x0d`, device `serdes + 0x11`" was **wrong** — it is a base and a
stride, and the device number never appears in the SDK address at all.

### ⚠ CORRECTION: "our SBus writes do not take effect" was overclaimed

The previous entry recorded that as answered. **It is not.** Two tests on the working Et2 lane:

| test | PORT_STATUS |
|---|---|
| clear `rx_en` + `tx_en` (`0x22 = 0x00`) | `0x08c0` → `0x08c0` — no change |
| clear `tx_output_en` (`0x0d = 0x00`) | `0x0cc0` → `0x08c0` → `0x08c0` |

That second one *looks* like an effect and is not: bit 10 is `Receiving`, an **activity** bit that
toggles by itself — Et1 was seen at `0x08c0`, `0x0cc0` and `0x0ec0` within seconds, untouched. The
stable bits — `RxLinkUp` b6, `HeartbeatOk` b7, `SerXmit` b11 — **did not move in either test**.

So the writes produce no observable effect, but the remap table has now removed the explanation that
made "the writes are not landing" attractive: **our device numbers were right all along.** The
competing reading is that these SBus controls simply do not govern a link that is already running —
the EPL-level `SERDES_RX_CFG.RxEn` / `SERDES_TX_CFG.TxEn` may be the master gate, with the SBus
enables mattering only during bring-up.

Both readings survive the evidence. Distinguishing them needs a write whose effect is unambiguous on
a *running* link — the TX pattern generator is the obvious candidate, since a lane transmitting PRBS
cannot carry traffic — or a way to read back the write-register space, which is what
`0xd1100 + 0x100*serdes` addressing may exist to provide.

### ⛔ CONFIRMED, and narrowed: writes to a SerDes device do nothing; writes to the SPICO work

The correction above asked for a write whose effect on a running link is unambiguous. Run on Et2:

| write | control | PORT_STATUS over 12 s |
|---|---|---|
| `0x22 = 0x00` | clear `rx_en` + `tx_en` | `0x08c0` unchanged |
| `0x0d = 0x00` | clear `tx_output_en` | stable bits unchanged |
| `0x07 = 0x80` | `rx_pattern_cmp_en` | `0x0f` stayed `0x3f` (inconclusive by nature) |
| **`0x0a = 0x80`** | **`tx_pattern_gen_en`** | **`0x08c0` unchanged, RxLinkUp/HeartbeatOk/SerXmit all 1** |

The last is decisive. **A lane transmitting PRBS cannot carry traffic** — had that write landed, the
Edgecore at the far end of the DAC would have lost sync and the link would have dropped well inside
twelve seconds. It did not flicker.

**So writes addressed to a SerDes device take no effect** — and this is no longer explicable by bad
addressing, because `fm6000SerDesRemapTable` confirms our device numbers, the register is the low
byte of the SDK address, and the command word we build is byte-identical to the ones EOS itself
issues in `fwd4-stock.txt` (`0x01214a17` = exec | op `0x21` | dev `0x4a` | reg `0x17`).

### ★ The narrowing that matters

**Writes to the SPICO broadcast device `0xfd` DO take effect.** `fm6000_sbus irq` writes regs `0x01`,
`0x02`, `0x03` and `0x0c` to device `0xfd` and gets back *per-target* answers — `2` for both working
lanes, `1` for Et3, `0` for lanes with nothing plugged in. Those responses cannot vary by target
unless the reg-`0x03` write that names the target is landing.

So the split is not read-versus-write, and not our command encoding. It is **which device answers**:

```
device 0xfd  (SPICO broadcast)   writes land, reads land
device 0x45 / 0x49 / 0x4a        reads land, writes do not
```

That is a much sharper question than "why is port 3 dark", and it suggests the individual SerDes
devices are not accepting bus writes in the state our boot leaves them — needing an unlock, an
owner, or an SBus-master initialisation that EOS performs and we do not. `fm6000_initsbus` already
exists in the tree and is the obvious first place to look; `fm6000WriteSBus`'s two
`fmRegCacheUpdateSingle1D` calls and its unnamed internal at `aristaFmGetNextHopUsed@@Base+0x12a4`
are the second.

⚠ One reading not excluded: that these particular controls are overridden while a lane is running,
so all four writes landed and none could show. It is strained — `tx_pattern_gen_en` overridden on a
live link would be a strange design — but it has not been disproven, and the same four writes on the
*dark* lane produced no state change either, which is consistent with both.

### Both cheap explanations eliminated — and a better hypothesis

**1. The bus is not reporting an error.** `fm6000_sbus` now prints the command register after each
transaction. SPICO and SerDes devices are indistinguishable:

```
reads   cmd=0x1122....  result=4     (0xfd, 0x45, 0x4a alike)
writes  cmd=0x0521....  result=1     (0xfd, 0x45, 0x4a alike)
```

And `fm6000_initsbus.c`'s own header already documented what those mean —
`[28:26]=ResultCode(reset0/write1/read4)`. They are **op-completion codes, not status**. A write to a
SerDes device completes exactly as a write to the SPICO does. The hardware is not refusing us.

**2. The SBus master is initialised.** `fm6000-fullseq.sh` runs `fm6000_initsbus` at STEP2 of every
boot. A missing bus init is not the explanation either.

### ★ The SPICO owns these registers

What is left is in our own tree, in `fm6000_spico.c`:

> *without the SPICO running, the SerDes RX equalizer never adapts and ports don't train*

and in `fm6000-fullseq.sh`: the replay uploads the SPICO firmware inline and **later resets and
starts the SPICO**, which is why a separate early upload gets wiped. So on a booted chip the
micro-controller is *running* and actively driving the SerDes lanes.

That fits every observation at once:

- writes to device `0xfd` take effect — that is the controller itself, and it is listening;
- writes to `0x45`/`0x49`/`0x4a` complete on the bus and change nothing — the running SPICO drives
  those control registers and overwrites, or simply outranks, whatever we put there;
- EOS gets away with the same writes because the SDK manages the controller around them —
  `fm6000SetSpicoState` exists for exactly that, and `fm6000EnableSerDes` runs inside a capture lock;
- and it explains the shape of the DFE work: the procedure that *did* produce a captured effect was
  invoked as a **SPICO interrupt**, not as register writes.

**Next: read `fm6000SetSpicoState` (`0x491e13`, 0x19e bytes) and the two `fm6000InterruptSpico` call
sites inside it.** If the lane-enable has to be asked of the SPICO rather than written behind its
back, that is the shape of the fix — and `fm6000_sbus irq` already speaks that protocol.

⚠ Still unexcluded, and cheap to test alongside: that these controls are inert on a lane whose EPL
block has not enabled it, so nothing we write to a *dark* lane could show regardless. The Et2
`tx_pattern_gen_en` result argues against it for a *running* lane, which is why it is not the
leading reading.

### ⚠ The SPICO-ownership hypothesis is weakened, and one claim retracted

**`fm6000SetSpicoState` decoded.** It is a single interrupt:

```
fm6000SetSpicoState(sw, state)  ->  fm6000InterruptSpico(sw, code 0x0f, param = !state, 50000)
```

so **SPICO run-state is interrupt code `0x0f`**, and it takes no lane argument — it is global.

**But nothing in the SDK calls it, and `fm6000EnableSerDes` does not halt the SPICO.** EOS enables a
lane with direct SBus writes *while the controller is running*, and those writes work for EOS. So
"the SPICO overwrites what we write" no longer explains the difference between EOS and us. The
hypothesis is not dead — the controller is still the obvious owner of these registers — but its main
support is gone.

**Retraction.** The `tx_pattern_gen_en` test was called decisive; as run, it was not. Writing
`0x0a = 0x80` set the enable while clearing `tx_pattern_gen_sel_cntl` (b4-5) and
`tx_pattern_gen_ctl_cntl` (b6), so the generator may simply not have been configured to emit.

**Re-run properly, and the conclusion holds.** `0x0a` = `0x90`, `0xd0`, `0xf0` on Et2 — enable plus
control plus all three select values, four seconds apart:

```
before      RxLinkUp=1 HeartbeatOk=1 SerXmit=1
0x90        RxLinkUp=1 HeartbeatOk=1 SerXmit=1
0xd0        RxLinkUp=1 HeartbeatOk=1 SerXmit=1
0xf0        RxLinkUp=1 HeartbeatOk=1 SerXmit=1
```

A configured pattern generator on a live 10GBASE-CR lane would break it. None of the three did.

**And the fine instrument agrees.** The SPICO lane-state readout, the one thing that has ever
distinguished these lanes, is unmoved by every SerDes write made today:

```
0x49 Et1 = 2    0x45 Et2 = 2    0x4a Et3 = 1    0x4b empty = 0
```

Et3 still reads `1` — the state our EPL provisioning put it in — after `rx_en`, `tx_en`,
`tx_output_en` and the pattern generator were all written to it.

### Where that leaves the question

Writes to a SerDes device complete on the bus, report `result=1` exactly as SPICO writes do, and
change nothing measurable by any of the four instruments available (PORT_STATUS stable bits, the
read-space observation registers, a live link's survival, the SPICO lane state). EOS issues
byte-identical command words and gets working lanes.

**Next, and it needs a decision because it carries risk:** re-run `fm6000_initsbus` on the booted
chip and repeat one write test. The replay's own SBus writes work at boot; if the master is left in
a state afterwards where writes are absorbed, a re-init would restore them and that is the whole
answer. ⚠ It re-initialises the bus that Et1 is running over, and Et1 carries the OSPF adjacency —
so this is a deliberate step, not a casual one.
