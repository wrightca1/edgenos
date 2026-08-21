# FM6000 SerDes SPICO firmware — reverse engineering notes

**Status 2026-08-06.** The *delivery protocol* is fully decoded and we can reconstruct the firmware
image from any register trace. The *instruction set* is not yet recovered.

Tool: `asic/fm6000/tools/spico_extract.py` (ours; bundles no firmware).

---

## 1. What SPICO is

A small microcontroller embedded in the FM6000 SerDes. Its firmware runs the SerDes housekeeping —
notably RX equaliser adaptation. It is Intel's code, shipped inside `libFocalpointSDK.so`, and it is
the one blob we cannot redistribute. It is **not required** for a 10GBASE-SR link (proven by
bisect), which is why it is not in the runtime path today.

## 1.1 Measured 2026-08-21: what actually works with NO Intel firmware

We have no Intel redistribution permission, so this was tested directly by moving
`/mnt/flash/fm6000_spico_code.bin` aside and cold-booting alpha47. STEP5b degrades silently, as
designed. Result:

| port | media | PORT_STATUS / LANE_STATUS | outcome |
|---|---|---|---|
| et1 | **SFP, fiber (10GBASE-SR)** | `000008c0` / **`00000940`** | clean lock, **forwards** |
| et2 | **DAC, copper (10GBASE-CR)** | `00000815` / `00000000` | no lock |

et1 carried traffic end to end with zero Intel code present: 5/5 pings from the peer, OSPF
adjacency up, `fibd: programmed 14 route(s)`. **A fiber-only EdgeNOS needs no Intel firmware at
all**, which is what makes distribution possible.

### The DAC failure is NOT a missing "start tuning" trigger

Tested on the running no-SPICO box, driving the SerDes over SBus from the host:

- **PLL lock and signal detect are IDENTICAL on both ports** — reg `0x0f` = `0x3f` and reg `0x14`
  = `0x14` on dev `0x45` (et2) and `0x49` (et1). The DAC is delivering signal; this is not a
  cabling or squelch problem.
- The DFE tune state machine **responds to host driving without SPICO**. Toggling reg `0x2a`
  `0x16`/`0x0e` and polling `0x2b` reaches the completion code `0x04` within 3 iterations, and
  et2's `0x2b` can be brought to `0x03` — the *same value the working fiber port holds*.
- **LANE_STATUS stays `0x00000000` regardless.** `0x2b` also oscillates (`0x05` → `0x02` → `0x04`
  → `0x03`), which reads as adaptation running without converging.
- SPICO's control register (dev `0xFD` reg `0x0c`) reads `0` on a stripped boot, so the lane is
  **not** being held down by an empty SPICO left running — that hypothesis is disproved.

Conclusion: SPICO performs **continuous RX equaliser adaptation**, not a one-shot trigger we can
substitute with a start pulse. Replacing it for copper means implementing an adaptive equaliser on
the host — reading eye/error metrics and driving the taps (`tap amp (K0)`, `tap pre (KM1)` exist as
named SerDes fields in the SDK descriptor table) — which is a research problem, not a port of an
algorithm we already have.

⚠ The reg-`0x2b` field decode below is INFERRED. The SDK's field-name table is located but its
group-to-register association is unsolved (see `sdk_regmap.py`), so the mapping of that group to
`0x2b` is a hypothesis consistent with observed values, not a proven layout:

    bit 0    Tune enabled          bits 4-5  Coarse status
    bit 1    Reserved0             bit 6     Single fine loop done
    bits 2-3 Fine status

### ★ SPICO IS ONLY NEEDED TO *ACQUIRE* LOCK, NOT TO MAINTAIN IT

Measured 2026-08-21 on a running alpha47 with both ports up. Putting SPICO into reset
(`dev 0xFD reg 0x0c <- 3`) and leaving it stopped:

| test | SPICO running | SPICO in reset |
|---|---|---|
| transit through et2 (DAC) | 6 frames | **6 frames** |
| paced load through et2 | ~0.25% loss | **1992/2000, 1997/2000** |

The DAC link keeps full performance with the DSP stopped. What SPICO leaves behind is a
small block of **equaliser coefficients latched in hardware**. Diffing the DAC lane
(`0x45`) against the fiber lane (`0x49`) shows 21 differing registers, of which
`0x1f`-`0x28` is a contiguous dual-bank block (`0x21`/`0x25` identical, `0x23`/`0x27`
identical) — the taps. They visibly drift while SPICO runs and freeze when it stops.

**This changes what "replace SPICO" means.** We do not need to reimplement Intel's
processor or recover its instruction set. We need to put the right numbers in
`0x1f`-`0x28` before the lane trains. Two routes, both free of Intel code:

1. **Search.** Sweep the tap block until the lane acquires. A bounded search over a small
   register block, not an ISA-recovery project.
2. **Learn once, on the operator's own switch.** Run the vendor firmware a single time from
   the operator's licensed EOS, capture the converged taps, store them, and boot without it
   thereafter. This is exactly the established publication standard — *the mechanism ships,
   the numbers are regenerated on the operator's own hardware* — the same shape as
   `mkconfigbcm.sh`/`mkpolarity.sh` on the 7050SX2.

### The tap-injection experiment: RUN, and it FAILED

Run 2026-08-21. Captured the converged taps from a working DAC lane
(`1f=25 20=c8 21=38 22=d8 23=50 24=48 25=38 26=fa 27=50 28=5b`), cold-booted with the blob
absent, and wrote them into the down lane. **The writes did not stick** -- every register
read back unchanged (cold defaults are a clean dual-bank `c0 80 00 10` / `c0 80 00 10`).

Then a volatility-controlled writability scan of the WHOLE SBus register space
(`0x00`-`0xff`, dev `0x45`): read twice to exclude volatile registers, write `0x5a`, read
back. **Not one register accepted the write.** (`0x00` reported writable but its original
value was already `0x5a` -- a false positive.)

The SBus tool is not at fault: `fm6000_sbus write` issues `OP_WRITE 0x21` and then prints a
*readback* line, which is why its output shows op `0x22` -- and the DFE tune loop provably
takes effect through the same path, driving `0x2b` to its completion code.

**Best explanation:** the registers at `0x1f`-`0x28` are the `_obs` (observe) copies, and
the hardware tuning engine owns them. The SDK's own field names come in pairs --
`sbus_dfe_a_ctl_1_cntl` / `sbus_dfe_a_ctl_1_obs`, `sbus_dfe_a_adv_cntl` / `_obs`, and so on
for ctl_2..ctl_4 -- so a writable **control** side exists. We have not located it, and with
`Tune enabled` set the engine plausibly overwrites any host value immediately.

### ★ 2026-08-21: the SerDes register names are recovered, and they CORRECT us

`asic/fm6000/tools/sdk_fieldmap.py --sbus` recovers named field layouts for **54
SBus registers** from `fm6000DbgGetEthWriteRegFields`, an if-chain whose every arm
names the register by NUMBER (`cmp [ebp+8], <reg>` then `lea` of its field group).

    0x1f  sbus_dfe_a_adv_cntl[3], sbus_dfe_a_dac_cntl
    0x21  sbus_dfe_a_ctl_1_cntl[3], sbus_dfe_a_ctl_2_cntl, sbus_dfe_a_ctl_3_cntl
    0x22  sbus_rx_en_cntl, sbus_tx_en_cntl, sbus_ignore_broadcast_cntl
    0x2a  sbus_dfe_scratch_pad_cntl
    0x2b  sbus_dfe_scratch_pad_cntl
    0x2e  sbus_dfe_a_ctl_i_cntl, sbus_dfe_a_ctl_1_cntl

⚠ **`0x2a` AND `0x2b` ARE A SCRATCH PAD, NOT A TUNE ENGINE.** The "DFE tuning"
loop in `../asic/fm6000/fm6000_serdes_enable.c` -- write `0x2a` = `0x16` then
`0x0e`, poll `0x2b` for `0x04` -- is not driving hardware. It is a **mailbox to
the SPICO firmware**: the host posts a command in the scratch pad, SPICO reads it,
runs the adaptation, and posts status back.

That single fact explains every earlier observation at once:

- the loop "completed" (reached `0x04`) whenever SPICO was loaded -- SPICO was
  answering the mailbox;
- on a stripped boot it oscillated and never converged -- **nobody was servicing
  the mailbox**;
- and getting `0x2b` to read `0x03`, the same value the working lane holds, meant
  nothing, because it was only ever a message, not tuning state.

**So the DFE adaptation genuinely IS Intel's algorithm running on Intel's core**,
reached over a scratch-pad IPC. It is not a hardware state machine we can start
ourselves. That closes off the cheap route.

⚠ Also corrected: SerDes control registers **are** host-writable, but only on a
**live** lane. The earlier sweep that concluded "not one register accepts a write"
was run on a cold, unclocked lane. On a live lane a write to `0x22` stuck
immediately and destabilised the link (recovered by reboot; `0x22` reads `0xea`
normally). Do not repeat that probe on a working port.

### The host-side adaptation attempt: written, and BLOCKED (2026-08-21)

`asic/fm6000/fm6000_dfe_adapt.c` implements the loop -- it decodes all seven DFE-A
parameters (including the `[3]` fields whose top bit lives in a *different*
register from its low three, an easy way to silently search a quarter of the
space), measures, and coordinate-descends. It is blocked on two measured facts,
neither of which is the search itself:

**1. The DFE controls reject writes on a cold lane.** A known-good profile
captured from this same DAC cable while it was up
(`ctl_2=12 ctl_3=1 ctl_4=4 adv=14 dac=18`) was injected into the cold lane and
**nothing moved** -- every value needing a change read back unchanged; only the
two already at target "stuck". The DFE power-down bits (`0x26` offset_pd /
data_pd) are already clear on both lanes, so that is not the gate. On a **live**
lane a write does land -- confirmed by accidentally destabilising et2 with one --
so writability depends on some lane state we have not identified, plausibly
something SPICO establishes itself.

**2. The error counter is the wrong metric for acquisition.** `0x08` reads 0 on a
DEAD lane exactly as on a healthy one: with no lock there is no decoded data to
count errors in. What separates them is `0x01` -- **`0x25` working vs `0x00`
dead** -- i.e. `sbus_rx_8b10b_comma_det_obs`. A future search must score comma
detect first and use the error count only as a tiebreaker.

Parameter values for reference, captured with both lanes up (they differ exactly
as a copper and a fiber channel should, which is the check that the decode is
right):

| lane | ctl_1 | ctl_2 | ctl_3 | ctl_4 | adv | dac | ctl_i |
|---|---|---|---|---|---|---|---|
| et2 DAC | 4 | 12 | 1 | 4 | 14 | 18 | 4 |
| et1 fiber | 4 | 7 | 0 | 15 | 15 | 20 | 4 |
| et2 cold, no SPICO | 4 | 0 | 4 | 0 | 6 | 0 | 4 |

**The ordering is now clear: making the DFE block writable on a cold lane is the
prerequisite.** Until that is solved a search has nothing to actuate.

⚠ Do not trust `0x22` (`sbus_rx_en_cntl` / `sbus_tx_en_cntl`) readings: it read
`0xea` on one boot and `0x00` on the next *on a working lane*, so either the
field mapping or the read path for that register is not what it appears.

### ★ Disassembly, 2026-08-21: what the SDK actually does

Four findings, from `objdump` on the SDK. The first settles a question we had only
inferred; the rest change what a replacement would have to look like.

**1. "Start DFE tuning" is a mailbox post, proven from vendor code.**
`fm6000StartSerDesDfeTuning` @0x4877a1 is small: one `SetSerDesRxDataGate`, one
`ReadSBus`, three `WriteSBus`. Its register addresses are built as
`(lane << 8) + base` with bases `0xb0517/0xb052a/0xb052b` or
`0xd1117/0xd112a/0xd112b` -- i.e. SBus registers **0x17, 0x2a and 0x2b only**.
0x2a/0x2b are the scratch pad. So the SDK starts tuning exactly the way our
inherited loop does: by posting to SPICO. There is no hardware tuning engine
anywhere in this path.

**2. But the coefficients ARE host-writable -- `fm6000SetSerDesDfeParams`
@0x48c29b.** It addresses the full set: `0x1f, 0x20, 0x21, 0x2e` (bank A),
`0x23, 0x24, 0x25, 0x2f` (bank B) and `0x26`, and writes each with its own
`WriteSBus`. So placing coefficients from the host is supported by the silicon;
our failure to do so is a defect in HOW we write, not proof that we cannot.

**3. ⚠ THE COEFFICIENTS ARE GRAY-CODED.** `fm6000SetSerDesDfeParams` calls
`fmConvertBinaryToGray` **six times** -- once per tunable parameter -- before
writing. (`fmConvertBinaryToGray` @0x4c5810 is the standard `g = b ^ (b >> 1)`,
done as a 64-bit loop.) **Writing binary values would be wrong even if they
landed**, and any search that treats the raw register value as a magnitude is
searching a scrambled space.

**4. ⚠ THERE ARE TWO SERDES REGISTER BLOCKS, `0xd11xx` AND `0xb05xx`,** selected
by a function argument, with identical register offsets. Everything we do goes
through `fm6000_sbus`'s `(op, dev, reg)` form, and `fm6000_serdes_enable.c`
asserts that is equivalent to `(lane << 8) + 0xd11RR`. That equivalence is an
ASSUMPTION and has never been tested against the `0xb05xx` block. If our dev
numbers address the wrong block, reads would still return plausible values while
writes went nowhere -- which is exactly the symptom we have.

Related functions worth reading next: `fm6000ClearSerDesDfeGates` @0x48e45e,
`fm6000SetSerDesDacOffset` @0x485fa6, `fm6000EnableSerDes` @0x48131e (which also
touches 0x1f and 0x26), and `fm6000GetPortLaneEyeScore` -- an EYE SCORE, a far
better acquisition metric than the error counter, which reads 0 on a dead lane.

⚠ Cold-lane register poking has hit diminishing returns and produces unreliable
readings: 0x26 never accepts a write, and 0x21 responded to a write by going to 0
rather than to the written value. Stop poking; read code instead.

### ★★ THE COMPLETE HOST-SIDE DFE RECIPE, read out of the SDK (2026-08-21)

Reading the whole subsystem rather than one function produced an actual
procedure. Everything below is derived from vendor code, not guessed.

**Addressing is confirmed, and our usage was right.** `fm6000WriteSBus(sw, addr,
val)` decomposes `addr` as **`dev = (addr >> 8) & 0xff`, `reg = addr & 0xff`**
(disassembled at 0x479f23). So `(lane << 8) + 0xb051f` with lane `0x40` gives
dev `0x45`, reg `0x1f` -- exactly what `fm6000_sbus` does. The `0xb05` vs `0xd11`
blocks differ only in the device byte (`0x05+lane` vs `0x11+lane`), and our
et2=0x45 / et1=0x49 / port3=0x4a sit in the `0xb05` (Ethernet) block, lanes
0x40/0x44/0x45. **The "wrong block" worry recorded earlier was unfounded.**

**The write path, from `fm6000SetSerDesDfeParams` @0x48c29b:**

1. Gray-code each parameter -- `fmConvertBinaryToGray` @0x4c5810, the standard
   `g = b ^ (b >> 1)`. Six calls, one per tunable.
2. Write the eight coefficient registers, one `WriteSBus` each:
   bank A `0x1f, 0x20, 0x21, 0x2e`; bank B `0x23, 0x24, 0x25, 0x2f`.
3. **THEN commit via `0x26`** -- read-modify-write, and this is the step we never
   did:

       bit 0  sbus_rx_dfe_gate         <- caller's flag
       bit 2  sbus_sel_dfe_a_data_cntl <- FORCED SET (`or eax,0x4` at 0x48d66c)

`sbus_sel_dfe_a_data_cntl` selects the host-supplied data path. Writing
coefficients without setting it leaves the hardware using its own values, which
is consistent with every failed injection: the writes may well have landed, and
the readback simply kept showing the value actually in use.

**The counterpart, `fm6000ClearSerDesDfeGates` @0x48e45e**, hands control back:
clears bit 0 of `0x0d` (`sbus_tx_pre_emphasis_gate`), bit 0 of `0x26`
(`sbus_rx_dfe_gate`), and bits 5 and 6 of `0x17` (the analog-to-core gates). So
`_gate` bits arbitrate between hardware/SPICO and host-supplied `_cntl` values --
which is the general rule this whole register file is built on.

⚠ **UNTESTED.** This recipe has not been run on hardware. What it predicts is
that coefficients can be placed from the host if, and only if, the `0x26` commit
follows them. That is the next experiment, and it is a much narrower one than
"implement an adaptive equaliser".

**Still missing for a full replacement:** the adaptation *algorithm*. Even with
coefficient placement working, something must choose the values. Note
`fm6000GetPortLaneEyeScore`, `fm6000GetPortLaneEyeScoreHeight/Width` exist -- an
eye score is the right metric, and far better than the error counter at `0x08`,
which reads 0 on a dead lane because there is no decoded data to count.

**Map of the subsystem, for whoever reads this next:**

| function | address | what it does |
|---|---|---|
| `fm6000StartPortLaneDfeTuning` | 0x430069 | entry; maps lane, stops old tuning, PcsRxReset, SetSignalDetection, then StartSerDesDfeTuning |
| `fm6000StartSerDesDfeTuning` | 0x4877a1 | **mailbox only** -- regs 0x17, 0x2a, 0x2b |
| `fm6000SetSerDesDfeParams` | 0x48c29b | the 8 coefficients + the `0x26` commit |
| `fm6000GetSerDesDfeParams` | 0x48d7ea | readback |
| `fm6000CheckSerDesDfeTuningState` | 0x48d9e2 | |
| `fm6000ClearSerDesDfeGates` | 0x48e45e | returns control to hardware |
| `fm6000WaitForSerDesPllLock` | 0x48eebb | |
| `fm6000SetSerDesDacOffset` | 0x485fa6 | regs 0x1f, 0x26 |
| `fm6000EnableSerDes` | 0x48131e | bring-up; also touches 0x1f and 0x26 |
| `fm6000WriteSBus` | 0x479e09 | dev/reg decomposition |
| `fmConvertBinaryToGray` | 0x4c5810 | `b ^ (b>>1)` |

### The recipe was IMPLEMENTED and TESTED. Partial movement, still no lock.

`asic/fm6000/fm6000_dfe_adapt.c` now Gray-aware, does the `0x26` commit
(`sel_dfe_a_data_cntl` forced set), and scores on comma-detect rather than the
useless error counter. Run on a cold DAC lane with no Intel firmware, injecting
coefficients captured from the same cable while it was up:

    baseline score = 1000 (err=0 comma=0)     <- metric behaves as designed
    set ctl_1 = 4  (readback 4)
    set ctl_2 = 12 (readback 0)   DID NOT STICK
    set ctl_3 = 1  (readback 4)   DID NOT STICK
    set ctl_4 = 4  (readback 0)   DID NOT STICK
    set adv   = 14 (readback 14)  <- STUCK, and did not on the previous attempt
    set dac   = 20 (readback 0)   DID NOT STICK
    after set+commit: 1f=01 20=a9 21=c0 2e=88 26=00  status01=04
    et2 LANE=0x00000000

**What moved:** `adv` took this time; `0x20` went `c0`->`a9` and `0x21` `80`->`c0`,
so writes DO reach these registers. `status01` moved `00`->`04`.

**What blocks it:** ⚠ **`0x26` reads `00` after the commit -- the commit write
itself does not land.** Without it `sel_dfe_a_data_cntl` is never set, so whatever
coefficients do land are not the ones the hardware uses. The values that "stick"
are also not reliably the ones written (`0x21` changed, but to neither the old nor
the requested value), consistent with the hardware still driving these bits.

**So the open question is now precise: what makes `0x26` writable?**
`fm6000EnableSerDes` @0x48131e writes it during bring-up, and sets up the whole
lane register set -- `0x03, 0x06, 0x0d, 0x17, 0x1d, 0x1f, 0x22, 0x26, 0x36, 0x3b`.
**We have that algorithm implemented already** in `asic/fm6000/fm6000_serdes_enable.c`
(recovered from the same function) but it is NOT shipped in the image.

**The next experiment is therefore bounded and concrete:** ship
`fm6000_serdes_enable`, run it on the cold lane to put the SerDes in the state the
SDK expects, then inject coefficients and commit. If `0x26` accepts a write after
a proper enable, the whole recipe follows.

⚠ Also settled: **the eye score is not available to us.**
`fm6000GetSerDesEyeScore` @0x48a97d goes through `fm6000InterruptSpico` -- it is a
SPICO service. Any search must acquire on `sbus_rx_8b10b_comma_det_obs`
(0x01 bit 5) and only then refine on the error counter.

### Ran fm6000_serdes_enable first, then injected. Still no lock -- and a
### METHODOLOGICAL ERROR that invalidates several earlier conclusions.

`fm6000_serdes_enable` was shipped to /mnt/flash and run on the cold DAC lane
(port 2, dev 0x45) with no Intel firmware, to put the SerDes in the state the SDK
expects before injecting. It ran correctly:

    rmw  reg 06  f0 -> f8 ; reg 03  be -> bf ; reg 1f  01 -> 01
    rmw  reg 26  00 -> 01           <- it DOES write the gate register
    rmw  reg 0d  a4 -> b5
    wait reg 14 -> d4 after 1 ms    <- signal detect OK
    dfe  reg 2a <- 0e, reg 2b <- 02 <- mailbox post
    dfe  state=0 TIMEOUT after 3000 ms   <- nobody answers it, as expected
    SerXmit SET -- lane is transmitting

Then coefficients + commit: `0x26` still read `00`, coefficients still "did not
stick", `LANE_STATUS = 0x00000000`.

⚠⚠ **THE READBACK TEST IS NOT VALID, AND SEVERAL EARLIER CONCLUSIONS RESTED ON
IT.** `0x26` reads `00` *immediately after* `fm6000_serdes_enable` wrote `01` to
it, and after a direct write of `0x05`. These registers do not read back what was
written -- most likely reads return the `_obs`/in-use side while writes go to the
`_cntl` side, exactly as the `_cntl`/`_obs` naming implies.

**Therefore "DID NOT STICK" never meant "the write failed".** It may have meant
"the write landed and you cannot see it". Every conclusion in this file of the
form *"the coefficients are not host-writable"* is downgraded to **unproven**.
The only trustworthy ground truth here is **does the lane lock** -- and with
converged coefficients injected after a full SerDes enable, it does not.

**Which leaves the more likely explanation:** SPICO's role in *acquisition* is not
merely to place final coefficients. A converged snapshot is the OUTPUT of an
adaptation process run against a live signal; replaying it into a cold lane need
not reproduce the state the receiver has to pass through to acquire. That is
consistent with everything observed, including the fact that SPICO can be stopped
once a link is up with no ill effect at all.

**What would actually settle it:** an independent way to tell whether a write
landed -- e.g. write a coefficient on a LIVE, locked lane and watch for a
measurable change in the error counter or in link behaviour. That separates
"cannot place" from "placement is not sufficient", which is the question this work
has repeatedly failed to distinguish.

### ★★ SETTLED: THE COEFFICIENTS *ARE* HOST-WRITABLE. The missing step is a PCS
### re-lock, not equalisation. (2026-08-21)

The experiment that finally separates "cannot place" from "placement is not
sufficient", run on a LIVE, locked DAC lane with **SPICO stopped** so it could not
re-adapt over the writes:

| step | LANE_STATUS | st01 | err |
|---|---|---|---|
| SPICO running | 0x940 | 0x25 | - |
| SPICO stopped | 0x940 | 0x24 | 0 |
| baseline x3 windows | 0x940 stable | 0x24 | 0, 0, 0 |
| **write dac=0** | **0x000** | 0x24 | 0 |
| all coefficients 0 | 0x000 | 0x24 | 0 |
| restore good coefficients | 0x000 | **0x24** | 0 |

**Writing a bad coefficient BROKE a working link.** LANE_STATUS had been stable at
0x940 across three baseline windows with SPICO already stopped, so nothing else
was driving it. **Host writes land and have real physical effect.** Every earlier
claim in this file that the coefficients "are not host-writable" is now
**DISPROVED** -- they rested on readback, which is not a valid test here.

Two further facts from the same run:

- On a LIVE lane the `0x26` commit IS visible -- it read back `0x82`. On a cold
  lane it reads `00`. So writability (or at least observability) genuinely differs
  with lane state, which is why the cold-lane experiments were so confusing.
- Readback of individual fields is unreliable even when the write works: `dac`
  reported "DID NOT STICK (readback 18)" in the very same command that took the
  link down.

★ **AND THE RECOVERY PATH IS NARROWER THAN FEARED.** Writing the known-good
coefficients back did NOT restore LANE_STATUS -- but `st01` bit 5
(`sbus_rx_8b10b_comma_det_obs`) **is set**. The SerDes is recovering symbols
again; it is the **PCS** that has not re-locked. So what is missing after a
coefficient change is a PCS re-lock, NOT better equalisation.

That is consistent with the SDK: `fm6000StartPortLaneDfeTuning` calls
**`fm6000SetPcsRxReset`** and `fm6000SetSignalDetection` around its tuning step.
`fm6000SetPcsRxReset` @0x435d82 maps a logical port to an EPL channel and
manipulates **bit 17 (`and eax,0x20000`)** of a per-channel register at offset
`channel * 0x64 + 0x8c` within its block; the full base was not resolved.

**Next experiment:** restore good coefficients, then toggle PCS RX reset (or
re-run the port's PCS bring-up) and see whether the lane re-locks. If it does,
host-driven coefficient control is proven end to end on a live lane, and the
remaining question shrinks to cold-lane ACQUISITION -- for which comma-detect is
the metric, since the eye score is a SPICO service.

### PCS re-lock tried too. Static coefficient placement is NOT sufficient.

The EPL register map is now fully named (`sdk_fieldmap.py`), which gave the exact
control:

    LANE_CFG      0xe0037 (+0x4000 for et2)   bit 23 ResetRx, bit 24 ResetTx
    LANE_STATUS   0xe0038   bit 6 PcsBaserBlockLock, bits 7-12 RxRate
    PORT_STATUS   0xe0000   bit 6 RxLinkUp, 8 HiBer, 9 Transmitting, 11 SerXmit
    PCS_10GBASER_RX_STATUS 0xe0026  bit 0 BlockLock, bit 1 HiBer, bits 2-9 BerCnt

(So `LANE_STATUS = 0x940` decodes as PcsBaserBlockLock=1, RxRate=18 -- the "0x940"
we have used as a magic number for months is simply block lock plus rate.)

Full sequence on a live lane, SPICO stopped: break the link with bad coefficients
(LANE 0x940 -> 0x000, reproducible), write the good coefficients back, then pulse
`LANE_CFG.ResetRx`. Result after 15 s: **LANE_STATUS stays 0x000.**

`0x01` reads `0x35` throughout the failed recovery: `comma_det` SET,
`slip_in_progress` SET, `error_occurred` SET. **The receiver is actively trying to
align and failing** -- it is not idle, and it is not starved of signal.

**Conclusion: replaying a converged coefficient snapshot does not restore a
receiver.** Placement works (proved by breaking the link) and PCS reset is
available, but the DFE evidently carries adaptation state beyond these register
values, or must converge against the live signal rather than be told the answer.
That is the same conclusion the acquisition experiments reached from the other
direction, now with the PCS excuse removed.

**Practical consequence: replacing SPICO requires implementing the ADAPTATION,
not just coefficient placement** -- and the natural metric, the eye score, is a
SPICO service (`fm6000GetSerDesEyeScore` -> `fm6000InterruptSpico`). A search
would have to run on comma-detect plus the 8-bit error counter, both of which are
weak, and would have to converge on a live link it keeps breaking.

**This line of attack is at a natural stopping point.** The distribution position
is unchanged and unaffected: fiber ships with zero Intel firmware; copper DAC
needs an operator-supplied blob.

**Next leads, in order of cost:****Next leads, in order of cost:****Next leads, in order of cost:****Next leads, in order of cost:****Next leads, in order of cost:****Next leads, in order of cost:****Next leads, in order of cost:****Next leads, in order of cost:****Next leads, in order of cost:**

1. ~~Clear `Tune enabled` and retry injection.~~ **Tried, failed** -- writing `0x2a`
   no longer moves `0x2b`, because `0x2a` is a mailbox, not a control.
2. ~~Locate the `_cntl` registers.~~ **DONE** -- `sdk_fieldmap.py --sbus`, above.
   `0x1f`, `0x21`, `0x2e` are the DFE controls; `0x22` is rx/tx enable.
3. **The remaining route is now clear, and it is the expensive one.** Writing the
   DFE controls directly means implementing the adaptation loop ourselves: choose
   coefficients, apply, measure the resulting eye/error rate, iterate. The
   registers to drive are known; the algorithm is not, and SPICO's version of it is
   the part we may not have. Alternatively, recover SPICO's instruction set -- 6,000
   words of 10-bit microcode -- and write our own firmware.

⚠ What is still SOLID: SPICO is dispensable once the link is up (transit and load both
normal with the DSP held in reset). The obstacle is purely *acquisition* -- getting the
right coefficients in place on a cold lane without Intel's code.

⚠ `LANE_STATUS 0x940` alone proves nothing here. During this work a lane read `0x940` with
`PORT_STATUS 0x8c0` and `pcsRx=1` and still forwarded nothing, because portd was not
running. Always confirm with `tools/transit-test.sh`, never with link registers alone --
and note that pinging et2's own address from the peer fails even on a healthy link, so it
is not a valid probe.

### What this means for distribution

**Fiber: ship it.** No Intel firmware, no operator-supplied blob, full function.

**Copper DAC: operator-supplied.** The same "bring your own, from a licensed EOS" model already
used for `fwd4.txt` — the image must degrade to "copper down", never to a failed boot, which
STEP5b already does.

## 2. Delivery protocol — fully decoded

The IMEM upload is a plain SBus transaction sequence on receiver `0xFD`. Per word:

```
reg 0x04 <- addr[15:8]
reg 0x05 <- addr[7:0]
reg 0x07 <- data[7:0]
reg 0x06 <- data[9:8] | 0xC     bit3 = IMEM write enable, bit2 = strobe
reg 0x06 <- data[9:8] | 0x8     strobe released
```

> **The IMEM word is 10 bits wide, not 16.**

Register `0x06` carries only `data[9:8]` in bits `[1:0]`. Confirmed empirically: across the entire
upload, every value written to `0x06` lies in `{0,8,9,a,b,c,d,e,f}` — the data field never exceeds
`0x3`. The stock image is **6000 words at 0x0000–0x176f**, contiguous, every word ≤ `0x3ff`.

Surrounding sequence: `0xFD0C <- 3` (reset), `<- 1` (enable), `0xFD06 <- 8` (IMEM write enable),
upload, `0xFD06 <- 0`, `0xFD0C <- 8` (run), then `0xF004` (a *direct* JSS CSR, not an SBus
transaction) Reset=0/Enable=1. Liveness is interrupt 2 → 1; interrupt 4 is a CRC self-check.

**Verified:** reconstructing the image from EOS's own trace yields a file **byte-identical** to the
SDK blob (md5 `0ba4fbcc057d052801c484288849d1d8`). That validates both the decode and our loader.

## 3. Image characteristics

| property | value |
|---|---|
| size | 6000 words × 10 bits |
| distinct word values | 461 of 1024 |
| entropy | 7.10 bits/word |
| most common | `0x3c7` (5.6%), `0x044` (5.5%), `0x000` (5.2%), `0x347` (4.5%) |

**Not fixed-width-multi-word.** Per-position entropy at strides 1–5 is flat (6.7–7.1), so
instructions are not simply k×10-bit groups.

**Evidence for variable length.** The image opens with an obvious unrolled loop:

```
0000: 146 000 004 000 044 047 000 347
0008: 037 036 362 036 001 009 005 04e
0010: 000 3fa 362 036 002 009 003 04f
0018: 362 036 003 009 003 04e 362 036
0020: 004 009 003 04e 362 036 005 009
```

`362 036 <n> 009 …` repeats with `n` incrementing 001,002,003,004,005 — an immediate operand in the
instruction stream. Successor-entropy confirms mixed behaviour: `0x343` is almost always followed by
`0x005` (H=1.08, a fixed idiom), `0x3c0`→`0x001` (H=1.95), while `0x347` has H=6.29 (operand
follows).

**Open:** opcode field boundaries, register file, the interrupt-handler dispatch table, and which
routine performs RX adaptation.

## 4. Practical note — RE is not on the critical path

Two separate goals, very different costs:

- **Make copper links work** — does *not* need this RE. The firmware can simply be loaded, exactly
  like the FM6000 microcode, on the established "bring-your-own from a licensed EOS" model. **And
  it turns out not to help anyway** — see below.
- **Replace SPICO so the platform is fully distributable** — needs full ISA recovery plus writing a
  SerDes control program. Large project, and the last blob after the microcode generator.

**Tested and refuted (2026-08-06):** loading SPICO cold does *not* fix Et2. The load succeeds —
alive check passes, CRC self-check OK, SPICO running — and Et2 stays at `PORT_STATUS=0x815`,
`pcsRx=0`. So SPICO is not what Et2 is missing, and this further strengthens the case that SPICO is
not required at all on this platform.

## 5. Next steps

1. Recover the ISA: identify the opcode split, then hand-disassemble the entry sequence and the
   interrupt dispatch (interrupts 2 and 4 have known semantics — alive and CRC — which gives two
   anchors into the code).
2. Look for a jump/dispatch table near the image start; `0x146` at address 0 is a plausible entry
   vector.
3. Only then consider writing our own SerDes control program.

Priority is low relative to the microcode generator: SPICO is not required for the working datapath,
and it does not fix copper.

## 2026-08-15: ⚠ "SPICO IS REQUIRED for 10GBASE-CR" does not survive the base-rate measurement

`fm6000-fullseq.sh` carries this at the top of the file, and checklist C1/C2 rest on it:

```
*** SPICO IS REQUIRED for 10GBASE-CR (DAC/copper). ***
An earlier bisect concluded it was unnecessary -- that was WRONG, because it
only ever checked Et1 (10GBASE-SR fibre). With the firmware stripped:
  Et1 (SR)  links fine   -> PORT_STATUS=0x8c0, pcsRx=1
  Et2 (CR)  does NOT     -> PORT_STATUS=0x815, pcsRx=0
With fwd4.txt unmodified, BOTH link at 0x8c0/pcsRx=1.
```

**The last line is measurably false.** Ten controlled boots, `fwd4.txt` unmodified in the relevant
sense — and it demonstrably contains the firmware, 30,479 SBus writes to the SPICO broadcast device
`0xfd` of 62,482 total — gave **Et2 up on about half of them**, sitting at exactly `0x0815` on the
rest. See `PORT3-BRINGUP.md` for the run.

So the inference collapses:

- "SPICO stripped → Et2 reads `0x815`" is **one boot per condition** on a port that reads `0x815`
  roughly 44% of the time *with* the firmware present.
- That observation is therefore consistent with an unlucky boot and establishes nothing.
- The earlier bisect that concluded SPICO was unnecessary was overturned on this evidence. **The
  overturning is what is now in doubt** — not necessarily the original conclusion.

⚠ **This is not a claim that SPICO is unnecessary for copper.** It is a claim that the evidence for
"required" does not survive contact with the base rate. Checklist C1 already half-noticed this
("Et2 is intermittent with *and* without SPICO") but read the intermittency as a property of copper
rather than of the boot.

### The experiment that settles it, and it is cheap

The general case is hopeless — separating two rates near 50% needs ~31 boots per arm. **But this
hypothesis predicts an extreme, not a shift**: if SPICO is genuinely required for CR, Et2 must come
up **never** without it. Testing a predicted zero is far cheaper than testing a difference:

```
P(0 up in k boots | the measured ~56% rate) = 0.44^k
  k=3  -> 0.085      not enough
  k=5  -> 0.016      significant
  k=7  -> 0.003      comfortable
```

**Five boots with the SPICO writes stripped, Et2 sampled for three minutes each.** One single
success refutes "required" outright. Five failures support it at p≈0.016. That is about an hour on
`tools/et2-baserate.sh` with a stripped replay, against the 6.5 hours per arm a general comparison
would need.

⚠ Run it against the **same** replay with only the `0xfd` writes removed — not against an older
stripped image — or the arm changes for other reasons too.

### Why this matters to scope

C2 is *"if copper needs it — our own equaliser loop over SBus. Large, unscoped."* If SPICO turns out
not to be required for CR, **C2 disappears** and "zero proprietary files" stops being a fibre-only
claim. That is the single largest piece of remaining work riding on a conclusion drawn from one boot.

### ⚠ Correction to this test's own framing, written before the result

Above I wrote that 0 of 5 would "support 'required' at p≈0.016". **That is sloppy and I am fixing it
before the answer arrives rather than after.**

The p-value tests the null hypothesis *"stripping changes nothing"* — i.e. the measured ~50% rate.
Rejecting that null establishes only that stripping **reduces** Et2's up-rate. It says nothing about
whether the reduced rate is zero, and "required" means zero. The alternatives are not just
{0%, 50%}:

| true no-SPICO rate | P(0 up in 5) |
|---|---:|
| 0% — genuinely required | 1.00 |
| 10% | 0.59 |
| 20% | 0.33 |
| 33% | 0.13 |
| 50% — unchanged | 0.03 |

**A true 20% rate produces 0-of-5 a third of the time.** So this test cannot separate "required"
from "reduced but possible", and was never going to.

**What each outcome licenses:**

- **Any Et2 success** → decisive. "Required" is refuted, C2 disappears. Falsification needs one
  observation and does not care about the base rate at all.
- **0 of 5** → *"stripping SPICO significantly reduces Et2's up-rate (p ≈ 0.03)"*. Enough to keep C2
  on the list as real work. **Not** enough to write "SPICO IS REQUIRED" back into
  `fm6000-fullseq.sh` as settled — that overclaim is what started this thread, and restating it with
  a p-value attached would be worse than the original, not better.

Separating "required" from "reduced" would need ~15 boots at a true zero to reach p<0.001. The
cheaper route is not more boots at all: it is catching *why* a good boot differs from a bad one
inside one boot, which is what the Et2 sampling now added to `fm6000-fullseq.sh` STEP7 is for.

**The general rule this hands the project: falsification costs one boot here, confirmation costs a
day. Design experiments so a single observation can kill them.**

## RESULT: 0 of 5 boots with SPICO stripped — and the statistic I pre-registered was wrong

**2026-08-15.** Five boots, IMEM stripped, arm verified `cac05757…` on every one, zero tainted,
zero failed, `Et1 = 0x00000cc0` throughout (fibre links fine without the firmware, as the original
bisect said).

```
with SPICO      5 up / 10 boots
without SPICO   0 up /  5 boots
```

### ⛔ The correction

I pre-registered "0 of 5 supports the hypothesis at p ≈ 0.03". **That statistic is wrong.** It
treats the 50% baseline as a *known* rate; it is not, it was estimated from 10 boots with a 24-76%
interval. Comparing two samples needs Fisher's exact test:

```
binomial against an assumed 50%       p = 0.031
Fisher exact, one-sided (correct)     p = 0.084     <- NOT significant
```

So 0-of-5 against a 50% arm is **suggestive and in the predicted direction, but does not reach
significance.** Had I not recomputed it, this file would now claim a significant result on the
strength of the wrong test — which is precisely the failure mode the rest of this document exists
to correct, arriving one layer up: not a wrong measurement this time, a wrong analysis of a good one.

### Extending the arm

Fisher, if the no-SPICO arm stays at zero:

| no-SPICO boots | p |
|---:|---:|
| 5 | 0.084 |
| 6 | 0.058 |
| 7 | **0.041** |
| 8 | 0.029 |
| 10 | 0.016 |

Two more boots buy significance for ~25 minutes, against a checklist item (C2) scoped as "large,
unscoped", so they were run.

### ⚠ The ceiling, which more boots do not raise

Even at 0-of-10, this design shows SPICO **significantly reduces** Et2's up-rate. It cannot show
"required", because required means a true rate of zero and a null result cannot distinguish zero
from small: a true 10% rate yields 0-of-7 about half the time.

**So `fm6000-fullseq.sh`'s "*** SPICO IS REQUIRED for 10GBASE-CR ***" should not be restored as
written, whatever this arm reaches.** The defensible replacement is:

> With the 30,002 IMEM transactions stripped, Et2 did not link on N consecutive boots, against an
> up-rate of 5/10 with the firmware present (Fisher p = …). Fibre (Et1) is unaffected. Whether the
> copper rate is zero or merely low is not established.

C2 therefore stays on the checklist as real work, on evidence, rather than on a single boot.

### FINAL: 0 of 7, Fisher p = 0.041

```
with SPICO      5 up / 10 boots
without SPICO   0 up /  7 boots      one replay md5 throughout, 0 tainted, 0 failed
                                     Et1 = 0x00000cc0 on all 17 boots

Fisher exact, one-sided:  p = 0.0407
```

**Stripping the 30,002 IMEM transactions significantly reduces Et2's up-rate.** That is the claim,
and it is now on evidence rather than on one boot.

⚠ **It is still not "required", and 0-of-7 is not close to showing that:**

| true no-SPICO rate | P(0 in 7) |
|---:|---:|
| 0% — required | 1.00 |
| 5% | 0.70 |
| 10% | 0.48 |
| 20% | 0.21 |

A true 10% rate produces this exact result about half the time. Distinguishing "zero" from "low"
is not reachable by adding boots at any affordable count — it needs a mechanism, not a tally.

### What to write in `fm6000-fullseq.sh`

Replace the "*** SPICO IS REQUIRED for 10GBASE-CR ***" block with:

> SPICO matters for 10GBASE-CR, measured: with the 30,002 IMEM transactions stripped Et2 did not
> link on 7 consecutive boots, against an up-rate of 5/10 with the firmware present (Fisher exact,
> one-sided, p = 0.041). Fibre is unaffected — Et1 came up on all 17. Whether the copper rate is
> zero or merely low is NOT established, so do not read this as "impossible without SPICO".
>
> ⚠ Et2's link is intermittent at ~50% even with the firmware. Any future claim here needs n boots
> per arm and a reported count; a single boot measures nothing. See docs/PORT3-BRINGUP.md.

### Status of C1 / C2

- **C1 "decide copper"** — decided, with a caveat. SPICO measurably matters for CR. "Zero
  proprietary files" remains honest only for a fibre-only build.
- **C2 "our own equaliser loop over SBus"** — stays on the checklist as real work. It is no longer
  resting on a single boot, which is the change; the scope is unaltered.

## Can the SPICO firmware be reverse engineered? (2026-08-21)

Asked directly while inventorying what is left to remove from the install. Analysed rather than
guessed.

### What it is, established

- **12,000 bytes = 6,000 words, every one 10 bits wide** (max `0x3ff`, one word per 16-bit slot,
  461 distinct values, byte entropy 4.70 — structured, not compressed or encrypted).
- Byte-identical to `fm6000_spico_code`, a 12,000-byte OBJECT at `0x57e500` in
  `libFocalpointSDK.so`, with `fm6000_spico_code_size` beside it.
- It is a **program for a microcontroller**, not a configuration table. That is a different
  category from everything else we have authored.

### Why full reverse engineering does not pay

**The ISA is not recoverable from what we have.** The SDK exports 11 SPICO symbols —
`fm6000LoadSpicoCode`, `fm6000InterruptSpico(V2)`, `fm6000SetSpicoState`, the eye-score
accessors — and **none is a disassembler, opcode table, or version string**. The SDK uploads the
image and thereafter communicates only through the interrupt interface. It does not know the
instruction set either; that belongs to the SerDes IP vendor.

Deriving a 10-bit ISA from 6,000 instructions with no reference is theoretically possible and
practically enormous — and the prize is the ability to rewrite **DSP control firmware**: DFE tap
adaptation, CDR, eye monitoring, running against analog blocks whose control registers we have
only partially mapped. That is a different discipline from recovering register tables, and it
would not make the switch better understood in any way that matters.

### What actually matters about it

- **It is only required for 10GBASE-CR (copper/DAC).** Measured: stripped, Et1 (10GBASE-SR
  fibre) linked on all 17 boots; Et2 (CR) linked 0 of 7 against 5 of 10 with it (Fisher
  p = 0.041). So the blob is removable **today** at the cost of copper support.
- **It is 12 KB** — the smallest artefact in the install by three orders of magnitude, against
  `fwd-executed.txt` at 1.67 MB.
- It is what runs **DFE adaptation**, which the port-3 work depends on
  (`docs/PORT3-BRINGUP.md`).

### Recommendation

**Keep it, and stop counting it as a blob to remove.** The standing rule is authorability
([[edgenos-understanding-over-relocation]]): author what can be understood, and say plainly why
the rest stays. Firmware for a third-party microcontroller core is in the same category as
JSS/SBUS board-measured lane tuning — not authorable, and honestly labelled rather than
pretended away.

If a copper-free build is ever wanted, `tools/strip-spico.py` already produces one and the
consequence is documented.
