# Bringing up front-panel port 3 on EdgeNOS

> ## ⚠ SUPERSEDED IN PART — read `ROUTED-PORT-ANATOMY.md` first
>
> **2026-08-12.** The egress hunt recorded below chased the wrong mechanism from end to end.
> Configuring `Ethernet3` as a **routed** port on EOS and diffing the live chip against its
> access-port state shows the whole difference is **ten words**, none of them in `GLORT_CAM`,
> `GLORT_RAM`, `L2F`, `SAF_MATRIX`, `LBS_CAM` or `L3AR` — every structure this document programs.
> Those regions diff to **exactly zero**.
>
> The real recipe is `MAPPER_SRC_PORT_TABLE[41]`, `MAPPER_VID1_TABLE/VID2_TABLE`,
> `L2L_EVID1_TABLE`, `MOD_L2_VLAN1_TX_TAGGED`, `NEXTHOP_TABLE` and the FFU BST. Also: port 3's
> logical id is **`0x03f0`**, allocated in configuration order, not the `0x03ed` guessed here.
>
> The link bring-up in this document (§ *The port*, the lane-1 EPL configuration) is correct and
> still current. The forwarding-path work is not.

## 2026-08-12: why lane 1 does not link, and where the gap actually is

Under `edgenos-special.swi`, `PORT_STATUS(EPL14, lane1)` sits at `0x15` while lane 0 (Et1) reaches
`0x8c0`/`0xcc0`. Diffing the whole EPL14 block (`0xe3800`, 256 words) between a live EOS with both
ports up and a live EdgeNOS localises it precisely.

**Three leads that looked good and were wrong**, recorded so nobody re-walks them:

- `AN_37_CFG` (offset `0x28`) is written for lane 0 and never for lane 1 — by our generators *and*
  by the replay. But it reads **zero on both lanes of a working EOS**, so it is not configuration
  that matters here.
- `SERDES_IP` (offset `0x41`) is likewise lane-0-only, and the live chip has `0x4b0` on both lanes.
  That looked like a missing write. **`IP` is "interrupt pending", not "IP config"** — `0x40` is
  `SERDES_IM` (mask) and `0x41` is `SERDES_IP` (pending), both written `0x3fff` as write-1-to-clear.
  The value is status.
- The **SFP laser**. `fm6000-fullseq.sh` STEP6 clears bit 6 of SCD `0x5010` for one port, and
  nothing enables port 3's. But every register in `0x5010`–`0x503c` already reads `0x180` with bit 6
  clear, so the lasers are on.

Equally, the differences at offsets `0x26`, `0x38`, `0x3e`, `0x3f`, `0x41`, `0x42` are all **status**
registers — `PCS_10GBASER_RX_STATUS`, `LANE_STATUS`, `SERDES_STATUS`, `SERDES_IP`, `LANE_DEBUG` —
which the replay writes for neither lane. They report a dead lane; they do not cause one.

**What is real: the replay captured lane 1 half-configured.** Its own final values are

| offset | lane 0 (11 writes) | lane 1 (**5** writes) | EOS live lane 1 |
|---|---|---|---|
| `0x39` `SERDES_RX_CFG` | `002a0281` | `00280280` | `002a0281` |
| `0x3a` `SERDES_TX_CFG` | `c0000581` | `80000080` | `c0001581` |
| `0x3b` | `00000c83` | `00000803` | `00000c83` |
| `0x3c` | `000001fe` | `000001ee` | `000001fe` |

EdgeNOS reproduces those byte for byte, so this is **not** a generator defect — the capture window
missed lane 1's SerDes bring-up. (Note EOS's lane 1 `0x3a` is `c0001581`, genuinely different from
lane 0's `c0000581`, so the values are per-lane and cannot be copied across.)

**But writing EOS's values does not bring the lane up.** All seven config-looking registers
(`0x04`, `0x10`, `0x21`, `0x39`–`0x3c`) were set to EOS's exact values on the live chip; lane 0 was
unaffected and lane 1 stayed at `PORT_STATUS=0x15`, `pcsRx=0`, `LANE_STATUS=0`.

⇒ **The gap is below the EPL register block, in the SerDes/SBus lane bring-up** — the `sbus=30752`
half of `fullseq`, a different address space (`SERDES_ETH_READ/WRITE`, SPICO). The EPL block is now
provably identical to a working one and the lane is still dark, which rules that layer out.

Also worth knowing: **nothing in the boot scripts mentions `0xe3880` at all.** `fm6000-fullseq.sh`'s
STEP7 is purely observational (sleep and report) and every address in it is lane 0's.

The cheapest route to a fix is a **fresh replay captured with Et3 configured and linked from the
start** — the capture would then contain lane 1's full SerDes sequence, SBus ops included, rather
than the 5 stray writes it has now.

### …and that capture has now been taken

`fmPlatformTraceRegOps` armed on the running EOS, then `interface Ethernet3 / shutdown / no
shutdown`, then disarmed. The recipe is the one `ET2-COPPER-LINK.md` proposed for Et2, and for a
*live* arm it is much simpler than the cold-boot watcher:

```sh
gdb -batch -p $(pgrep -x FocalPointV2) \
    -ex 'call (void)fmPlatformTraceRegOps(1)' -ex detach -ex quit
# ... toggle the port, wait for link ...
gdb -batch -p $(pgrep -x FocalPointV2) \
    -ex 'call (void)fmPlatformTraceRegOps(0)' -ex detach -ex quit
```

⚠ **Disarm afterwards.** The noise floor is ~9,000 lines/second of JSS/SBus polling reads; `/var/log`
is a 389 MB tmpfs and fills in about twenty minutes. Mark the log line count before and after and
`sed` out the window — a 160,518-line window held the whole bring-up.

The capture is clean: **79 writes to EPL14 lane 1 and zero to lane 0**.

**The answer: lane 1's SerDes is SBus device `0x4a`.**

| device | ops | what |
|---|---:|---|
| `0xfd` | 112 | SPICO firmware broadcast (reloaded during bring-up) |
| **`0x4a`** | **43** | **EPL14 lane 1 — port 3** |
| `0x49` | 4 | EPL14 lane 0, incidental |
| `0x45` | 4 | EPL16 lane 0 (Et2), incidental |

So the mapping is **+1 per lane**: EPL14 lane0 = `0x49`, lane1 = `0x4a`. The two devices that get a
full bring-up in the older cold-boot trace, `0x49` and `0x45`, are Et1 and Et2 — which is why no
existing trace contained a lane-1 sequence, and why this had to be captured.

The sequence itself mirrors lane 0 step for step, at lane-1 addresses, with values differing only
where they are genuinely per-lane:

```
lane 0:  0xe3837 <- 018c0002   0xe3839 <- 002a0280   0xe383a <- c0000580   0xe3840 <- 00003fdf
lane 1:  0xe38b7 <- 018c0002   0xe38b9 <- 002a0280   0xe38ba <- c0001580   0xe38c0 <- 00003fdf
                                                              ^^^^ bit 12
```

Raw trace: `notes/reference/scd-dumps/fm6000-et3-noshut-LIVE-trace.txt` (568 lines: all EPL14 lane-1
writes plus every SBus op in the window). It is EOS-derived and lives in the notes repo, never in
this tree.

**What this unblocks:** `fm6000_linkup.c` is a working lane-0 bring-up — 422 MMIO ops and 37 SBus
ops, every SBus op hardcoded to device `0x49` and every MMIO address to `0xe38xx` lane 0.
Parameterising it by lane (device `0x49 + lane`, MMIO offset `+ 0x80 * lane`) and replaying the
captured lane-1 values is now a well-defined job rather than an investigation.

### `fm6000_lanelink`: built, validated, and it does not link the lane

`fm6000_lanelink` implements exactly that generalisation — the 52 EPL-lane MMIO ops and 37 SBus ops
held lane-relative, with addresses, SBus device and TX equalisation all derived. Offline it is
exact: **port 1 reproduces `fm6000_linkup` 89/89 ops identically**, and port 3 produces lane-1
addresses, device `0x4a`, SPICO reg-0x03 payload `0x4a` and `SERDES_TX_CFG` `c0001580`/`c0001581`
— every one matching the Et3 capture and the live-chip measurement.

Two bugs surfaced only because the check was a *full-sequence* diff, not a spot check:

- `EPL_BASE` written `0x0E000` instead of `0xE0000` — every address wrong by 16×.
- **16 of the 89 SBus ops target `0xfd`, the SPICO broadcast, not the lane's SerDes.** Retargeting
  the broadcast *device* is wrong; what moves is its *payload*, because its reg-0x03 write carries
  the target device as data. Confirmed against the capture, which writes `dev=0xfd reg=0x03
  data=0x4a`. Spot checks passed while those 16 ops were silently corrupt.

**On hardware it runs cleanly and the lane stays dark.** All 89 ops execute, `PIN` stays `0x208`,
**Et1 is completely unaffected** (`0x8c0`/`0xcc0`, `pcsRx=1`) — and lane 1 remains `PORT_STATUS=0x15`,
`pcsRx=0`.

The SerDes is not the problem. A raw SBus read of core register `0x00` returns the same ID `0x5a`
from `0x49`, `0x4a` and `0x45`, so device `0x4a` is alive and reachable; registers `0x03`/`0x06`
differ between the three, i.e. they are simply in different states.

**What is actually wrong is the source sequence.** `fm6000_linkup`'s window is a *cold Et1* capture:
52 EPL writes, 21 SBus ops to the lane, 16 to SPICO. The live Et3 down→up capture is materially
bigger — **79 EPL writes, 43 SBus ops to `0x4a`, and 112 to `0xfd`** — so the live bring-up does
substantially more SPICO work than the cold window contains. Parameterising the wrong sequence
faithfully still gives the wrong sequence.

⇒ Next: build the lane bring-up from **the lane-1 capture itself**
(`fm6000-et3-noshut-LIVE-trace.txt`) rather than by transposing the lane-0 cold window. The capture
is already in hand and `fm6000_lanelink`'s structure — lane-relative ops, derived addresses, derived
equalisation, the two SBus device classes — carries over unchanged; only the op table is replaced.

### Half the link now works: our TX is good, our RX never locks

The op table was rebuilt from the Et3 capture, segmented to that lane by the SPICO broadcast's
reg-0x03 payload: **168 ops** (79 EPL, 43 SBus to `0x4a`, 46 SPICO-for-`0x4a`), excluding the 66
SPICO ops belonging to other ports in the same window. The generator reproduces the capture
168/168 identically.

Run on hardware it executes cleanly, leaves Et1 untouched, and afterwards **every configuration
register on lane 1 matches a working EOS lane 1 exactly**:

```
off  EdgeNOS    EOS-working
0x37 000c0002   000c0002     0x3a c0001581   c0001581
0x39 002a0281   002a0281     0x3b 00000c83   00000c83
0x3c 000001fe   000001fe
```

The two that still differ, `0x04` and `0x21`, are `LINK_IP` (interrupt pending) and
`MAC_LINK_COUNTER` — status, not configuration. Writing EOS's values into them changes nothing, as
expected.

**And yet:** `PORT_STATUS = 0x15`, `pcsRx = 0`, and SerDes core registers `0x22`/`0x26` read `0x00`
where the working lane reads `0x6c`/`0x4c`.

The decisive measurement is at the far end. The test host reports:

```
Speed: 10000Mb/s
Link detected: yes
```

**It locks onto our transmitter.** Our TX is fine, the fibre is fine, the optics are fine, and SCD
`0x5030` bit 0 (rxlos) is clear so light is arriving. What fails is only our receiver locking to it.

That is exactly the asymmetry recorded for Et2 in `ET2-COPPER-LINK.md` — "the far end reports Link
detected: yes while our pcsRx stays 0; our receiver never locks" — reached here from a completely
different direction, on a different port, with different media (SR, not CR). So it is not a
copper-specific problem.

### Why a live capture cannot fix this

A `shutdown`/`no shutdown` on a running EOS toggles a SerDes that is **already calibrated**. The RX
adaptation that a cold lane needs happened at EOS's boot and is therefore *absent from the live
capture by construction* — which is why replaying it faithfully still leaves the receiver unlocked.
It is the same shape of error as transposing the cold lane-0 window: the sequence is authentic, and
it is the wrong sequence.

⇒ **What is needed is a COLD-boot trace with Ethernet3 configured.** Two attempts with the
`fpcoldwatch.sh` harness both failed, and the reason is now measured rather than guessed.

### The cold capture cannot be armed in time by this method

Attempt 1 armed at **219 s** into boot. Attempt 2 added `set auto-solib-add off` to skip
shared-library symbol resolution and armed at **211 s**. Both produced the same useless signature:

```
distinct lane-1 addresses written: 33
   204 0x000e3890      200 0x000e3892      198 0x000e3893      198 0x000e3891
```

33 addresses hit ~200 times each is EOS's port agent **polling**, not a one-shot calibration. The
SBus histogram agrees — `0x49`/`0x4a`/`0x45` at 73/73/74 ops, symmetric, where a real cold bring-up
would put one device far ahead.

⚠ **"Contains `0x000e38xx` writes" is not a sufficient completeness test.** Attempt 1 passed it with
7,206 EPL writes and was worthless. The test that discriminates is **configuration vs polling**:
many distinct addresses written once or twice, versus few addresses written ~100×.

The timing makes the cause plain:

| | |
|---|---|
| FocalPointV2 process appears | **167.9 s** |
| gdb finishes attaching and arms | **211.4 s** |
| gdb attach cost | **~44 s** |

FocalPointV2 does not exist before ~168 s, so nothing can arm earlier than that; and the agent
completes the whole ASIC bring-up inside the 44 s gdb needs to attach and plant the breakpoint on
`fmPlatformWriteCSR`. Skipping solib symbols saved only ~8 s — the cost is elsewhere.

**So the breakpoint approach cannot win this race.** Arming has to happen *without* attaching a
debugger after the fact — options, none yet tried:

- an EOS knob that starts FocalPointV2 with reg-op tracing already enabled (the function exists;
  something must set it);
- `LD_PRELOAD` a constructor into the agent that calls `fmPlatformTraceRegOps(1)` before `main`;
- start the agent under gdb rather than attaching to it, so the breakpoint exists before it runs.

Until one of those works, `fwd4.txt` remains the only cold artifact, and it captured Et3 as an
access port with lane 1 half-configured.

### The SerDes core: the boot leaves it asleep, and waking it is still not enough

`fm6000_sbusdump` (new, read-only) reads the SerDes core over the SBus — the layer the EPL block
says nothing about. Comparing **the same device `0x4a`, working-on-EOS versus dead-on-EdgeNOS**
rather than lane against lane:

**Register `0x00` — the device ID — reads `0x5a` on EOS and on both lane 0s, but `0x00000000` on
EdgeNOS's lane 1.** The boot never brings that SerDes core out of reset. Nothing above it could
have worked.

`fm6000_lanelink` **does** wake it — ID goes `0x00` → `0x5a` — which is the first demonstration that
the tool does real work. The link still does not come up.

⚠ **A control group is essential here and it is easy to skip.** Lane 0 works under *both* operating
systems, yet **29 of its 256 core registers differ** between them: the core is full of adaptation
state and counters that drift. A raw EOS-vs-EdgeNOS diff of lane 1 therefore shows 23–35
differences that mean nothing. Only registers that differ on lane 1 **and not** on lane 0 are
candidates.

With the core awake that filter leaves exactly **two**, `0x0b` and `0x0c` — and both are volatile.
Re-reading them before writing showed them already holding EOS's values, and writing `0x00` to
`0x0b` read back `0x20`. They fluctuate between reads; they are status, not configuration.

⇒ **With the core awake, every stable SerDes core register on lane 1 already matches a working
lane, and the receiver still will not lock.** Combined with the earlier result that every EPL
configuration register matches too, the conclusion is that **nothing is missing as a register
*value*.** What is missing is a *procedure* — the SPICO's per-lane RX adaptation, which is an action
the cold bring-up performs, not a state it leaves behind. That cannot be recovered by diffing
register files, only by capturing or reimplementing the routine.

So the cold-trace problem is not a detour around the SerDes question; it **is** the SerDes question.

### The missing procedure has a name: DFE tuning

`notes/reference/fm6000-sdk/AltaLib.py` (SDK reference, read not copied) makes the missing step
explicit. `AltaSerdes` exposes receiver adaptation as a first-class operation with **readable
state**:

```python
_dfeModes  = {'continuous', 'kr', 'one shot', 'static'}
dfeState   -> fm6000CheckSerDesDfeTuningState(sw, serdes, FM6000_SERDES_TYPE_ETH)
              returns (coarse, fine) in {not started, in progress, complete, error}
dfeValue   -> fm6000GetSerDesDfeStatus / fm6000SetSerDesDfeParams
```

**DFE — the decision-feedback equaliser — is the receiver adaptation.** That is exactly the thing
that is an *action* rather than a register value, which is why every register on lane 1 can match a
working lane while the receiver still refuses to lock. On our lane DFE tuning has almost certainly
never started; on EOS's it is complete.

It also confirms the SerDes numbering independently. SerDes numbers run 0–95 and `fm6000_dump.c`
calls SBus `0x49` "serdes-68", so **SBus address = SerDes number + 5**:

| port | EPL/lane | SerDes | SBus |
|---|---|---:|---|
| Et1 | 14/0 | 68 | `0x49` |
| **Et3** | **14/1** | **69** | **`0x4a`** |
| Et2 | 16/0 | 64 | `0x45` |

Every value measured on hardware fits, and it generalises to all 52 ports.

### The experiment this points to

**Do not chase the cold-boot trace.** DFE tuning can be triggered on demand — `dfeMode = 'one shot'`
is an SDK setter and EOS ships the library implementing it. So:

1. On EOS, arm `fmPlatformTraceRegOps` — the **live** arm works fine; only the cold arm loses the race.
2. Trigger a one-shot DFE tune on serdes 69 through the SDK.
3. Disarm and extract.

That isolates **precisely the missing procedure** — the SPICO command sequence for RX adaptation —
with no boot-timing race at all, using the same targeted-capture method that found the lane-1 SerDes
address in the first place. Replaying that sequence under EdgeNOS is then the test.

Worth a pre-check on the same visit: read `dfeState` for serdes 68 and 69 on EOS. Both should read
*complete*. If lane 1 reads anything else while linked, the model above is wrong, and that is much
better known before building on it.

### …and the experiment worked: the DFE procedure is captured

**A foreign process cannot drive the SDK.** EOS ships the bindings — `import Tac` first, then
`FmApi`/`DosFmApi` load fine — but calling `fm6000CheckSerDesDfeTuningState` from a standalone
Python process segfaults inside `fm6000ReadSBus`: a fresh process has its own *uninitialised* SDK
with no switch opened. (Contained; both ports stayed up.)

**gdb inside the live agent can.** The same mechanism that arms the tracer also calls SDK functions
against the agent's initialised state:

```sh
gdb -batch -p $(pgrep -x FocalPointV2) -ex 'set auto-solib-add off'     -ex 'set $c=(int*)malloc(4)' -ex 'set $f=(int*)malloc(4)'     -ex 'call (int)fm6000CheckSerDesDfeTuningState(0,69,0,$c,$f)' ...
```

Pre-check result — **serdes 68 and 69 both read `coarse=2 fine=1`** on working EOS. The model
survives: both lanes tuned.

The SDK exposes 41 DFE symbols, found with `grep -ao` on `libFocalpointSDK.so` (EOS has no
`strings` or `nm`). Among them the trigger: **`fm6000StartSerDesDfeTuning`**, plus
`fm6000RestartSerDesDfeFineTuning` and `fm6000TriggerDfeTuningRecovery`.

So: arm the trace, `call (int)fm6000StartSerDesDfeTuning(0,69,0)`, disarm. **Et3 stayed up
throughout**, and the captured window is tightly targeted:

| | |
|---|---:|
| EPL lane-1 writes | 29 |
| SBus ops to **`0x4a`** | **35** |
| SPICO broadcast (`0xfd`) | 80 |
| other lanes (`0x49`/`0x45`) | 3 each — incidental |

That is the RX adaptation procedure on its own, with **no boot-timing race and no cold reboot** —
392 lines, in `notes/reference/scd-dumps/fm6000-et3-dfetune-LIVE-trace.txt`.

**Why this succeeds where two cold captures failed:** the thing we needed was never boot-specific.
It is an SDK operation with an entry point, and the entry point can be called on demand. Two boots
were spent racing gdb's attach against the ASIC init to catch a procedure that could simply be
*asked for*.

Remaining: segment it to serdes 69 (SPICO reg-0x03 payload rule, as for the link sequence), fold it
into `fm6000_lanelink` after the link ops, and test under EdgeNOS.

### Segmented and folded in — untested on hardware

**2026-08-13.** Done for the first two; the third needs a lab visit.

The segmentation was written as a script rather than done by hand, and **validated against a known
answer before being trusted**: run over `fm6000-et3-noshut-LIVE-trace.txt` it reproduces the 168-op
`SEQ[]` already shipping in `fm6000_lanelink.c` **byte-for-byte**. Only then was it pointed at the
DFE capture. Its rules are the ones the file already documents — an op belongs to the lane if it is
an SBus op to the lane's own SerDes, a SPICO op whose most recent `dev=0xfd reg=0x03` write named
that device, or an MMIO write inside `base .. base+0x7f`.

| | trace | kept |
|---|---:|---:|
| EPL lane MMIO | 29 | 29 |
| SBus to `0x4a` | 35 | 35 |
| SPICO (`0xfd`) | 80 | 30 |
| other lanes (`0x45`/`0x49`) | 52 | 0 |

**96 ops segmented, 94 kept.** The capture was disarmed mid-block: its last two ops open a SPICO
interrupt with reg `0x01`/`0x02` and the window ends before the reg-`0x03` write that would name its
target. Replaying a half-formed interrupt is not something to do to a live SerDes, so the table ends
on the last complete block.

`DFE[]` runs after `SEQ[]`, and only if `SEQ[]` completed — tuning a lane whose bring-up aborted
would adapt to a link that is not there. `-l` restores the old link-only behaviour. The op-execution
loop is now shared by both tables, so the device retargeting is applied identically to each;
verified by dry-running port 2 (EPL16 lane 0), where **no `0x4a` survives anywhere in either
table** and all ten SPICO reg-0x03 payloads carry `0x45`.

### ⛔ Tested on hardware — the lane stays dark. DFE is not the missing procedure.

**2026-08-13, alpha9 (`d4d5555`), cold boot, FULLSEQ complete, Et1 forwarding.**

| | port 3 lane (`0xe3880`) | Et1 lane (`0xe3800`) |
|---|---|---|
| control, before | `PORT_STATUS=0x15`, `+0x04=0x1002`, `pcsRx=0` | `0xcc0`, `0x9b87`, `pcsRx=1` |
| after 262 ops | `PORT_STATUS=0x15`, `+0x04=0x0000`, `pcsRx=0` | unchanged, still forwarding |

All 262 ops completed — no SBus failure, no off-bus, `262 ops done`. The sequence runs correctly and
**does not link the lane**. `+0x04` moved `0x1002`→`0`, consistent with a W1C status register rather
than with progress.

**The external fact that interprets this:** the test host on the other end of port 3 reports
`Link detected: yes, Speed: 10000Mb/s`. **Our TX reaches it; our RX never locks.** That is the same
split recorded earlier in this document, unchanged by DFE.

**Why this should have been predicted.** The capture was taken by calling
`fm6000StartSerDesDfeTuning` on a port that was already **up** — DFE *adapts an existing signal*.
It cannot originate lock. Folding it in after the link ops was, at best, necessary-but-not-
sufficient; the gap is whatever brings RX/CDR to lock in the first place, and DFE runs after that.
The "missing procedure is DFE tuning" hypothesis is refuted in this form.

⚠ SerDes regs `0x20`–`0x27` (the block the DFE trace polls) differ between the two lanes —
e.g. `0x21` = `0x0e` on Et1 vs `0x4c` on port 3, `0x25` = `0x0e` vs `0xcc`. Et1's `0x20`–`0x23`
mirror its `0x24`–`0x27`; port 3's do not. **No before/after was captured on port 3**, so this is
not attributable to the DFE run — it may equally be the dark lane's resting state. Read the
register header before drawing anything from it.

⚠ Two things this does **not** do, both worth knowing before the hardware test:

- **It issues the procedure; it does not wait for it.** The polls are replayed as fixed reads. The
  SPICO firmware performs the adaptation, and the real caller
  (`fm6000CheckSerDesDfeTuningState`) loops until `coarse`/`fine` report complete. If the lane
  needs more iterations than the captured run took, a verbatim replay walks away early.
- **It is inert without SPICO firmware.** DFE tuning *runs on* the SPICO, so on the fibre-only
  build that strips the 30,002 IMEM transactions (C1) there is nothing to execute it. The replay
  currently on flash retains SPICO, so this is a constraint on the C1 decision, not on the test.

---

**2026-08-11.** Front-panel port 3 is now up under EdgeNOS. This is the second connected port, and
it is what unblocks the transit traffic that A4 (MOD command split) and B1 (FFU ByteMux) need —
neither can be settled with CPU-terminated traffic alone.

## The port

From `asic/fm6000/fm6000_serdes_ports.h`:

```
{ 1, 40, 14, 0, ... }   front-panel 1 = alta 40, EPL14 lane 0   (Et1)
{ 3, 41, 14, 1, ... }   front-panel 3 = alta 41, EPL14 lane 1
```

Port 3 is **the same EPL as Et1, one lane over**, and both share `rxpol=0, txpol=1`. EPL14 is
already up and clocked for Et1, which is why this is lane-level work and not a full EPL bring-up.

## The recipe

Not derived from first principles — **read off a working EOS boot**, which has the port up as
`Et3 connected 10GBASE-SR` because the test host is plugged in. See `EOS-VS-EDGENOS-DIFF.md` for
how that reference was captured.

```sh
# 1. enable EPL port 1 and select its PCS  (EPL14 shared config)
fm6000reg 0000:02:00.0 0xe3b01 0x7e1d7899     # EPL_CFG_A: bit 20 = Active_1
fm6000reg 0000:02:00.0 0xe3b02 0x00090033     # EPL_CFG_B: bits 4-7 = Port1PcsSel = 3

# 2. apply EOS's lane-1 block, 0xe3880-0xe38ff (30 non-zero words)
fmload lane1.txt
```

`EPL_CFG_A` bit 20 and `EPL_CFG_B` bits 4–7 were identified from the header
(`FM6000_EPL_CFG_A_b_Active_1`, `FM6000_EPL_CFG_B_l_Port1PcsSel`) — the two bits are the *only*
genuine configuration differences between EOS and EdgeNOS in the whole EPL region.

⚠ Step 1 alone is **not** sufficient: with only `Active_1`/`Port1PcsSel` set the lane stayed at
`0x00000015`. The lane-1 block carries the rest.

## Result

```
PORT_STATUS(EPL14, lane1) = 0xe3880
   before: 0x00000015   LinkFaultDebounced + LinkFaultMac + LinkFaultRx, RxLinkUp clear
   after:  0x000008c0   RxLinkUp, HeartbeatOk, SerXmit      <- stable over 30s
```

Et1 is unaffected throughout (`lane0` alternating `0x8c0`/`0xec0`, which is the Receiving bit
toggling on live traffic), `routes=34`, `PIN_STRAP=0x208`.

## Ingress is proven working

Controlled measurement — the test host given `10.99.99.2/24` on eth1 and made to transmit, while
`PORT_STATUS(EPL14, lane1)` is sampled once a second:

```
host idle:          0x8c0 0x8c0 0x8c0 0x8c0 0x8c0 0x8c0
host transmitting:  0xcc0 0xcc0 0xcc0 0xcc0 0xcc0 0xcc0     <- bit 10, Receiving
```

Six consecutive samples each way. The ASIC receives the test host's frames on port 3. The RX half
of the path is done.

**Nothing egresses yet**: `tcpdump -i eth1` on the test host captured 0 packets over 12s, and its
`rx_packets` counter did not move. Port 3 has link and receives, but is in no forwarding domain.

## What is still needed for transit traffic

Link is up; forwarding through it is not configured.

- **GLORT.** `PARSER_INIT_FIELDS[41]` reads `0x00010103` — logical id `0x103`, default SGLORT
  `0x0001`. **EOS leaves it at the default too**, because Et3 is a switched VLAN-1 port rather than
  a routed one; `GLORT-MAPPING.md` records that only configured-up routed ports (Et1 `0x03ef`,
  Et2 `0x03ee`) get real GLORTs. So a unique SGLORT plus a `GLORT_CAM` entry is needed only if we
  want to *inject* on port 3 from the CPU.
- **For transit we may not need that at all.** Wire→wire forwarding does not involve the CPU: a
  frame ingressing port 3 and egressing Et1 exercises the full pipeline including MOD, with no
  portd on port 3.
- **The capture direction that matters** is `peer → Et1 → switch → port 3 → test host`, because
  the test host is ours and can run tcpdump. That is the egress observation A4 has been blocked on
  since the beginning — MOD's edits become directly visible.

## Note on signals

`routes=34` is the reliable health metric here, not ping. Ping collapses to 100% loss on this box
for unrelated reasons (checklist D5) — it did so during this very session while OSPF was converged
with 34 routes and the FIB programmed. Do not read a ping failure as a forwarding failure.


---

## 2026-08-11, pulling the egress thread: three findings

### 1. The DGLORT→DestMask path is not populated at all

`GLORT_RAM.DMaskBaseIdx` indexes `FM6000_MCAST_DEST_TABLE` (`0x240000`, 4096 entries × 4 words,
`DestMask[75:0]` — one bit per port, so bit 40 = Et1, bit 41 = port 3).

Dumped all 4096 entries on a live chip with Et1 forwarding: **every word is zero.** No entry
selects port 40, or any port. So the chain GLORT_CAM → GLORT_RAM.DMaskBaseIdx → MCAST_DEST_TABLE
is not how a CPU-injected frame reaches Et1 — it cannot be, because the table is empty.

Combined with the earlier result that no `GLORT_CAM` entry matches `0x03ef` at all, the conclusion
is firm: **the ISL/F64 DGLORT does not resolve through the GLORT/DMask machinery.** Whatever
carries a CPU-injected frame to its egress port is something else, and both of the obvious
candidates are now eliminated by measurement rather than argument.

### 2. portd instances share DMA rings — you cannot run two

Running a second portd for `et3` alongside the `et1` one broke the dataplane: `routes` fell
34 → 3 while the et3 instance consumed **1,707 frames**. Killing it restored `routes=35`
immediately.

`edgenos-up.sh` warns about this for a *second run of the script*; the same hazard applies to a
second portd on a **different port**, because the rings are shared and **not demultiplexed by
port**. The second instance simply steals frames the first one needed.

So port 3 cannot be given its own portd as things stand. Supporting two CPU-attached ports needs
portd extended to demultiplex one ring set across ports — presumably on the ISL tag's source
GLORT, which is exactly what `PARSER_INIT_FIELDS[port]` stamps on ingress.

### 3. ⚠ RETRACTED: port 3's ingress does NOT reach the CPU

I originally read the 1,707 frames the et3 portd consumed as proof that port 3's receive path
worked end to end. **That was wrong, and it followed from finding (2).** The DMA rings are shared
and *not demultiplexed by port*, so an et3 portd reads whatever is in the ring — which was Et1's
traffic. The count proved the rings were being drained, not where the frames came from.

Direct check with `fm6000_rxdump` and no portd running, while the test host transmitted: **every
punted frame carries tag word 1 = `0x03ef`**, Et1's SGLORT. Nothing from port 3 is punted at all.

What is established for port 3 is the MAC-level measurement only: `PORT_STATUS` bit 10 `Receiving`
tracks the test host's transmission exactly (`0x8c0` idle, `0xcc0` transmitting, six samples each
way). The MAC receives; the frames go no further, because port 41 is in no forwarding domain.

## The punt frame format, decoded

`fm6000_rxdump` with no portd competing shows the real layout — and it is simpler than portd
assumes:

```
33 33 00 00 00 05 | 80 a2 35 81 ca b4 | 07 01  03 ef  00 01  ff ff | 86 dd ...
DMAC (0..5)         SMAC (6..11)        F64 tag: 4 x 16-bit at 12    ethertype (20)
```

- **There is no receive prefix.** The frame starts at offset 0 every time. portd scans offsets
  0..39 for a plausible ethertype because the framing was never characterised; it always finds 0.
- The **F64 tag is inline at offset 12**, four 16-bit words, exactly where portd puts it on inject.
- **Tag word 1 is the GLORT: source on RX, destination on TX.** Every punted frame from Et1 carries
  `0x03ef`, which is precisely `PARSER_INIT_FIELDS[40] >> 16`. portd's TX tag is
  `{0x0100, 0x03ef, 0xff00, 0x0000}` — same slot, opposite direction.
- Word 0 varies with frame type (`0x0701` on IPv6/OSPF multicast, `0x0301` on MLD) — a flags or
  priority field, not yet decoded.
- Word 3 is `0xffff` on RX, `0x0000` on TX.

**This is the demux key multi-port portd needs**: tag word 1 identifies the ingress port, provided
each port has a distinct SGLORT.

## ⚠ PARSER_INIT_FIELDS cannot be written after boot

Giving port 41 a unique SGLORT is a single register write —
`PARSER_INIT_FIELDS[41] = (0x03ed << 16) | 0x103` at `0x1082a4` — and **it does not stick**. The
register still reads `0x00010103` immediately afterwards. `fm6000reg` writes work fine elsewhere
(EPL_CFG_A/CFG_B took in this same session), so this is the parser tables specifically.

That is phase 76/78's *writability is a boot state* again: the replay's `PARSER_INIT_FIELDS` writes
land during fullseq, when the memory subsystem is writable, and post-boot writes do not. So port
41's SGLORT has to be set **in the boot path** — in a generator or spliced into the replay — not
interactively. That is a small, well-defined change and it is the next concrete step.


---

## Two constraints that shape the fix

### `PARSER_INIT_FIELDS` cannot simply be moved into a generator — already tried, already backed out

The obvious fix for "post-boot writes don't stick" is to have our own code write
`PARSER_INIT_FIELDS` during boot, via the `gen_list` mechanism that already strips replay writes
in favour of a generator. **That was tried and reverted on measured evidence.** From
`fm6000_tbl3init.c`'s own header:

> `PARSER_INIT_FIELDS` 970 writes. Passed its first boot 7/8 rounds clean, then a 3-boot soak gave
> 2 clean and 1 at 90-100% loss. TBL3 alone soaks 3 of 3 clean, so this looks like a reliability
> regression rather than variance.
>
> 970 writes is not worth degrading a platform that already fails 1 boot in 6.

(`ESCHED_DRR_Q` was backed out the same way: OSPF up at 35 routes, RX alive, 100% ping loss on 8
of 8 rounds, bisected to that register alone.)

So "own the GLORT allocation" cannot be done by lifting this table wholesale. The options left are
a **single-value edit** to the operator-supplied replay (one register, not a table move), or a
targeted write **inside `fm6000-fullseq.sh`** immediately after STEP5, while the memory subsystem
is still writable. The second is ours and does not touch the replay; both need an SWI rebuild.

⚠ And note the SGLORT alone is **necessary but not sufficient**: port 41 is in no forwarding
domain, so its frames are not punted at all. A unique SGLORT makes port-3 frames *identifiable* in
the punt stream; it does not make them *appear* there.

### One DMA-ring consumer per boot — and that includes `fm6000_rxdump`

`edgenos-up.sh` warns that restarting portd wedges the rings. The same applies to **any** tool that
opens them. Running `fm6000_rxdump` to capture the punt format and then starting portd via
`edgenos-up.sh` produced exactly the documented symptom:

```
[up]  t=96s  kernel routes=2  et1 rx=0
```

RX silently zero, no adjacency, and it looks precisely like a dataplane defect. A reboot and a
single consumer restored `routes=35, et1 rx=33`.

So: **capture with `rxdump`, or run portd — not both in one boot.** Reboot between them.


---

## Port 41 now has a real SGLORT — done, and it changes nothing yet

### What the replay actually does

The replay **does** write `PARSER_INIT_FIELDS[41]`, nine times, ending at `0x00010103`. (An earlier
note here said it never did; that was a `tail`-truncated grep, the same mistake that hid port 0
from the `PARSER_INIT_STATE` scan. Both are corrected.)

Reading the two ports side by side shows exactly what EOS does and does not do:

```
port 40 (Et1)   0 -> 0x00010000 -> 0x00010001 -> 0x03ef0001 -> 0x03ef0101
port 41 (p3)    0 -> 0x00010000 -> 0x00010003 -> 0x00010103
                                                 ^ never gets an SGLORT
```

EOS assigns `0x03ef` to Et1 because it configured that port up, and leaves port 41 on the default
`0x0001` because it never did. Nothing is special about port 41 — it is simply unconfigured.

### The change

One line of the operator-supplied replay, at its final write for that register:

```
312646:  001082a4 00010103   ->   001082a4 03ed0103
```

Backed up as `/mnt/flash/fwd4-preport3.txt`. Verified across a reboot:

```
PARSER_INIT_FIELDS[41] = 0x03ed0103    (was 0x00010103)
PARSER_INIT_FIELDS[40] = 0x03ef0101    (Et1, untouched)
```

This is the surgical option from the previous section — a single value, not the wholesale table
move that `fm6000_tbl3init.c` records as a measured reliability regression. It also sidesteps the
post-boot writability wall, because the write happens inside the replay where the memory is
writable.

### And it produces no new behaviour, as expected

With port 3 enabled and the test host transmitting continuously:

```
lane1 PORT_STATUS = 0x00000cc0     Receiving — the MAC has the frames
et1 capture, 40 frames            0 with the host's MAC, 0 with its payload pattern
```

So the SGLORT does what it says — it stamps an identity on frames ingressing port 41 — and that is
all it does. **Punting is a separate decision**, made by the forwarding tables, and port 41 is in
no VLAN or forwarding domain. This was stated in advance rather than discovered afterwards, and
the measurement confirms it.

What the change buys is that *when* port-3 frames do start reaching the CPU, they will be
identifiable by tag word 1 = `0x03ed`, which is the demux key multi-port portd needs. It is a
prerequisite that is now satisfied and verified, not a fix.

### Next

Port 41's VLAN/forwarding-domain membership. That is where the frames are being discarded, and it
is the last thing between here and transit traffic.


---

## The destination-resolution chain, found — and it is not MCAST_DEST

`fm6000_l2_probe` / `fm6000_l2.c` in this tree already implement the path, and reading them
answers the question the last several sections circled:

```
DGLORT --> GLORT_CAM[cam_idx] --> GLORT_RAM[cam_idx].DMaskBaseIdx = gid --> L2F_TABLE_256[gid]
                                                                            = 76-bit port bitmask
```

**The destination mask lives in `L2F_TABLE_256` (`0x1a0000 + 4*gid`), not `MCAST_DEST_TABLE`.**
That is why dumping all 4096 `MCAST_DEST_TABLE` entries found them uniformly zero on a chip that
forwards perfectly — it is simply not the table in use.

### Programmed for port 41 and verified

`fm6000_l2_probe 41 8 16` points the special-delivery GLORT at port 41 instead of the CPU:

```
GLORT_CAM[8]  = 0xff000000     matches exactly 0xff00
GLORT_RAM[8]  = 0x00000040     DMaskBaseIdx = 0x40>>2 = 16
L2F[16]       = w0 0x00000000  w1 0x00000200   <- bit 9 of word 1 = port 41
```

All three verified by readback. Dataplane stayed healthy throughout (routes 34, both lanes up).

### And injecting still does not egress port 3

```
fm6000_txinline 20 0100 ff00 ff00 0000
   frame[0:24]: 80 a2 35 81 ca b4 | 44 4c a8 31 5d ab | 01 00 ff 00 ff 00 00 00 | 08 00
   tag@12 = 0100 ff00 ff00 0000, queued=60 DONE=60 STATUS=0x00000012

lane1 PORT_STATUS = 0x000008c0     no Transmitting bit
test host tcpdump  = nothing
```

Frames are queued and completed by the DMA engine, the GLORT resolves to a mask naming port 41,
and nothing reaches the wire. So the mask is necessary but something downstream of it still
discards the frame — egress-port enable, VLAN membership on the egress side, or the MOD/egress
stage refusing a port it has no configuration for.

That is the current edge. Worth noting the same tag with `03ef` egresses Et1 perfectly, so the
inject path itself is sound and the difference is entirely port 41's configuration.


---

## Looking at EOS for the egress config — what it gave us

EOS has `Et3 connected 10GBASE-SR`, so it is a valid reference for port 41's egress. Dumped
`L2F_TABLE_256` (`0x1a0000`, 64K words) under both.

### EOS's masks that include port 41

```
L2F_256[0,2] [1,2] [2,2]   ports {0, 41}              CPU + port 3
L2F_256[3,0] [3,1] [3,3]   ports {0, 3, 20, 40, 41}   CPU + every configured port
```

EdgeNOS had **none** of them. Its only entry with bit 41 set was `[3,2]`, listing 51 scattered
ports — SRAM noise presented as a forwarding decision.

### Exactly three words differ

```
0x1a0009  eos=0x00000200   L2F_256[0,2].w1   bit 41
0x1a0409  eos=0x00000200   L2F_256[1,2].w1   bit 41
0x1a0809  eos=0x00000200   L2F_256[2,2].w1   bit 41
```

### ★ Wide entries commit on the last word

Writing `0x1a0009` alone **did nothing** — readback unchanged, which looks exactly like the
post-boot writability wall and is not. Writing the *whole* entry (w0, w1, w2 in order) took
immediately:

```
before  0x1a0008: 0x00000001 0x00000000 0x00000000
after   0x1a0008: 0x00000001 0x00000200 0x00000000
```

**L2F_TABLE_256 is a 3-word-wide entry and commits on the last word; a single-word write is
discarded.** This is worth remembering generally: a table that appears unwritable may simply be
wide. `fm6000_l2.c` always writes full entries, which is why its writes have always worked.

### And a third memfill gap

The garbage in `[3,2]` is another short fill run — the same defect as MOD and MAPPER:

```
{0x1a0000, 3072, ...}   covers to 0x1a0bff
garbage begins at       0x1a0c08   (602 words, all zero on EOS)
```

Fixed to 4096. Three of these now, all from fill lengths that were reconstructed rather than read.

### Still no egress

With the masks matching EOS, port 3 link up and traffic flowing, the test host still receives
nothing and `lane1` shows no Transmitting bit. The masks are correct but **nothing we generate
resolves to them** — the remaining unknown is which lookup selects `L2F_256[0..2, 2]`, i.e. what
`GLORT_RAM[i].DMaskBaseIdx` values point at those gids and which DGLORT reaches them.

That is now a small, well-posed question against a known-good reference, which is a much better
place than where this started.


---

## Frame type implemented and live — still no egress

The parser now carries the special-delivery rule, verified on the running chip:

```
LIVE: slice 3 entry 2   hw0=0x8000/care=0x8000   SetFlags[0,3]
EOS:  slice 3 entry 27  hw0=0x8000/care=0x8000   SetFlags[0,3]
one entry sets ACTION_FLAGS bit 0, same count as EOS
```

With every element below now verified against EOS on the same boot:

| element | state |
|---|---|
| port 3 link | up, `lane1 = 0x8c0` |
| port 41 SGLORT | `0x03ed` in `PARSER_INIT_FIELDS[41]` |
| `GLORT_CAM[8]` | matches `0xff00` |
| `GLORT_RAM[8]` | `DMaskBaseIdx = 16` |
| `L2F[16]` | `w1 = 0x200` = port 41 |
| parser SPECIAL rule | live, matching EOS's encoding |
| inject | `tag@12 = 8100 ff00 ff00 0000` |

**Nothing egresses.** `lane1` never sets Transmitting, the test host's `rx_packets` does not move,
and a NORMAL/SPECIAL A/B gives identical results.

So the frame type was necessary to decode and implement — our parser genuinely could not express
it, and now can — but it is **not sufficient**, exactly as the SGLORT and the L2F mask were not.
Three prerequisites satisfied in a row without reaching the goal.

### What that pattern suggests

Each fix has been verified in isolation and none has changed the outcome, which points at
something structural rather than another missing table entry. The untested link in the chain is
**L2AR** — the block that makes the actual forwarding decision, and the one this project has never
decoded (checklist A2, blocked on `DMT_PROFILE` / CPU-code / mirror semantics). Every element we
have verified is *upstream* of it (parser, GLORT resolution, destination mask) or *downstream*
(MOD egress). L2AR sits in the middle and is the one thing we cannot currently inspect.

The honest read is that CPU-inject to an arbitrary port needs L2AR configuration we have not
decoded, and that A2 is therefore not merely the next item on the list but a prerequisite for
this one.


---

## Following the replay's own recipe for Et1

Rather than decode L2AR, ask what the replay already does to make Et1 forward, and copy it. Every
replay write whose value contains Et1's GLORT `0x03ef` — 30 in total:

| page | n | table | Et1's value |
|---|---:|---|---|
| `0x00d000` | 12 | `L2L_SWEEPER` | `0x03ef3333` |
| `0x108000` | 7 | `PARSER_INIT_FIELDS` | `0x03ef0101` |
| `0x160000` | 5 | `NEXTHOP_TABLE` | `0x03ef80a2` |
| `0x034000` | 2 | `L2L_IVID1_TABLE[0x3ef]` | `0x020003ef` |
| `0x032000` | 2 | `L2L_EVID1_TABLE[0x3ef]` | `0x020043ef` |
| `0x037000` | 1 | `L2L_IVID2_TABLE[0x3ef]` | `0x000003ef` |
| `0x036000` | 1 | `L2L_EVID2_TABLE[0x3ef]` | `0x000003ef` |

That is the complete list of tables the working port's GLORT appears in — a much better target than
"decode L2AR", and it came straight out of the replay.

### The VID tables were genuinely missing

```
EVID1[0x3ef] = 0x020043ef      EVID1[0x3ed] = 0x000003ed
IVID1[0x3ef] = 0x020003ef      IVID1[0x3ed] = 0x000003ed
```

Et1 carries a `0x0200` prefix (bit 25, the `ET_IDX`/`IT_IDX` field) and `ETAG1 = 2`; our GLORT had
neither — only the identity value the replay writes across the whole table. Programmed
`EVID1[0x3ed] = 0x020043ed` and `IVID1[0x3ed] = 0x020003ed`, verified by readback.

### And still nothing egresses

Tried all three combinations — SPECIAL with DGLORT `0xff00`, SPECIAL with our own `0x03ed`, NORMAL
with `0x03ed`. Every one queues and completes; `lane1` never sets Transmitting; the test host's
`rx_packets` does not move.

### Remaining unreplicated

Two of the seven tables are still untouched for our GLORT: **`NEXTHOP_TABLE`** (5 writes, e.g.
`0x160015 = 0x03ef80a2` — the GLORT paired with a MAC, so this is the routed-next-hop binding) and
**`L2L_SWEEPER`** (12 writes, `0x03ef3333` — MAC aging). Neither is obviously required for a
special-delivery inject, which is why they were left, but "not obviously required" has been wrong
repeatedly in this session and they are the only replay-derived items left.

## Where this stands

Verified against a working reference and still not forwarding: link, SGLORT, GLORT_CAM, GLORT_RAM,
L2F destination mask, parser frame type, EVID1/IVID1/EVID2/IVID2. Seven prerequisites, each
confirmed by readback, none sufficient.

The honest conclusion has not changed and is now better supported: **CPU-inject to a port EOS never
configured needs state we cannot yet enumerate**, and the two candidates left from the replay
(NEXTHOP, SWEEPER) plus the undecoded L2AR are where it must live.


---

## ★ The positive control, which should have been run first

Injecting to **Et1** with the same tool, same boot, same moment:

```
lane0 before inject:  0x00000cc0
lane0 after inject:   0x00000ac0     <- bit 9, Transmitting, SET
lane1 (port 41):      0x000008c0     <- never sets Transmitting
```

**The inject path works.** `fm6000_txinline` reaches the wire on Et1 and does not on port 41, in
the same configuration seconds apart. That validates every negative result above: the failures are
about port 41 specifically, not about the injection mechanism, the DMA rings, or the tag.

This control was cheap and available from the beginning, and running it earlier would have saved
several rounds of "did that even do anything". A negative result is only worth the positive control
that frames it.

## Where the port-41 egress question now stands

Verified identical to a working reference and still not transmitting:

- link up, `lane1 = 0x8c0`, MAC receiving (`0xcc0` under host traffic)
- SGLORT `0x03ed` in `PARSER_INIT_FIELDS[41]`
- `GLORT_CAM[8]` → `GLORT_RAM[8]` → `DMaskBaseIdx 16` → `L2F[16].w1 = 0x200` = port 41
- `EVID1`/`IVID1`/`EVID2`/`IVID2` for our GLORT, matching Et1's pattern
- parser frame type: all three F64 types now expressible, e27 live
- tried DGLORT `0xff00`, `0x03ed`, `0xf000`; ftype normal and special; e29 shape

⚠ Also learned along the way: **`0x03ed` was never resolvable at all** — no `GLORT_CAM` entry
matches `0x03xx`, so those injections had no destination. Only `0xff00` and `0xf000` were ever
real tests.

Since the inject path demonstrably reaches the wire on one port and not the other, what remains
must be **per-port egress enablement** that Et1 has and port 41 does not — the scheduler/CM per-port
configuration, or a MOD egress-side per-port entry. Neither is in the GLORT-referencing set the
replay writes, which is why copying that set was not enough.

The two replay-derived tables still unreplicated (`NEXTHOP_TABLE`, `L2L_SWEEPER`) remain worth
doing, but the control makes a per-port egress enable the stronger hypothesis: a destination mask
that names a port the egress pipeline has not been told to serve is exactly what "queued, completed,
never transmitted" looks like.


## Systematic per-port sweep — what is and is not different

With the positive control established, the question narrowed to "what per-port state does Et1 have
that port 41 lacks". The header lists every per-port table (`ENTRIES = 76`), so this is a bounded
sweep rather than a hunt. Reading index 40 against index 41 on the live chip:

**Identical** (not the blocker): `ESCHED_CFG_1/2/3` (all `0x00ffffff`, and identical for port 20
too), `ESCHED_DRR_CFG`, `LAG_PORT_TABLE`, `CM_BSG_MAP`, `CM_TC_PC_MAP`, `CM_PC_RXMP_MAP`,
`CM_PAUSE_CFG`, `CM_ESCHED_STATE`, `ERL_CFG_IFG`, `MOD_MAP_DATA_W16E/W12A/W8A`, `MOD_TX_PORT_TAG`,
`MOD_DST_PORT_TAG` (`0x130`), `MOD_MIN_LENGTH` (`0x40`), `STATS_AR_TX_PORT_MAP`.

That rules out the egress scheduler and MOD's per-port configuration outright — both were strong
candidates for "queued, completed, never transmitted".

**Differed:**

```
LBS_CAM                  p40 0x0001fffe   p41 0x0003fffc
MAPPER_SRC_PORT_TABLE.w1 p40 0x00000049   p41 0x00000021
MCAST_LOOPBACK_SUPPRESS  p40 0xffff0001   p41 0xffff0003
SAF_MATRIX               p40 w0 0x00000007 w1 0x00000000
                         p41 w0 0x0010000f w1 0x00000100
```

Copied port 40's `SAF_MATRIX`, `MCAST_LOOPBACK_SUPPRESS` and `LBS_CAM` onto port 41, verified by
readback, and injected again: **still no Transmitting bit, still nothing at the test host.**

`MAPPER_SRC_PORT_TABLE` was deliberately left — it is ingress-side (source-port classification) and
cannot explain an egress failure, and it is checklist item B3 in its own right.

## Honest status

The elimination is now broad and the control is solid: injection reaches the wire on Et1 and not on
port 41, with link up, destination mask naming port 41, VID tables, GLORT resolution, frame type,
scheduler, and MOD per-port state all verified equal or correct.

What has *not* been examined is the one block this project has never decoded — **L2AR** — and the
two replay tables still unreplicated (`NEXTHOP_TABLE`, `L2L_SWEEPER`). Everything cheap and
enumerable has been checked. That is a reasonable place to stop and take A2 seriously, rather than
keep sampling registers.


---

## ★ Why none of it worked: GLORT is the wrong mechanism for an access port

Asked the question that should have come first — **what DGLORT does EOS itself use to reach Et3?**
— by walking every `GLORT_CAM` entry on a live EOS, following `GLORT_RAM[i].DMaskBaseIdx` into
`L2F_TABLE_256`, and checking which masks name port 41. Both plausible indexings of
`DMaskBaseIdx` were tried (`i1*4+i0` and `i1=flat`).

```
CAM entries whose mask includes port 41: 0
```

**None.** On a system where Et3 is `connected` and working, there is no GLORT that resolves to
port 41.

So EOS does not reach Et3 via a GLORT, and neither can we. `Et3` is a **switched VLAN-1 access
port**; frames reach it through the L2 destination lookup or a VLAN flood. The
`GLORT_CAM → GLORT_RAM → L2F` chain is the path for **routed and CPU-directed** traffic — which is
exactly why Et1 (`routed`, GLORT `0x03ef`) works through it and port 41 never will.

That single fact explains the whole sequence of failures above. Every prerequisite we satisfied —
SGLORT, GLORT_CAM entry, `DMaskBaseIdx`, `L2F` mask naming port 41, VID tables, frame type, SAF,
LBS, loopback-suppress — was correct *for a mechanism that does not apply here*. The masks EOS has
containing port 41 (`{0,41}` and `{0,3,20,40,41}`) are reached by the L2 lookup, not by any DGLORT.

### What to do instead

Two mechanisms actually deliver to a switched port, and both are testable:

1. **Flood.** Inject a broadcast frame into the VLAN and let it flood to every member port.
   `fm6000_txinline` already supports this — argv[6] is a `bcast` flag — so it is a one-command
   experiment, and the L2F masks EOS holds are exactly flood masks.
2. **L2 lookup hit.** Install a MAC→port-41 entry in the L2 table and inject a frame addressed to
   it.

Both are ordinary switching, and neither needs the GLORT machinery this section spent its time on.

### The lesson

The positive control proved injection reached the wire on Et1 and not port 41, and I read that as
"port 41 is missing configuration". The other reading — *Et1 and port 41 are reached by different
mechanisms* — was never tested, and it was the true one. A control that distinguishes two ports
does not tell you they differ only in degree.


---

## Broadcast inject does not flood — tested, with the control in the same run

The cheap test the mechanism argument implied: inject a broadcast and let the VLAN flood deliver
it. Port 3 enabled, and port 41 added to the three L2F flood masks EOS uses (full-entry writes,
verified by readback).

```
frame: ff ff ff ff ff ff | 44 4c a8 31 5d ab | 01 00 f0 00 ff 00 00 00 | 08 00

A  bcast, DGLORT 0xf000   lane0 0xcc0 (no TX)   lane1 0x8c0 (no TX)
B  bcast, DGLORT 0x03ef   lane0 0xac0 TRANSMIT  lane1 0x8c0 (no TX)
C  bcast, DGLORT 0x0000   lane0 0x8c0 (no TX)   lane1 0x8c0 (no TX)
```

**B is the control and it passes** — the same broadcast frame egresses Et1 when the tag names Et1.
So the inject path works, the frame is well formed, and the DMAC is genuinely broadcast.

**A CPU-injected frame is delivered by its DGLORT, not by a MAC lookup.** A broadcast destination
MAC changes nothing; the tag decides. That is consistent with the ISL tag's purpose — it exists
precisely to bypass the lookup — and it disproves the flood hypothesis for injected traffic.

Case C is the `e29` shape (DGLORT low 12 bits zero) and produces no egress anywhere, so "no
destination in the tag" does not fall back to a lookup either.

### What that leaves

Flooding to port 41 would have to be triggered by a frame that goes through the **L2 lookup**, and
a CPU-injected tagged frame never does. The remaining route to observing egress on port 3 is
therefore traffic that *ingresses from the wire* — i.e. a frame arriving on Et1 whose destination
resolves to port 41 — which needs the MAC table programmed, not the inject path.

That is ordinary L2 switching and it is the same A2/L2AR territory this document keeps arriving at,
but from a clearer direction: what is needed is a MAC→port-41 entry, not more GLORT plumbing.


## MAC-table route: structure known, entry placement blocked

`FM6000_L2L_MAC_TABLE` (`0x280000`, 4096 buckets x 16 ways x 4 words):

```
MAC[47:0]  FID1[59:48]  FID2[71:60]  Prec[73:72]  GLORT[95:80]  TAG[107:96]  DATA[115:108]
```

So the L2 lookup's destination **is a GLORT** — the lookup feeds back into the same
`GLORT_CAM -> L2F` chain, just reached from the wire instead of from the CPU. That is coherent with
everything above and means a MAC entry pointing at a GLORT that resolves to port 41 would work.

Two obstacles, both practical rather than conceptual:

1. **Placement needs the hash.** Entries are hash-bucketed; writing one requires the L2 hash
   function (`HASH_BASE 0xb000`, `fm6000_hashinit`), which is not decoded.
2. **Learning would place it for us, but port 41 cannot learn.** Learning requires the port to be
   in a VLAN with learning enabled — the forwarding-domain membership this whole document is about.
   Scanning the first 4 buckets after sustained host traffic found **no entries at all**, and
   `fmdump` cannot sparsely scan the remaining 4,092 (stride `0x4000`, 64 words used per bucket).

So the MAC route is circular with the original problem: it needs the VLAN membership that would
also have made flooding work.


---

## FM_PORT_MASK_WIDE located and fixed — necessary, still not sufficient

The SDK trace (`docs/SDK-TRACING.md`) named the attribute and gave the index arithmetic:

```
address = 0x1a0000 + (group-8)*0x400 + port*4        3-word entry
```

⚠ That corrects an error in the earlier L2F analysis here: I scanned the second index as `0..3`
(from `ENTRIES_1 = 4`) when the SDK indexes it by **port number**. The per-port masks live at
`port*4` within a block — offsets `0xa0` and `0xa4` for ports 40 and 41 — and I had never looked
there.

Reading the correct offsets, EOS versus EdgeNOS:

```
EOS      block 3, ports 0/3/20/40/41:  {0, 3, 20, 40, 41}
EdgeNOS  block 3, port 40:             {0, 3, 20, 40}      <- 41 absent
         block 3, port 41:             {0, 3, 20, 40}      <- 41 absent
```

**No port's forwarding mask included port 41.** In word terms `w1 = 0x100` where EOS has `0x300`.
That is a genuine, measured defect and it is now fixed on the live chip (all of ports 0, 3, 20, 40,
41 set to `0x300`, verified by readback, dataplane healthy at 35 routes).

### And still nothing egresses

With masks permitting port 41, `GLORT_CAM[8]` → `DMaskBaseIdx 16` → `L2F[16]` naming port 41, port 3
linked, and injection in both normal and SPECIAL frame types: no Transmitting bit, nothing at the
test host.

`FM_PORT_MASK_WIDE` is a **permission** mask — "may this source port forward to that destination
port" — not a destination selector. It gates delivery but does not cause it. So it joins the list
of things that had to be right and were not, without being the thing that makes a frame come out.

### Count of prerequisites now satisfied

link, SGLORT, `GLORT_CAM`, `GLORT_RAM`, `L2F` destination mask, `L2F` per-port permission mask,
`EVID1`/`IVID1`/`EVID2`/`IVID2`, F64 frame type (all three expressible), `SAF_MATRIX`,
`MCAST_LOOPBACK_SUPPRESS` — each verified by readback against a working EOS reference. Egress on
port 41 remains at zero, while the same injection reaches the wire on Et1 in the same breath.


---

## ★ The framing error: Et1 and Et3 were never in the same forwarding domain

From EOS's own `show interfaces status`, captured early in this investigation and not read closely
enough:

```
Et1   to-Edgecore5610-port6   connected   routed   10GBASE-SR
Et2   to-Edgecore5610-port7   connected   routed   10GBASE-CR
Et3                           connected   1        10GBASE-SR
```

**Et1 is a routed port. Et3 is an access port in VLAN 1.** They are in different forwarding domains
*on the working EOS system*. A frame arriving on Et1 is routed, not switched; it reaches Et3 only if
routed to a next hop on VLAN 1's subnet, and nothing is.

So the goal this document has been chasing — "get a frame that ingresses Et1 to egress port 3" — was
never achievable in the captured configuration, on EOS or on EdgeNOS. And `fwd4.txt` is a capture of
precisely that configuration, so the forwarding tables contain **no path to port 41 by
construction**. Every "necessary but not sufficient" result above is explained by this: the
individual pieces were right, and the path they were meant to complete does not exist in the data.

### What this means for the transit-traffic goal

A4 (MOD command split) and B1 (FFU ByteMux) need traffic that transits the switch. That requires
two ports in a forwarding relationship, and the cheapest way to get one is **not** to decode it:

1. Configure EOS so Et1 and port 3 are in the same domain — both access ports in one VLAN, or a
   routed path between two SVIs.
2. Confirm on EOS that traffic actually transits.
3. **Capture a new replay from that configuration.**
4. Boot EdgeNOS on the new replay; the tables now describe a path that exists.

That is hours of work rather than weeks, uses tooling that already exists, and sidesteps the entire
`FM_PORT_MASK_WIDE` / VLAN-membership decode — which becomes an optimisation rather than a blocker.

⚠ It also means the mask edits made during this investigation (`L2F` per-port masks set to `0x300`,
`GLORT_CAM[8]` repointed, `EVID1`/`IVID1` for glort `0x3ed`, the `0x03ed` SGLORT in `fwd4.txt`) were
attempts to hand-build a forwarding path the rest of the configuration has no notion of. They should
be reverted rather than carried forward — `fwd4-preport3.txt` is the pre-edit replay.

---

## ★ The SerDes SBus register map is in the header — and every difference is a *status* register

**2026-08-13, same boot as the DFE test above.** The previous entry ended by saying the gap is RX
lock, before adaptation. Chasing that produced a decode that should have been found much earlier.

### The header documents all 256 SBus SerDes registers

`fm6000_api_regs_int.h` defines **`FM6000_SERDES_ETH_WRITE_0..255`** and
**`FM6000_SERDES_ETH_READ_0..255`** with named bit fields — the read and write views of the SerDes
core registers reached over the SBus. Every `fm6000_sbusdump` column and every SBus op in
`fm6000_lanelink` can now be named instead of guessed. There are no address macros because these
are not MMIO; the register number *is* the SBus register field.

Read-view names for the registers that matter here:

| SBus reg | READ field |
|---|---|
| `0x02`–`0x06` | `sbus_rx_prbs_data_obs` |
| `0x09`, `0x0a` | `sbus_rx_error_count_obs` |
| `0x12`, `0x13` | `sbus_from_analog_obs` |
| `0x15` | `sbus_rx_elec_idle_detect_obs` b0, `sbus_tx_detect_rx_result_obs` b1, `sbus_tx_detect_rx_complete_obs` b2 |
| `0x1e`, `0x1f` | `sbus_dfe_scratch_obs` |
| write `0x0d` | `sbus_near_loopback_en_cntl` b7 (`SERDES_ETH_WRITE_13`) |

### 21 of 256 registers differ between the lanes, and all 21 are `_obs`

Full dump of the working lane (`0x49`, Et1) against the dark one (`0x4a`, port 3): 21 differences,
every one an **observation** register — PRBS data, error counts, analog readback, DFE scratch,
electrical-idle/tx-detect status. **Not one configuration register differs.** The lane is not
misconfigured; it is not receiving.

Two that read zero on the dark lane are the shape of the problem: `sbus_rx_error_count_obs`
(`0x06`/`0x04` on Et1, `0x00`/`0x00` on port 3 — a lane that never locks also never counts errors)
and `0x15` (`0x02` vs `0x00`: `tx_detect_rx_result` set on Et1, clear on port 3, with
`tx_detect_rx_complete` clear on both, so that result may simply be stale).

⚠ `0x21`–`0x28` change between consecutive reads on both lanes — they are live. Any single-shot
comparison of those is meaningless.

### The EPL side is provably identical

| register | Et1 | port 3 |
|---|---|---|
| `SERDES_CFG` (`0x34`) | `0x0aaa86c0` | `0x0aaa86c0` — PowerDown=0, NearLoopback=0, FarLoopback=0 |
| `SERDES_RX_CFG` (`0x39`) | `0x002a0281` | `0x002a0281` — RxEn=1, RxPolarityInvEn=0 |
| `SERDES_TX_CFG` (`0x3a`) | `0xc0000581` | `0xc0001581` — differs only by the intended pre=1 |

**RX polarity was the best remaining hypothesis for a TX-works/RX-dead split, and it is refuted:**
both lanes have `RxPolarityInvEn` clear. Note also that neither op table in `fm6000_lanelink`
writes `SERDES_CFG` at all — it is left at its boot value, which happens to be correct.

### ⚠ Readback-vs-EOS-writes is NOT a diagnostic here

Comparing what EOS wrote to `0x4a` during the successful bring-up against what the lane reads back
shows 7 of 8 registers "wrong" — but **the working lane fails the same comparison**. E.g. EOS wrote
`0x17=0x00` and both lanes read `0xc0`; wrote `0x2a=0x0e` and both read `0x00`. These are
command/strobe registers whose read view is different silicon from the write view. The mismatch is
a property of the register, not of the lane. Discard the method.

### Near loopback: no effect, and the test is not conclusive

`SERDES_CFG` bit 28 `NearLoopbackEn` set on port 3 (readback confirmed `0x1aaa86c0`), link ops
re-run under it, reverted after: `PORT_STATUS` stayed `0x15` and `pcsRx` stayed `0` throughout. Et1
was unaffected the whole time.

⚠ **This does not yet prove the RX path is dead.** The MMIO bit's efficacy is unvalidated — the
header also defines an SBus-side `sbus_near_loopback_en_cntl` (`WRITE_13` b7), and the SPICO may be
the actual consumer. There is no positive control: Et1 and Et2 are both up and carrying traffic, so
neither can be used as a loopback test subject without taking a working link down.
`fm6000_sbusdump` is deliberately read-only, so trying the SBus-side bit needs a new tool.

### What is now the cheapest untried discriminator: is light arriving at all?

Everything inside the chip matches a working lane, and the far end reports `Link detected: yes`.
That makes a one-directional optical fault — dead strand, dirty connector, failed SFP receiver —
a live hypothesis that has never been tested, and it is a five-minute check *if* the SFP DOM can be
read (SFF-8472 page `0xA2`, RX power at bytes 104–105).

**It cannot be read on this image.** `/dev/i2c-2` and `/dev/i2c-20..26` exist and busybox ships
`i2cget`/`i2cdump`, but **no device answers at `0x50` on any of them** — the SFP cages sit behind
the SCD's own I2C masters, which `services/scd-setup.sh` configures and this minimal M1 image does
not. Wiring that up is the next concrete step, and it is worth doing for platform services anyway.

### ⚠ Datasheet Table 9-4 does not agree with `sbus_dev()`

§9.4.3 maps SBus addresses to interfaces in **blocks of four** (PCIe 1–4, then each EPL), listing
EPL[14] at **41** (`0x29`) — not `0x49`. Our `0x49`/`0x4a`/`0x45` are empirical, taken from EOS's
own traces, and they work; so the EPL indices in `fm6000_serdes_ports.h` are evidently not the
datasheet's numbering. The consequence: `sbus_dev()`'s `0x49 + lane - 2*(epl-14)` extrapolation is
built on a structure the datasheet contradicts (+4 per interface, not +2), so it should not be
trusted for any port whose device address has not been observed. The tool already refuses to drive
unobserved mappings — keep it that way.

### ⛔ The optics are fine — measured, not assumed

The previous section named "is light arriving at all?" as the cheapest untried discriminator and
said it could not be read on this image. **That was wrong, and the correction matters more than the
mistake: everything needed is already in the image and already running.**

`scd` + `scd_hwmon` are loaded, `scd-setup.sh` runs at boot, and it had already created **61 I2C
buses and 468 `sfp*` nodes** before any of this investigation started. The earlier "no device at
`0x50` on any bus" came from probing `/dev/i2c-2` and `20..26` — a truncated listing I read as the
whole set. The SFP cages are on **master 3, buses 0–7 = `i2c-9`..`i2c-16` for Ethernet1..8**, so
port 3 is **`i2c-11`**.

SCD cage GPIOs, read straight out of sysfs:

```
sfp3_present=1  sfp3_rxlos=0  sfp3_txfault=0  sfp3_txdisable=0
sfp1_present=1  sfp1_rxlos=0  sfp1_txfault=0  sfp1_txdisable=0
```

**`rxlos=0` on port 3** — the module itself reports it is receiving light. SFF-8472 A2h confirms it
with numbers:

| | temp | Vcc | TX bias | TX power | **RX power** |
|---|---|---|---|---|---|
| Et1 (working) | 31.8 °C | 3.296 V | 6.28 mA | 596.9 µW (−2.24 dBm) | **469.7 µW (−3.28 dBm)** |
| Et3 (dark) | 31.4 °C | 3.316 V | 8.65 mA | 589.3 µW (−2.30 dBm) | **527.9 µW (−2.77 dBm)** |

**The dark port receives more optical power than the working one**, and both sit comfortably inside
10GBASE-SR limits. A2h byte `0x6e` reads `0x30` on both — no soft Rx_LOS, no soft Tx_Fault.

And the modules are the same part class, so a rate or media mismatch is out too — A0h byte 3 =
`0x10` (10GBASE-SR) and byte `0x0c` = `0x67` (10.3 GBd nominal) on **both**; only the vendor
differs (`CISCO-AVAGO` on Et1, `CISCO-FINISAR` on Et3).

### Where that leaves it

Everything outside the SerDes RX slicer is now measured good, on both sides:

- correct module, correct rate, healthy light **arriving** (−2.77 dBm) and **leaving** (−2.30 dBm)
- the far end reports `Link detected: yes, 10000Mb/s`, so our TX is being decoded by a real receiver
- no LOS, no TX fault, laser enabled, cage present
- every EPL and SerDes **configuration** register byte-identical to the working lane
- the only registers that differ anywhere are `_obs` status registers

So the signal reaches the chip and the chip does not lock to it. Port 3 is lane 1 of the same EPL as
Et1 (`0x49` lane 0, `0x4a` lane 1), which makes this a per-lane failure inside a block whose other
lane works — the RX analog/CDR for lane 1 is not being started, and the SPICO is what starts it.

**Next step: an SBus write tool.** Three of the remaining questions all need one and none can be
answered without it — the SBus-side `sbus_near_loopback_en_cntl` (`WRITE_13` b7) to get the positive
control the MMIO bit could not provide; issuing a SPICO interrupt at lane 1 and reading the response
to see whether its micro-controller answers for that lane at all; and re-running the lane-1 RX
start-up by hand. `fm6000_sbusdump` is deliberately read-only and should stay that way — this wants
a separate, single-purpose tool with the device address as an explicit argument.

### ★★ PORT_STATUS decoded: `SerXmit=0` — the lane is not transmitting either

**2026-08-13.** `FM6000_PORT_STATUS` is in the header, and reading it settles what this document has
assumed since it was started. Fields at `EPL_BASE + 0x400*epl + 0x80*lane`:

```
b0-1 LinkFaultDebounced   b6  RxLinkUp     b9  Transmitting
b2-3 LinkFaultMac         b7  HeartbeatOk  b10 Receiving
b4-5 LinkFaultRx          b8  HiBer        b11 SerXmit
```

| field | Et1 `0x0ec0` | Et2 `0x08c0` | **Et3 `0x0015`** |
|---|---|---|---|
| LinkFaultDebounced | 0 | 0 | **1** |
| LinkFaultMac | 0 | 0 | **1** |
| LinkFaultRx | 0 | 0 | **1** |
| RxLinkUp | 1 | 1 | **0** |
| HeartbeatOk | 1 | 1 | **0** |
| **SerXmit** | **1** | **1** | **0** |

Stable across repeated samples (`Transmitting`/`Receiving` fluctuate with traffic and are activity
bits, not state).

### ⚠ This overturns "our TX works, our RX never locks"

That framing appears throughout this document and it is **wrong**. `SerXmit=0` says the lane's
serializer is not transmitting at all. The two pieces of evidence that built the old story both
fail on inspection:

- **"The far end reports `Link detected: yes, 10000Mb/s`."** That was read from `eth1` on the test
  system — which is a **veth inside an LXC container** (`eth1@if11`, `veth addrgenmode`, and it
  reports `Port: Twisted Pair` on a fibre link). A veth reports link-up at 10 Gb/s whenever its
  container peer is up. It says nothing about the wire. The physical NIC is on the LXC host.
- **"The SFP is emitting 589 µW."** Laser bias current is on (`txdisable=0`), which produces light
  regardless of whether the serializer is feeding it valid 10GBASE-R. Optical output power is not
  evidence of transmission.

So the failure is not RX adaptation, and never was — **the whole lane is down, TX included**, which
is exactly what "the boot leaves lane 1's SerDes core asleep" said earlier in this document. The DFE
work, the polarity check, the loopback attempt and the optical measurements were all aimed at an RX
problem that is a *symptom*.

**What this redirects the work to:** whatever starts a lane's serializer. `SerXmit` is the thing to
watch — it is a single bit that says yes or no, it is stable, and neither `fm6000_lanelink`'s 168
link ops nor its 94 DFE ops move it. Any future attempt should be judged on `SerXmit` first and
`pcsRx` second, rather than on ping.

### Et3's forwarding config is now applied under EdgeNOS

Separately from the link: `fm6000_rport 41 0x3f0` was run on alpha9 and reports **VERIFY PASS
(9 words)**, with the before-state reading the documented access-port values (`0x123053=0x21`,
`0x327e0=0x3f0`, VID1/VID2/TX_TAGGED zero). Et3 is now a routed port at the ASIC level, matching
what EOS programs. This changes nothing about the link — forwarding config and PCS lock are
independent layers — but it removes one variable.

⚠ **No `et3` netdev.** `fm6000_portd` is one instance per interface and they share the single punt
DMA ring, so a second instance would steal Et1's frames and take the OSPF adjacency down.
`edgenos-up.sh` refuses to run twice for this reason. An `et3` netdev needs portd to learn multiple
ports, not a second process.

---

## ★★★ ROOT CAUSE: the replay provisions Et3's lane as an UNUSED lane

**2026-08-13.** Found by grepping the replay, offline, in minutes — no hardware, no new tool. The
question was simply *does the boot touch lane 1 at all?*, and the answer is worse than "no": it
touches it exactly as much as it touches the lanes with nothing plugged into them.

Per-lane EPL writes (`EPL_BASE + 0x400*epl + 0x80*lane`), identical in `fwd4-stock.txt` (pure EOS)
and our spliced `fwd4.txt`, so this is EOS's own doing and not a generator defect:

| lane | writes | distinct offsets |
|---|---:|---:|
| EPL14 lane0 — **Et1, links** | 459 | 20 |
| **EPL14 lane1 — Et3, dark** | **391** | **18** |
| EPL14 lane2 — nothing plugged in | 391 | 18 |
| EPL14 lane3 — nothing plugged in | 391 | 18 |
| EPL16 lane0 — **Et2, links** | 1236 | 20 |

**Et3's lane write sequence is byte-identical to unused lane 2.** Not similar — identical.

### The two enables are off

| register | Et1 lane | **Et3 lane** |
|---|---|---|
| `SERDES_TX_CFG` (`0x3a`) | `c0000581` — **TxEn=1** | `80000080` — **TxEn=0** |
| `SERDES_RX_CFG` (`0x39`) | `002a0281` — **RxEn=1** | `00280280` — **RxEn=0** |

`FM6000_SERDES_TX_CFG_b_TxEn` is bit 0 and `FM6000_SERDES_RX_CFG_b_RxEn` is bit 0, both from the
header. **That is `SerXmit=0` explained exactly**: the boot never enables the transmitter, so the
serializer never runs, so there is nothing for the far end to lock to and nothing for our own
receiver to do either.

The active lanes also get two registers the dark lane never sees at all —
**`AN_37_CFG` (`0x28`)** written once to `0`, and **`SERDES_IP` (`0x41`)** written three times
(`0x20`, `0xe0`, `0x3fff`) — plus far more traffic on the shared ones (`0x02`: 20 writes vs 2;
`0x04`: 10 vs 1, and the dark lane's single write is `0x00000001` where the live lane ends at
`0x07ffffff`; `0x3c`: 3 vs 1; `0x3b` ends `0x00000c83` vs `0x00000803`).

### ⚠ This explains why `fm6000_lanelink` cannot work, and it is not a bug in the op table

`fm6000_lanelink` replays the **Et3 no-shut window captured on EOS** — and on that machine the lane
was *already provisioned as an active port* with `TxEn`/`RxEn` set by EOS's own init, merely
administratively down. `no shut` is a small delta on top of a fully provisioned lane.

Under EdgeNOS the lane starts from the **unused-lane** state. The sequence is being applied to the
wrong initial state, and no amount of faithfulness in replaying it fixes that. The same applies to
the DFE window, which was captured on a lane that was up.

That retires, in one stroke, the DFE hypothesis, the RX-polarity hypothesis, the near-loopback
result and the optical investigation. All of them were downstream of a transmitter that was never
switched on.

### What to build

**Lane 0's 459-write init sequence, retargeted to lane 1** — the same "relocate the sequence"
treatment already used for EPL, and `fm6000_lanelink` already has the retargeting machinery
(`base + 0x400*epl + 0x80*lane`, SBus device, SPICO reg-0x03 payload). Apply it *before* the
existing link ops, which then find the state they were captured against.

⚠ Retargeting is **not purely mechanical** — some values are legitimately per-lane. `0x10` ends
`0x2000033c` on lane 0 against `0x2000031c` on lane 1 (one bit), and `SERDES_TX_CFG` carries the
port's own pre/post emphasis, which `patch_tx_cfg()` already handles. Diff lane 0 against lane 2
(both fully written, one active one not) to separate "because the lane is active" from "because it
is lane N" before generating anything.

**Judge the result on `SerXmit`**, which is bit 11 of `PORT_STATUS`: one bit, stable, currently 0,
and 1 on both working ports.

### Provisioning applied on hardware — necessary, still not sufficient

**2026-08-13, alpha9, live.** `INIT[]` + `SEQ[]` + `DFE[]` = **963 ops, all clean**, no SBus
failure, no off-bus, Et1 untouched throughout (`0x0ec0`, routes 35).

Readback afterwards — **every EPL register on Et3 now matches the working lane**:

| | Et1 | Et3 |
|---|---|---|
| `SERDES_TX_CFG` | `c0000581` (TxEn=1) | `c0001581` (TxEn=1, port 3's emphasis) |
| `SERDES_RX_CFG` | `002a0281` (RxEn=1) | `002a0281` (RxEn=1) |
| `AN_37_CFG` | `00000000` | `00000000` |
| `SERDES_CFG` | `0aaa86c0` | `0aaa86c0` |
| **`PORT_STATUS`** | **`0x0ec0`** | **`0x0015` — SerXmit still 0** |

Also tried: `PowerDown=3` on lane 1 (`SERDES_CFG` bits 30:31), back to 0, then the full 963 ops
again — on the theory that the config needs a power cycle to latch. **No change.**

So enabling the transmitter in the EPL block does not start it. The remaining difference is inside
the SerDes core, where it was all along:

```
SBus reg   0x12 from_analog_obs   Et1 0x26   Et3 0xfc
           0x13 from_analog_obs   Et1 0xe0   Et3 0x80
           0x15 tx_detect_rx      Et1 0x02   Et3 0x00
```

### What this says about the approach

The replay sends lane 1 **391 MMIO writes and zero SBus ops** — its SerDes core has never been
addressed, so it is sitting in power-on state. We retargeted lane 0's 44 SBus and 198 SPICO ops to
it and that was not enough to bring the core up.

Two candidate explanations, and they need different work:

1. **The SerDes bring-up is not relocatable.** Unlike the EPL block, it may only work inside its
   original position in chip init — after a particular reset, or in a window where the SPICO is in a
   known state. "Relocate the sequence" is proven for EPL and CM/L2F; it has never been shown to
   work for SerDes.
2. **The SPICO does not answer for lane 1.** The 198 SPICO ops we replay are interrupt-style
   commands whose responses we write and never read. If its micro-controller was never started for
   that lane, every one of them is a no-op and nothing downstream can work.

**(2) is testable and (1) is not, so test (2) first** — and it needs the SBus write tool: issue one
SPICO interrupt at `0x4a`, read the response, and compare against the same interrupt at `0x49`. A
lane whose SPICO answers and one whose SPICO does not are trivially distinguishable, and that single
measurement decides whether any amount of register replay can ever work here.

### ★★ The SPICO answers for lane 1 — and it reports three distinct lane states

**2026-08-13.** `fm6000_sbus irq` issues one SPICO interrupt at a named target and prints the
response registers. Running the same interrupt (code `0x20`) at every lane:

| target | lane | **resp reg 0x01** | resp reg 0x02 |
|---|---|---:|---|
| `0x49` | Et1 — cabled, **working** | **2** | `0x2b` |
| `0x45` | Et2 — cabled, **working** | **2** | `0x32` |
| `0x4a` | **Et3 — cabled, dark** | **1** | `0x40` |
| `0x4b` | EPL14 lane2 — nothing plugged in | **0** | — |
| `0x4c` | EPL14 lane3 — nothing plugged in | **0** | — |
| `0x46` | EPL16 lane1 — nothing plugged in | **0** | — |

**The SPICO answers for lane 1.** That retires the second candidate: its micro-controller is alive
for that lane, so the 198 interrupt-style ops are not no-ops firing into a dead block.

### ★ And the provisioning demonstrably moved the lane — 0 → 1

`reg 0x01` reads **0** on every lane with nothing plugged in, **1** on Et3, **2** on both working
ports. Et3 is one state short of working, and it did not start there:

- the replay writes EPL14 lanes 1, 2 and 3 **byte-identically** (established above), so after boot
  those three lanes must be in the same state;
- lanes 2 and 3 read `0`, and lane 1 reads `1`;
- therefore the difference was produced by what we applied to lane 1 afterwards — the 963-op
  provision + link + DFE run.

⚠ Inferred, not directly measured: no reading of `reg 0x01` was taken on lane 1 *before* the
provisioning ran. The argument rests on the byte-identical replay, which is solid, but a direct
before/after on a fresh boot would settle it and costs one reboot.

**So the provisioning is not inert — it advances the lane, and `PORT_STATUS` simply cannot show it.**
`SerXmit` stays 0 across a transition the SPICO reports plainly. This is now the instrument to use:
one interrupt, one register, three values, and it distinguishes states that every EPL register in
the chip reports identically.

**What is left is the 1 → 2 transition**, and it is a much better-posed question than "why is the
lane dark". Two ways at it, in order of preference:

1. **Find the interrupt that performs it.** `libFocalpointSDK.so` in the EOS SWI on flash carries 41
   DFE/SerDes symbols (found earlier with `grep -ao`; EOS ships no `strings`/`nm`). The interrupt
   codes are constants in that code. Reading them is analysis, not a live experiment.
2. **Sweep interrupt codes at `0x4a` and watch `reg 0x01`.** Bounded — the lane is already dead, and
   the target is named per interrupt. But an unknown code could disturb the shared SPICO and take
   Et1's adjacency with it, so this is the fallback, not the opener.

---

## ★★★ LANE_STATUS names the remaining gap: signal detected, no block lock, RxRate = 0

**2026-08-13, end of session.** A full live diff of the two EPL lane blocks — all 128 words of
`0xe3800` (Et1) against `0xe3880` (Et3) — leaves only ten differences, and naming them from the
header settles what they are:

| offset | register | Et1 | Et3 |
|---|---|---|---|
| `+0x00` | `PORT_STATUS` | `0x0ec0` | `0x0015` |
| `+0x04` | (status) | `0xbb87` | `0` |
| `+0x20` | `MAC_CODE_ERROR_COUNTER` | `0x596` | `0` |
| `+0x21` | `MAC_LINK_COUNTER` | `0x14803055` | `0x1001` |
| `+0x26` | pcsRx | `1` | `0` |
| `+0x36` | `PCS_10GBASER_RX_BER_STATUS` | `0x45` | `0` |
| `+0x38` | `LANE_STATUS` | `0x000940` | `0x018000` |
| `+0x3a` | `SERDES_TX_CFG` | `0xc0000581` | `0xc0001581` |
| `+0x3e` | (unnamed) | `0x00100f0f` | `0x0c100000` |
| `+0x42` | `LANE_DEBUG` | `0x343` | `0` |

**Every one is a counter or a status register**, except `SERDES_TX_CFG`, which differs only by port 3's
intended pre-emphasis. So **there is no configuration difference left between the dark lane and the
working one** — not in the EPL block, and not in the SerDes (PLL locked, `rx_rdy` = `0x3f` matching
Et1, signal strength 3 of 3).

`LANE_STATUS` decodes the failure exactly:

| field | Et1 | Et3 |
|---|---|---|
| `PcsBaserBlockLock` b6 | **1** | **0** |
| `RxRate` b7-12 | **0x12** | **0** |
| `RxSignalDetectSample` b13-16 | 0 | **set** |

**The lane sees signal and never achieves 10GBASE-R block lock, and its recovered rate reads zero.**

That is a far sharper statement than "port 3 is dark", and it points at the rate path rather than at
enables or gates: a receiver that detects light but recovers no rate is not clocking the incoming
data correctly. The candidates, in order:

1. **The divider fields.** Steps 5 and 6 write `0x36` and `0x3b` with the rate constant `0x40`, taken
   from the 10G branch of the SDK's rate table. If the field is positioned or scaled differently than
   assumed, the SerDes runs at the wrong rate and block lock is impossible.
2. **Step 3's `ref_sel`.** `0x37` made the PLL lock in 5 ms with `rx_rdy` exactly matching Et1, which
   is encouraging but does not prove the reference is right for 10.3125 Gbps.
3. **Step 12 (`0x1f`)**, still undecoded, and the one remaining write in the enable path.

`RxRate` is now the instrument to use — it is a six-bit field that reads `0x12` on a working 10G lane
and `0` here, so any change to the rate path can be judged directly instead of through `SerXmit`.

### ★★★ WHY PORT 3 IS NOT UP: the DFE engine has never run on it

**2026-08-13.** Two measurements close the argument.

**1. Near loopback does not lock either.** With `sbus_near_loopback_en_cntl` (`0x0d` b7) set — the lane
fed its own transmit — `LANE_STATUS` stayed `0x0001c000`: `BlockLock=0`, `RxRate=0`. If our
transmitter were running, a loopback has no fibre, no far end and no attenuation to blame; it would
lock. So `SerXmit=0` is a true report, and **the far end is not the problem** — a hypothesis that had
been open since the optical measurements.

**2. The DFE registers are empty.** A full SBus dump of the working lane against the dark one, taken
*after* the enable sequence, leaves 24 differences, and the shape of them is the answer:

```
0x19 0x0f -> 0      0x1e 0x0b -> 0      0x20 .. 0x27   populated -> ALL ZERO
0x1a 0x0f -> 0      0x1f 0x25 -> 0      (the DFE taps)
```

`0x1e`/`0x1f` are `sbus_dfe_scratch_obs` and `0x20`-`0x27` are the DFE tap/state block. On Et1 they
carry live, changing values. On Et3 **every one of them is zero**. The receiver's equaliser has never
adapted, because nothing has ever asked it to.

This is exactly what this repository already knew, in `fm6000_spico.c`:

> *without the SPICO running, the SerDes RX equalizer never adapts and ports don't train*

### The chain, end to end

```
DFE never runs  ->  receiver never adapts  ->  no 10GBASE-R block lock
                ->  RxRate reads 0         ->  PCS never comes up
                ->  MAC never transmits    ->  SerXmit = 0  ->  port down
```

Every earlier symptom sits somewhere on that chain, which is why each fix in turn was necessary and
none was sufficient. The provisioning, the enables, the clock gate and the reference select all had
to be right — and they now are, with the PLL locked, `rx_rdy` matching Et1 exactly and signal
strength at maximum — but the last step was never taken.

### What to build next, precisely

**Step 17, DFE tuning**, which `fm6000StartSerDesDfeTuning` (`0x4877a1`) shows is not a SPICO
interrupt but three `WriteSBus`, one `ReadSBus`, one `and $0xffffffe0`, and
`fm6000SetSerDesRxDataGate(.., 0)`, over registers **`0x17`, `0x2a`, `0x2b`** — the same registers
`fm6000_lanelink`'s captured `DFE[]` table writes, which is a useful cross-check on both.

Judge it on the DFE taps themselves: `0x20`-`0x27` going non-zero is the proof the engine ran, and it
is visible without waiting for the port to come up. Then `RxRate` (`LANE_STATUS` b7-12, `0x12` on a
working lane), then `SerXmit`.

### ★★ The DFE engine now runs on Et3 — taps populated for the first time

**2026-08-13.** Step 17 implemented from `fm6000StartSerDesDfeTuning` (`0x4877a1`): read `0x17`, write
it back with bits [4:0] cleared, then `0x2a <- 0x08` and `0x2b <- 0x02`. (Its
`fm6000SetSerDesRxDataGate(.., 0)` is deliberately left out — step 11 opens that gate and the
argument's polarity is not established.)

```
17 0x17 [4:0] cleared    reg 0x17 <- 0x00
17 0x2a <- 0x08
17 0x2b <- 0x02
DFE taps 0x20-0x27:  fe 0e ee 30 7e 0e ee 30   <-- NON-ZERO
```

Against a working lane's `fe 0e 61 d0 7e 0e 61 d0`: **four of the eight are identical** (`fe 0e` and
`7e 0e`) and the other four are the adapted coefficients, which are expected to differ per lane and
per channel. Every one of them was zero on this lane an hour ago.

So the equaliser has run and adapted. `fm6000_serdes_enable` now implements **16 of 18 steps**.

**And the lane still does not lock:** `LANE_STATUS = 0x00018000`, `BlockLock=0`, `RxRate=0`,
`SerXmit=0`.

### Where that leaves it

The receiver is now, by every measurement available, alive: clock gated on, PLL locked in 5 ms with
`rx_rdy` identical to Et1, signal strength at maximum, DFE adapted with plausible coefficients. The
PCS above it still finds no 10GBASE-R block lock and recovers no rate.

Two candidates remain, and they are testable in this order:

1. **DFE completion.** `fm6000CheckSerDesDfeTuningState` exists precisely because tuning is not
   instantaneous — the earlier gdb session read `coarse=2 fine=1` on working lanes. One pass of the
   start sequence may not be convergence; the taps being non-zero says it ran, not that it finished.
2. **Step 12 (`0x1f`)**, the last undecoded write in the enable path — and `0x1f` reads `0x25` on Et1
   against `0` on Et3, which is now one of the few remaining SerDes differences.

⚠ `0x1f` is `sbus_dfe_a_adv_cntl_*` in the write view and `sbus_dfe_scratch_obs` in the read view —
DFE registers both. Given that the remaining fault is DFE-adjacent, step 12 is the more interesting
of the two despite being the harder to decode.

### ★★ DFE fine tuning now runs too — coarse and fine both active on Et3

**2026-08-14.** `fm6000CheckSerDesDfeTuningState` (`0x48d9e2`) reads **register `0x1f`** and extracts
two-bit fields with `>>2 &3` and `>>4 &3` — the `coarse` and `fine` pair the earlier gdb session read
as `coarse=2 fine=1` on working lanes. That makes `0x1f` the DFE state register, and it gave a
precise diagnosis:

| | `0x1f` | coarse | fine |
|---|---|---|---|
| Et1 (works) | `0x25` | 2 | **1** |
| Et3 after one DFE pass | `0x21` | 2 | **0** |

**Coarse tuning completed on Et3; fine tuning had not started.** Repeating the start sequence five
times did not change it — the taps oscillated between `0xcc` and `0xee` while `fine` stayed 0.

`fm6000RestartSerDesDfeFineTuning` (`0x489bef`) is one read and one write, both on register **`0x2a`**,
modifying it as `(read & ~0x06) | X`. The live DFE capture writes `0x2a = 0x0a`, and
`(0x08 & ~0x06) | 0x02 = 0x0a`, so `X = 0x02` — bit 1 restarts fine tuning.

Run on Et3:

```
before   0x1f = 0x21   coarse=2  fine=0
0x2a <- 0x0a
after    0x1f = 0x69   coarse=2  fine=2
```

**Fine tuning now runs on that lane for the first time.** Note Et3 settles at `fine=2` where Et1 reads
`fine=1`, so the two are in different fine states — `1` may be "converged" and `2` "in progress", or
the reverse; the field's encoding is not established.

**Still `BlockLock=0`, `RxRate=0`, `SerXmit=0`.**

### The receiver is now fully alive by every available measure

```
clock gate      on, ref_sel loaded          PLL lock        5 ms, rx_rdy = 0x3f (identical to Et1)
rx_en/tx_en     set                         signal strength 3 of 3 (maximum)
tx_output_en    set                          DFE coarse      2 (identical to Et1)
data + DFE gates open                        DFE fine        running (0 -> 2)
```

and the PCS above it still finds no 10GBASE-R block lock and recovers no rate. Every layer below the
PCS now matches or exceeds what a working lane reports.

**Next, in order:** whether `fine` must reach `1` rather than `2` (poll it while re-running the
restart, and compare against Et1's behaviour under the same operation); then step 12, the last
undecoded write, which targets `0x1f` itself — the very register this section is reading.

### ⛔ EOS's own SerDes sequence, replayed exactly, still does not lock

**2026-08-14.** Candidate 2 turned up something better than step 12's value. Extracting **every SBus
write EOS makes to a working lane's SerDes** from the replay — all 30 to device `0x49` — gives the
actual bring-up, and it is much smaller than `fm6000EnableSerDes`:

```
0x01 <- 0x1f      0x17 <- 0x10      0x2a <- 0x0e
0x02 <- 0x3f      0x06 <- 0x00      0x2b <- 0x02
                  0x17 <- 0x00      then 0x2a alternating 0x16 / 0x0e, 13 times
```

**No `0x22`, no `0x0d`, no `0x1d`/`0x36`/`0x3b`, and no `0x1f` at all** — so step 12's value does not
matter on this path, and several registers our tool writes are ones EOS never touches. It also
writes `0x01` and `0x02` first, which nothing in our sequence did, and sets `0x06` to **`0`** where
the `EnableSerDes` decode had us setting `0x08`.

The thirteen alternations of `0x2a` between `0x16` and `0x0e` are the fine-tuning loop — bits 1 and 2
held while bits 3 and 4 toggle.

Replayed verbatim on Et3 after a device reset:

```
after setup                    LANE_STATUS=0x00018000  BlockLock=0 RxRate=0  0x1f=0x01
after 13 tuning iterations     LANE_STATUS=0x00018000  BlockLock=0 RxRate=0  0x1f=0x29
```

`0x1f` reaches **`0x29`** — coarse 2, fine 2 — against Et1's `0x25` (coarse 2, fine 1). The DFE state
is now one field away from a working lane, and the PCS still finds no block lock and no rate.

### What that eliminates, and what it leaves

This was the strongest possible version of the replay approach: EOS's exact writes, in EOS's order,
to a device put in a known state by reset, on hardware where the write path is proven. It is not a
question of missing registers or wrong values any more.

So the remaining difference is **not in the SerDes register set**. What is left, in order of
plausibility:

1. **Ordering against the EPL block.** In the replay these SerDes writes happen *inside* a full chip
   bring-up, interleaved with that lane's EPL configuration. We configure Et3's EPL block by copying
   a finished state and then run the SerDes sequence — the reverse order, and PCS block lock is an
   EPL-side function that may need to be started after, or reset alongside, the SerDes.
2. **The `fine` field.** Et3 sits at 2 where Et1 sits at 1, across every route tried. If `1` means
   converged, the loop is running and never settling — which would point back at signal quality
   despite the optical measurements.
3. **Something the EPL does at boot that we have never replicated**, since Et1's lane was configured
   by the replay from reset while Et3's was configured by us from a running chip.

The cheapest test of (1) is to run the SerDes sequence **before** applying the EPL lane provisioning,
rather than after — a reordering, not new decoding.

### All three candidates tested — and the SerDes is now indistinguishable from a working lane

**2026-08-14.**

**Candidate 2 dissolves — `0x1f` is dynamic.** Read across all three lanes in one pass:

```
dev 0x49 (Et1, works)   0x1f = 0x29   coarse=2 fine=2
dev 0x45 (Et2, works)   0x1f = 0x29   coarse=2 fine=2
dev 0x4a (Et3, dark)    0x1f = 0x29   coarse=2 fine=2
```

**All three identical.** The earlier "Et1 fine=1, Et3 fine=0" was a *moving* register sampled at
different moments, not a state difference — so no conclusion should have been drawn from it, and the
"one field away from a working lane" reading was wrong. Et3's DFE state matches both working lanes
exactly.

**Candidate 1 fails — ordering is not it.** Reset the device, run EOS's SerDes sequence first, then
the EPL link sequence:

```
after serdes    LANE_STATUS=0x00018000  BlockLock=0 RxRate=0
after link ops  LANE_STATUS=0x00018000  BlockLock=0 RxRate=0
```

No change in either order.

### Where this genuinely stands

Every measurable property of Et3's SerDes now matches a working lane: clock gate, reference select,
PLL lock time and `rx_rdy`, signal strength at maximum, all ten configuration registers, and now the
DFE coarse/fine state. Every EPL lane register matches apart from counters. The PCS above it finds
no 10GBASE-R block lock and recovers no rate, in both orderings, with and without near loopback.

The chip-side explanations are exhausted. Two things remain, and neither can be settled from here:

1. **Provision the lane from a chip reset, exactly as Et1's is.** Every difference tried so far has
   been applied to a *running* chip. Et1's lane is configured by the replay during a cold boot, in
   context, from reset. The test is to splice the 30 SerDes writes for device `0x4a` into
   `fwd4.txt` alongside the existing `0x49` ones and cold-boot — the project already has the
   splicing tooling for exactly this shape of experiment (`gen_split` / `gen_list`).
2. **Verify the far end actually transmits valid 10GBASE-R.** The optical measurements prove light
   arrives at good power, and `RxSignalDetectSample` confirms the lane sees it — but neither proves
   the *content* is a valid 10G bitstream. The far end is a NIC on the Proxmox host that has never
   been checked, and a link partner with its laser on but no valid encoding produces exactly this
   signature: signal present, no block lock, no rate.

⚠ The near-loopback attempt was meant to settle (2) and does not, on reflection: it wrote `0x0d`,
a register **EOS never writes on this path**, so that test may have disturbed the very TX it was
trying to loop. It should be redone using only registers EOS itself uses.

### ⛔ The cold-boot splice: no effect on Et3 — and ⚠ Et2 is now degraded

**2026-08-14.** The 44 SBus ops the replay issues to Et1's SerDes (`0x49`) were mirrored onto Et3's
(`0x4a`) and spliced into `fwd4.txt`, so that lane would be provisioned from a chip reset in exactly
the same context as the working one. The splice is clean — 44 ops mirrored including the op-`0x20`
reset, every MMIO line byte-identical, no other device touched, `+132` lines.

Cold-booted on it, FULLSEQ complete in 131 s:

```
Et1  PORT_STATUS=0x0cc0  SerXmit=1  BlockLock=1  RxRate=0x12     works
Et3  PORT_STATUS=0x0015  SerXmit=0  BlockLock=0  RxRate=0        unchanged
```

**No effect on Et3.** Provisioning the lane from cold, in context, with EOS's own writes, does not
bring it up either. Candidate 1 from the previous section is therefore closed.

### ⚠ Et2 regressed, and the splice is not the cause

Et2 read `0x08c0` all evening and now reads **`0x0815`** — `SerXmit=1` but `RxLinkUp=0` with all three
link-fault bits set. It transmits and does not receive.

**Rolled back**: `fwd4.txt` restored from `fwd4-presplice.txt` (md5 verified against the local copy),
cold-booted again — and **Et2 stayed at `0x0815`** while Et1 returned to `0x0cc0` and forwards. So the
splice did not cause it; the state is older and survives a cold boot and a full replay of the
known-good file.

The likeliest cause is one of this session's own write tests on Et2's SerDes. To settle the "do our
writes land" question, `0x0a` (`tx_pattern_gen_en`) was set on device `0x45` with four different
values, and `0x22`/`0x0d` were cleared and restored. At the time each was judged to have "no effect"
because `PORT_STATUS` did not move — but this session later proved writes *do* land, so that
judgement was made with the wrong instrument. **A lane transmitting PRBS into an Edgecore port is
exactly the kind of thing that gets a port err-disabled at the far end**, and `sfp2` still reports
`rxlos=0`, so the cable is live and the fault is above the physical layer.

⚠ **Et2 needs its far-end port bounced on the Edgecore 5610 to confirm.** That is the test: if the
link returns after the peer port is shut/no-shut, the damage was to the far end's port state and
nothing on this switch is broken.

**The lesson is the one this document keeps relearning, in a more expensive form:** a test judged
against the wrong instrument does not merely fail to inform — it can do damage while reporting
nothing. `PORT_STATUS` was known by then to be an EPL/MAC view that SerDes-level writes do not move.

### ⛔ The far end is exonerated — and near loopback proves the fault is ours

**2026-08-14.** The far end of Et3 is `enp4s0d1` on the Proxmox host, bridged to `vmbr1` alongside the
container's `eth1` veth. `ethtool` reports it a **10G fibre port, administratively up**, supporting
`10000baseT/Full`, auto-negotiation off, `Link detected: no`. Its module cannot be read — the `cxgb4`
driver does not implement `ethtool -m` — but the operator confirms it is the correct module and
**this link has been up before**.

So the far end is transmitting, which matches the −2.77 dBm measured at Et3's SFP, and its
`NO-CARRIER` is *our* doing: `SerXmit=0` means we have never given it anything to lock onto. Both
ends have been reporting no-link for opposite reasons.

**Near loopback, done properly.** With the lane fully provisioned (`RxEn=1`, `TxEn=1`, `SigDet=12`),
`SERDES_CFG` bit 28 `NearLoopbackEn` set, and both `lanelink` and `serdes_enable` re-run underneath
it:

```
loopback off, provisioned    LANE_STATUS=0x00018000  BlockLock=0 RxRate=0 SigDet=12
loopback on                  LANE_STATUS=0x00018000  BlockLock=0 RxRate=0 SigDet=12
loopback on + re-provisioned LANE_STATUS=0x0001a000  BlockLock=0 RxRate=0 SigDet=13
```

`SigDet` moves, so something in the path responds. **Block lock never appears.** With the fibre, the
optics and the far end all removed from the path, the lane still cannot recover a rate from its own
transmitter — so the fault is inside this chip, and every remaining far-end hypothesis is closed.

⚠ One caveat kept honest: nothing confirms `NearLoopbackEn` actually engages. The bit is set and
`SigDet` changes, which is suggestive, but the SBus-side `sbus_near_loopback_en_cntl` is a register
EOS never writes on this path and was avoided deliberately.

### ⚠ A false negative worth recording

The first attempt at this test, immediately after the cold boot, read `LANE_STATUS=0x00000000` with
`SigDet=0` through every step and looked like a total failure. It was not: the cold boot returns the
EPL lane block to its **unused-lane** state, so the SerDes work had nothing above it. Running
`lanelink` first — restoring `RxEn`/`TxEn` — brought `SigDet` straight back to 12.

**Every SerDes-level experiment needs the EPL provisioning applied first**, and after any reboot that
must be redone. This is the third time in this document that a result was nearly misread because the
lane was in a different state than assumed.

### Also validated by accident

`edgenos-up.sh` was run to restore the control plane, and the pinned mgmt route
(`10.22.1.0/24 via 10.1.1.1 dev eth0 metric 5`) **held through `ospfd` installing its 34 routes** —
mgmt SSH survived where it previously died. That is the first live confirmation of the fix now in
`init-m1`, which until this point had only been reasoned about.

---

## ★★★ ROOT CAUSE FOUND: Et3's PCS was never enabled

**2026-08-14, by static analysis.** Going back to the SDK with everything the hardware work had
taught us found it in minutes — in a register no amount of lane-block diffing could ever have
surfaced.

`fm6000GetPcsSel` / `fm6000InitPcsSel` led to **`FM6000_EPL_CFG_B`**, which is **per-EPL**, at
`0x400*epl + 0x302 + EPL_BASE`, and carries a **4-bit PCS mode per port**:

```
Port0PcsSel b0-3   Port1PcsSel b4-7   Port2PcsSel b8-11   Port3PcsSel b12-15   QplMode b16-18
```

Read on EPL14, which carries **both** Et1 (lane 0) and Et3 (lane 1):

```
EPL_CFG_B(14) = 0x00090003
    Port0PcsSel = 3    <- Et1, works
    Port1PcsSel = 0    <- Et3, dark
```

Datasheet §6.8.9: *"PCS can be changed from any mode to any mode on a port-by-port basis and is
configured in EPL_CFG_B register. The correct method is to first set to **PCS_DISABLE** and then
change to the desired mode."* **`0` is PCS_DISABLE.** Et3's PCS has been switched off the whole time.

### Setting it turned the transmitter on

```
EPL_CFG_B(14): 0x00090003 -> 0x00090033      (Port1PcsSel 0 -> 3)

before   PORT_STATUS = 0x0015   SerXmit = 0
after    PORT_STATUS = 0x0815   SerXmit = 1
```

**`PORT_STATUS` moved for the first time in this entire investigation, and `SerXmit` went to 1.**
Et1 was unaffected throughout, routes stayed at 35.

### Why nothing else could have found it

- It is **per-EPL, not per-lane**, at offset `0x302` — outside the `0x80`-word lane blocks that every
  diff in this document compared. Et1 and Et3 are *fields of the same register*, so "identical
  configuration" was true of everything examined and false of the one thing that mattered.
- The replay never writes it for lane 1, consistent with everything else it treats as an unused lane.
- Every SerDes-level measurement was correct and irrelevant: PLL lock, signal strength, DFE coarse
  and fine all sit **below** the PCS, so they could be brought to match a working lane exactly while
  the PCS above them was switched off.

That also retires the near-loopback result: a disabled PCS cannot block-lock on any signal, internal
or external, so that test could never have succeeded regardless of the loopback bit working.

### Still to do

RX does not lock yet — `BlockLock=0`, `RxRate=0`, and Et3 now sits at `0x0815`, transmitting and not
receiving, which is **the same state Et2 is in**. With our transmitter finally running, the far end
should now see carrier for the first time; whether it links is the next thing to check, on the
Proxmox side (`enp4s0d1`).

⚠ `Port1PcsSel = 3` was chosen by copying what Et1 and Et2 use. The datasheet's mode encoding has not
been read, and `3` may not be the correct mode for every media type — it is simply what both working
10G ports on this board are set to.

### ★★ A second lane-1 gate: `EPL_CFG_A.Active_1`

Finding `EPL_CFG_B` by symbol was luck; the class it belongs to is the real lesson. Searching the
header for **every register with per-port fields** turns up three:

```
FM6000_EPL_CFG_A       Port0ReverseTxLanes / Port0ReverseRxLanes, and Active_0..Active_3
FM6000_EPL_CFG_B       Port0PcsSel .. Port3PcsSel, QplMode
FM6000_EPL_LED_STATUS  per-port LinkUp / Transmitting / Receiving / ...
```

`EPL_CFG_A` (`0x400*epl + 0x301 + EPL_BASE`) carries **`Active_0`..`Active_3` at bits 19-22** — a
per-lane active enable. On EPL14:

```
EPL_CFG_A(14) = 0x7e0d7899    Active_0 = 1  (Et1)    Active_1 = 0  (Et3)
```

**A second gate held shut for lane 1**, in the same blind spot as the first: per-EPL registers with
per-port fields, which no lane-block diff can ever surface because both lanes live in one register.
Set to `0x7e1d7899`.

### Where the night ends

```
PcsSel(port1) = 3      Active_1 = 1      SerXmit = 1      SigDet = 12
BlockLock = 0          RxRate = 0        PCS_10GBASER_RX_STATUS = 0   (Et1 reads 1)
```

Both gates open, the full bring-up re-run underneath them, the far end confirmed correct and bounced
by the operator — and the receiver still does not block-lock. Et1 unaffected throughout, 36 routes.

### ⚠ The reference problem, stated plainly

**No lane other than lane 0 has ever been active on this switch.** EPL14 and EPL16 both read
`Active_0=1` with every other lane 0, and both working ports are lane 0. So for every per-EPL,
per-port field there is *no known-good lane-1 value anywhere on this box* — `Active_1 = 1` and
`PcsSel(port1) = 3` were both chosen by analogy with port 0, not read from a working example.

That is the same trap this document fell into with `fine=2` versus `fine=1`: comparing against a
reference that does not exist.

**The next step is to create one.** Boot EOS, configure Et3 up, and capture `EPL_CFG_A`, `EPL_CFG_B`
and the whole per-EPL region (`0x300`-`0x3ff` of EPL14) with `fmdump` — exactly the differential
method that produced `ROUTED-PORT-ANATOMY.md`. That gives the first genuine lane-1-active reference
this project has ever had, and every value guessed above becomes checkable.

### What tonight established regardless

- **Et3's PCS was disabled** (`Port1PcsSel = 0`), which is why `SerXmit` was 0. Setting it turned the
  transmitter on — the first movement in `PORT_STATUS` in this entire investigation.
- **Et3's lane was not marked active** (`Active_1 = 0`), a second independent gate.
- Both live in per-EPL registers with per-port fields, a register class this investigation had never
  examined, and which explains how "every register matches a working lane" could be true and useless
  at the same time.

---

## ★★★ THE REFERENCE EXISTS NOW: EOS with Et3 up, captured

**2026-08-14.** Booted EOS. `show interfaces status`:

```
Et1   to-Edgecore5610-port6      connected   routed   full  10G  10GBASE-SR
Et2   to-Edgecore5610-port7-DAC  connected   routed   full  10G  10GBASE-CR
Et3                              connected   routed   full  10G  10GBASE-SR
```

**Et3 comes up under EOS.** Cabling, optics and far end are all good, and every port-3 failure in
this document is ours. Et2 links under EOS too, so its degradation is also EdgeNOS-side.

Captures are in the notes repo (`fm6000-eos-epl14-lane{0,1}-UP.txt`, `-cfg-UP`, and
`fm6000-eos-serdes-0x49-0x4a-UP.txt`) — the first ever taken with **lane 1 actually working**.

**Access:** under EOS, Et3 carries `10.99.99.1/24`, so the test system on that subnet is a fast
channel — `sshpass -p arista ssh -J lab-console … admin@10.99.99.1` with `enable` piped on stdin,
since ssh lands in the unprivileged CLI. Far quicker than the 9600-baud console.

### Both hand-set gates were right

```
EPL_CFG_A(14) = 0x7e1d7899     identical to what was set by hand   (Active_1 = 1)
EPL_CFG_B(14) = 0x00090033     identical to what was set by hand   (Port1PcsSel = 3)
```

Guessed by analogy with port 0 and confirmed correct by a working system.

### The EPL side is fully exonerated

With **both** lanes up, EPL14's two lane blocks differ at only **4 of 128 offsets** — `0x04`, `0x21`,
`0x3a`, `0x42`: status, counters, and the intended TX emphasis. And every EPL register EdgeNOS sets
on Et3 matches EOS's working values exactly:

```
SERDES_CFG 0x0aaa86c0    LANE_CFG 0x000c0002    SERDES_RX_CFG 0x002a0281
SERDES_TX_CFG 0xc0001581 SERDES_IM 0x00003fff   PCS_10GBASER_CFG 0x00000000
```

### The difference is in the SerDes, and now it is enumerated

EOS's working lane 1 against EdgeNOS's dark lane 1, all 256 SBus registers:

```
0x19 0x1a   EOS 0x0f 0x0f      EdgeNOS 0x00 0x00
0x12 0x13   EOS 0x56 0x64      EdgeNOS 0x00 0x00     (from_analog)
0x1e 0x1f   EOS 0x07 0x29      EdgeNOS 0x00 0x00     (dfe scratch / state)
0x20-0x27   EOS populated      EdgeNOS mostly 0x00   (DFE taps)
0x1d        EOS 0x01           EdgeNOS 0x08
0x28        EOS 0x1f           EdgeNOS 0x64
0x2b        EOS 0x03           EdgeNOS 0x05
0x06        EOS 0x00           EdgeNOS 0xe9
0x14        EOS 0x14           EdgeNOS 0xd4
```

⚠ The EdgeNOS column is from a dump taken **earlier in the session**, before the last rounds of DFE
work, so some of those zeros are now populated. The capture must be retaken on EdgeNOS before any of
these are treated as live differences.

Two are informative regardless. **`0x06` reads `0x00` on a working lane** — the `EnableSerDes` decode
had us setting `0x08` (`sig_strength_en`), and the replay writes `0x00`; a working lane does not carry
it. **`0x14` reads `0x14` on a working lane**, bit 6 clear, confirming signal-strength observation is
a bring-up-time thing and not a resting state.

### Next

Reboot to EdgeNOS, run the bring-up, retake the SerDes dump, and diff it against
`fm6000-eos-serdes-0x49-0x4a-UP.txt`. For the first time that diff has a **true** reference on both
sides, and whatever survives it is the answer.

### ★★ The like-for-like diff, with a control

**2026-08-14.** Rebooted to EdgeNOS, confirmed the cold boot leaves **both gates closed again**
(`EPL_CFG_A = 0x7e0d7899`, `EPL_CFG_B = 0x00090003`) — so the replay genuinely never sets them and
they must be added to the boot path. Opened both to the EOS values, ran `lanelink` + `serdes_enable`,
and dumped all 256 SBus registers of both lanes.

That gives a diff with a **control**: lane 0 works in *both* systems, so any register where the two
lane 0 columns agree is stable configuration rather than live data, and a lane-1 difference there is
real. Filtering on that:

| reg | field | EOS l0 | EOS l1 | EDG l0 | **EDG l1** |
|---|---|---|---|---|---|
| `0x19` | `sbus_analog_to_core_obs` | `0f` | `0f` | `0f` | **`02`** |
| `0x1a` | `sbus_analog_to_core_obs` | `0f` | `0f` | `0f` | **`c0`** |
| `0x1b` | — | `aa` | `aa` | `aa` | **`2a`** |
| `0x1d` | `sbus_dfe_scratch_obs` | `01` | `01` | `01` | **`09`** |
| `0x48` | — | `2a` | `2a` | `2a` | **`2b`** |
| `0x14` | `rx_ib_sig_strength_obs` | `14` | `14` | `14` | **`d4`** |

**`sbus_analog_to_core_obs` is the headline.** It reads `0x0f` on *every* working lane in both
operating systems — EOS lane 0, EOS lane 1, EdgeNOS lane 0 — and `0x02`/`0xc0` on our dark lane. That
is the analog block's output *into the core*, and it is the first register found whose working value
is identical across all four working cases and wrong only on ours.

The gate bits for that path are named in the write view of `0x17`:
`sbus_analog_to_core_lsb_gate` (b5), `sbus_analog_to_core_msb_gate` (b6), `sbus_from_core_msb_gate`
(b7) — and **our step 7 writes `0x17` with bits [4:0] set to `0x10` against a zeroed shadow, leaving
b5-b7 clear**. The replay writes `0x17 = 0x10` then `0x17 = 0x00`, which also leaves them clear, so
this is not a simple "we forgot the gates" — but it is the register that controls the exact path
whose observation is wrong, and it is where to look next.

⚠ `0x14 = 0xd4` on our lane against `0x14` everywhere else is **our own doing**: `serdes_enable`
step 10 sets `sig_strength_en`, which no working lane carries at rest. That write should be dropped —
the replay sets `0x06 = 0x00`, and EOS's working lanes agree.

### Where this leaves the investigation

Both EPL gates are now known, confirmed against a working system, and reproducible. The remaining
fault is inside the SerDes analog-to-core path, and for the first time there is a **single register
with a known-good value across four independent working cases** to aim at.

**Next:** work `0x19`/`0x1a` — find what writes `sbus_analog_to_core_obs` to `0x0f`. The `0x17` gates
are the obvious lever; the fact that neither EOS's replay nor our sequence sets them means the value
arrives some other way, and finding it is now a bounded question about one register rather than an
open-ended hunt.

### ★★ Every measurable difference is now closed — and it still does not lock

**2026-08-14.** `fm6000InitSerDesSBusConfig` — the live path's SerDes config — does a bitfield insert
on `0x17`:

```asm
xor $0x11,%eax ;  and $0x1f,%edx ;  xor     ->  bits[4:0] = 0x11
```

**It writes `0x11`, not the `0x10` our `serdes_enable` step 7 writes.** Applying `0x17 = 0x11`
followed by a DFE restart moved `sbus_analog_to_core_obs` from `0x02`/`0xc0` to **`0x0f`/`0x0f`** —
the exact value every working lane carries, stable across repeated samples.

The remaining stable differences were then closed the same way:

```
0x0c   working lanes 0x01, ours 0x00   ->  written, now 0x01
0x48   working lanes 0x2a, ours 0x2b   ->  written, now 0x2a
0x06   working lanes 0x00, ours 0xe9   ->  sig_strength_en dropped (our own doing)
```

⚠ `0x0c` and `0x48` are **undocumented** — the header defines no fields for either, and `WRITE_12`
is entirely "Reserved". They were set purely because every working lane reads those values. That is
empiricism, not understanding, and is recorded as such.

### The state now

```
EPL_CFG_A.Active_1 = 1          EPL_CFG_B.Port1PcsSel = 3       (both match EOS)
every EPL lane register         matches EOS's working lane 1
analog_to_core_obs = 0x0f/0x0f  matches all four working cases
0x0c = 0x01, 0x48 = 0x2a        match
DFE coarse/fine, PLL, signal    match
SerXmit = 1                     the transmitter runs

BlockLock = 0   RxRate = 0   PCS_10GBASER_RX_STATUS = 0
```

**Every register this project can read on Et3 now matches a lane that works, and the PCS still does
not achieve block lock.**

### What that means, honestly

The register-level approach has been taken to its conclusion. Two possibilities remain and they are
different in kind:

1. **Something not in the SerDes or EPL lane register space.** The per-EPL registers with per-port
   fields were one such blind spot and yielded two real gates; there may be others — chip-level or
   MAC-level state, or a register block never enumerated.
2. **Sequence and timing.** EOS reaches this state through an asynchronous state machine
   (`fm6000SerDesEventHandler`) driven by interrupts, with waits between transitions. We arrive at
   the same register values by a different route. Identical end state does not imply identical path,
   and a PCS that must *observe* a transition may never latch if the transition never happened.

(2) is now the more likely, and it is testable: `fm6000SetPortState` → `fm6000SetSerDesState` walks
`IDLE -> PWRDOWN -> CONFIG -> PWRUP -> WAIT_PWRUP -> WAIT_SIGDETECT`, and we have never driven those
transitions in order — every attempt has written the final values directly.

### Both hypotheses tested

**(2) Sequence and timing — no lock.** Drove the state machine's transitions in order rather than
writing end values: `PWRDOWN` (`SERDES_CFG` PowerDown=3), `CONFIG` (device reset, then
`InitSerDesSBusConfig`'s `0x06=0x00` and `0x17[4:0]=0x11`, then the enables), `PWRUP` (PowerDown=0),
`WAIT_PWRUP` (`rx_rdy=1` ✓), `WAIT_SIGDETECT`, then DFE. **`BlockLock=0`, `RxRate=0`.**

⚠ This refutes *my emulation* of the state machine, not the hypothesis. The real handler is 14 KB
and does more than the four operations we can name — DFE parameters, `ReadStatus`, locking, and
whatever the transitions themselves imply.

**(1) Outside the lane space — checked the obvious region, and it is status.** Diffed the entire
per-EPL block (`0xe3b00`-`0xe3b7f`, 128 words) between EOS-with-Et3-up and EdgeNOS. 13 differ:

```
0x300 EPL_IP                    0x30b-0x30e <unnamed>
0x303 EPL_LED_STATUS            0x313 PCS_10GBASEX_TX_STATUS
0x305-0x308 TX_FIFO_*_PTR_STATUS  0x315 PCS_40GBASER_RX_STATUS
                                0x316 EPL_1588_TIMER_STATUS
```

**Almost every one is status** — interrupt pending, LED state, FIFO pointers, timers — differing
*because* one link is up and the other is not. The configuration registers in that region
(`EPL_CFG_A`, `EPL_CFG_B`) already match. Four registers at `0x30b`-`0x30e` are unnamed in the header
and differ substantially (`0x147`/`0x69c`/`0x9a3`/`0x89f` against `0x31`/`0x3b`/`0x44e`/`0x00`);
they are the only candidates left in this region and are unidentified.

### Standing back

Two full days of register-level work have produced real, permanent results — the write path, the
device reset requirement, the SerDes SBus register map, the enable algorithm, the DFE decode, and
**two genuine configuration gates** (`Active_1` and `Port1PcsSel`) confirmed against a working
system. Et3 now transmits where it never did.

What has not been achieved is receive lock, and the honest position is that **the difference is no
longer visible in any register this project can read**. Continuing to diff registers is unlikely to
find it.

The two remaining routes are both larger pieces of work:

- **Trace EOS bringing Et3 up.** `fmPlatformTraceRegOps` captured the Et3 no-shut window before; doing
  it again now — with the knowledge of which registers matter — would show the *ordering* and the
  intermediate states that a static comparison cannot. This is the same technique that produced every
  op table in `fm6000_lanelink`, applied to the one question left.
- **Decode `fm6000SerDesEventHandler` properly** (0x364a89, 14 KB), state by state, rather than
  approximating it.
