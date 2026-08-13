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
