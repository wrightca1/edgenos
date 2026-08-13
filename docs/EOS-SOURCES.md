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
