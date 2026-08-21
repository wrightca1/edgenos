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

## 2026-08-14 (evening): the power-cycle test, and Et2 converging on Et3's fault

A site power outage cold-started everything, which handed us the one experiment we could not
perform deliberately: Et2 had been stuck at `0x0815` across *warm* reboots, and the open question
was whether a true power-off would clear it.

**It does not.** Booted `alpha9` after the outage, ran FULLSEQ to completion:

```
Et1  EPL14 l0  0xe3800 = 0x00000cc0    up, forwarding
Et3  EPL14 l1  0xe3880 = 0x00000015    dark, as expected
Et2  EPL16 l0  0xe4000 = 0x00000815    still degraded
```

### Et2 is not the Et3 problem

The two gates that were closed on Et3 are **open on Et2**, and were never closed:

```
EPL16   EPL_CFG_A 0xe4301 = 0x7e0d7899   Active_0 = 1
        EPL_CFG_B 0xe4302 = 0x00090003   Port0PcsSel = 3
```

Byte-identical to EOS's own values for the same registers, captured the same evening with Et2 up
(`fm6000-eos-epl16-cfg-Et2-UP.txt`). Whatever is wrong with Et2 is not the EPL provisioning fault.

### The three-column diff, and one bit

With EOS captured showing **all three front ports connected**, the SerDes diff finally has a proper
control: Et1 works in both operating systems, so any register where Et1 agrees across them is stable
configuration. **231 of 256 registers qualify**, and among those Et2 differs in exactly three:

| reg | Et2 EdgeNOS | EOS | Et1 (stable) |
|---|---|---|---|
| `0x0b` | **`0x40`** | `0x00` | `0x00` |
| `0x0c` | `0x12` | `0x01` | `0x01` |
| `0x1f` | `0x25` | `0x29` | `0x29` |

`0x1f` is `sbus_dfe_scratch_obs` — dynamic, a symptom. The interesting one is `0x0b`, where the
**write** view names bit 6 `sbus_tx_elec_idle_cntl`: transmitter squelch. A single bit, set only on
the lane a write test damaged, clear on every working lane in both systems.

⚠ **This is suggestive, not proven.** `SERDES_ETH_READ_11` has *no* documented fields — only
`WRITE_11` does — and read and write views at the same register number are different silicon. The
argument is empirical (anomalous on exactly the broken lane) plus circumstantial (the write view's
bit 6 is exactly the function that would produce these symptoms). It is not a decode.

**Cleared it** — `fm6000_sbus write 0x45 0x0b 0x00`, readback `0x00`, stable. No link.

### ⚠ `0x0c` is not a usable signal, and that retracts an earlier fix

Wrote `0x01` to `0x0c` and it read back **`0x77`**. The read view is neither the write view nor
stable. That makes `0x0c` worthless as a diff target — and it **undermines the empirical
`0x0c = 0x01` applied to Et3** on 2026-08-14, which was justified purely by "every working lane
reads `0x01`". That justification is void: what a working lane reads there is not what was written.

### What the outage actually settled

Et2 links under EOS as `10GBASE-CR` **minutes before** each EdgeNOS boot, on the same DAC, with the
same far end, which has not rebooted in between. So:

- the far end transmits valid 10G — proven, because EOS locks to it
- under EdgeNOS `LANE_STATUS` reads `0x00000000`: no block lock, no rate, **and no signal detect**

**The fault is our receiver, not the link.** And that is the same symptom Et3 has. Et2 and Et3 now
fail identically, from opposite directions — Et3 never worked, Et2 worked and regressed — which
makes Et2 the better subject: its EPL provisioning is correct out of the replay, and its RX
demonstrably worked under EdgeNOS once.

### Eliminated: the replay, the far end, persistent flash state

- **The replay.** `fwd4.txt` today differs from `fwd4-preport3.txt` (the one running when Et2 last
  worked) by **exactly one line of 373,345**:
  `001082a4  00010103 -> 03ed0103`. `0x03ed` is Et3's GLORT — the SGLORT splice. Unrelated to Et2.
  ⚠ Worth knowing on its own: **the shipped `fwd4.txt` is not a clean baseline**, it carries a
  hand-edited Et3 GLORT word.
- **The far end.** `swp7` on the AS5610 (`10.1.1.238`, EdgeNOS on ppc) bounced down and up; it
  returns to `LOWER_UP` immediately and Et2 does not move. ⚠ Its instant relock means that carrier
  report is not evidence of PCS lock — the same trap as the veth on Et3. Do not use it as a signal.
- **Persistent state.** A full power cycle changes nothing, and the only flash difference is the one
  GLORT word. So Et2's state is *reproducible*, not latched — which contradicts "one write test
  damaged it permanently" and points instead at EdgeNOS's bring-up leaving Et2's RX off every time.

That last point reopens the question of why Et2 ever worked. The surviving hypothesis is **warm-boot
inheritance**: EOS left the SerDes receiver configured, our replay never re-established it, and the
first genuinely cold start exposed a gap that had always been there.

### ★★★ ROOT CAUSE: Et2 was never damaged — one spliced replay word takes it down

**2026-08-15.** Et2's failure is caused by the **Et3 SGLORT splice**, and it reverses cleanly.
Same image, same transition (EOS → EdgeNOS), same far end, one variable:

| `fwd4.txt` | Et2 `PORT_STATUS` | `LANE_STATUS` | `PCS_10GBASER_RX_STATUS` |
|---|---|---|---|
| `fwd4-preport3` — unspliced | **`0x08c0`** | `0x0940` | **1** |
| `fwd4-splicedGLORT` — one word | `0x0815` | `0x0000` | 0 |

The whole difference is line 312646 of 373,345:

```
001082a4   00010103   ->   03ed0103
```

Ran in both directions, and the reversal was run *after* the peer bounce, so the `swp7` bounce is
eliminated as the cause — Et2 broke again with `swp7` untouched.

**This retracts "⚠ Et2 regressed, and the splice is not the cause".** That conclusion rested on Et2
staying at `0x0815` across cold boots — but every one of those cold boots ran the *spliced* replay.
It was circular, and the write test on Et2's SerDes was blamed for something it did not do. Et2's
hardware has been fine throughout, which is also why it kept linking under EOS.

### The address, decoded

`0x1082a4` = `PARSER_BASE`(`0x100000`) + `0x082a4`, and `PARSER_INIT_FIELDS` starts at `0x08200`
with a stride of 4, so this is **`PARSER_INIT_FIELDS(41, 0, 0)`**.

Dumping the table and looking for real GLORTs settles the port numbering, which several documents
had guessed at:

```
port 20   03ee0102     Et2
port 40   03ef0101     Et1
port 41   03ed0103     Et3      <- the splice, in Et3's OWN slot
```

⚠ So this is **not** an index error. The splice went where it was meant to go. Writing Et3's GLORT
into Et3's parser slot takes down **Et2's receiver** — a port whose slot was not touched.

Unused ports carry `0001` in the high half and an odd low byte (`0109`, `010b`, `010d`, `010f` for
ports 36-39); the three cabled ports carry a real GLORT and low bytes `01`/`02`/`03`.

### Why this matters beyond Et2

`fm6000_tbl3init.c` records that **`PARSER_INIT_FIELDS` was tried as a generated table and backed
out**: *"970 writes. Passed its first boot 7/8 rounds clean, then a 3-boot soak gave 2 clean and 1
at 90-100% loss."* That was filed as a reliability regression — variance.

It probably was not variance. A single word of this table deterministically decides whether a port
achieves receive lock, so a generator that reorders or drops writes here would produce exactly that
pattern: mostly clean, occasionally a port down and total loss. **`PARSER_INIT_FIELDS` is
load-bearing for link bring-up, not just for parsing.**

The mechanism is still unknown, and it is genuinely surprising: `LANE_STATUS = 0` is *no signal
detect*, a physical-layer reading that a parser register has no business reaching. Candidates, none
tested: the value feeds one of our own generators, which then emits a different block; or the
register is not parser-only despite its name and address; or an unmapped GLORT on a lane that is not
enabled wedges something upstream of EPL16.

### State left on the box

`/mnt/flash/fwd4.txt` is back to the unspliced replay (`71bffafe…`), so **Et2 comes up on the next
boot** and the second forwarding port is available again. The spliced copy is kept as
`/mnt/flash/fwd4-splicedGLORT.txt` — it is the reproducer, do not delete it.

⚠ **The Et3 SGLORT work is blocked on this.** Port 41's GLORT cannot simply be re-added; doing so
costs Et2. Whatever unblocks A4 and B1 through Et2 must not carry that splice.

### The mechanism: a live-chip diff between the two states, and what it rules out

**2026-08-15.** Both states are reproducible on demand, so the technique that has worked repeatedly
here — diff a live chip against a live chip — applies directly. Dumped ten regions under the spliced
replay (Et2 dark) and again under the unspliced one (Et2 up), same image, same boot path:

```
EPL14 lane0/Et1, EPL14 cfg, EPL16 lane0/Et2, EPL16 cfg,
PARSER_INIT_STATE, PARSER_INIT_FIELDS, GLORT_CAM, GLORT_RAM,
L3AR_DGLORT/SGLORT_PROFILE                       1,626 words
```

**151 words differ, and every one of them is accounted for without a mechanism:**

- **`GLORT_CAM`, `GLORT_RAM`, `L3AR_DGLORT/SGLORT_PROFILE`: 0 differences of 512 words.** The GLORT
  written into the parser table **never propagates** to any GLORT structure. That kills the obvious
  theory — that a GLORT appearing for a lane which is not enabled corrupts a downstream lookup.
- **Every EPL difference is a status register or a counter** — `PORT_STATUS`, `LANE_STATUS`,
  `PCS_10GBASER_RX_STATUS`, `PCS_10GBASEX_TX_STATUS`, `EPL_LED_STATUS`, the four
  `TX_FIFO_*_PTR_STATUS`, `EPL_1588_TIMER_STATUS`. These differ *because* one port is up and the
  other is not. The configuration registers in both EPLs are identical.
- **The 120 remaining differences are unwritten memory.** They sit in `PARSER_INIT_STATE` indices
  **76-127** and `PARSER_INIT_FIELDS` indices **76-97** — on a 52-port switch. They read as
  random-looking values in duplicate address pairs, differing by a handful of bits between boots.
  That is boot-to-boot noise in memory nothing ever writes, not an effect of the splice.
  ⚠ Worth pursuing separately: it is another instance of the uninitialised-memory class that
  produced three real memfill defects (MOD, MAPPER, L2F).

**So the only real configuration difference between "Et2 works" and "Et2 is dark" is the spliced
word itself.** The effect is not mediated by anything in the GLORT path, the L3AR profiles, or
either EPL's configuration.

One reading worth keeping: `LANE_STATUS = 0` was described earlier as "no signal detect", but every
field in that register — block lock, rate, signal-detect sample — reads zero when the PCS is held
inactive, regardless of what is arriving on the fibre. So the symptom is equally consistent with
**the lane's PCS never being brought out of reset**, which is a configuration story rather than a
physical one, and fits a parser-table cause far better.

**Next test, one boot each:** does *any* non-default value at `PARSER_INIT_FIELDS(41,0,0)` kill Et2,
or only `0x03ed`? Try `0x0002` in the high half. That separates "the GLORT value matters" from
"writing this field at all matters", and it is the cheapest remaining discriminator.

### ⚠ RETRACTION: "reverses cleanly" was over-claimed

**2026-08-15.** A fifth boot broke the pattern. With the **unspliced** replay on flash
(`71bffafe…`), the parser word confirmed at its default `00010103`, and `FULLSEQ DONE`, **Et2 came
up dark** — `PORT_STATUS = 0x0815`, `LANE_STATUS = 0x0000`, stable across repeated samples.

The five runs, with the variable I stopped controlling for:

| # | preceding state | replay | Et2 |
|---|---|---|---|
| A | EOS → EdgeNOS | spliced | dark |
| B | EOS → EdgeNOS | unspliced | **up** |
| C | EOS → EdgeNOS | spliced | dark |
| D | EOS → EdgeNOS | unspliced | **up** |
| E | EdgeNOS ×3 partial → EdgeNOS | unspliced | **dark** |

A-D remain a clean four-run alternation inside one transition type, and that is real evidence. But
E was preceded by three **mid-FULLSEQ reboots** rather than a clean EOS boot, so it is a different
experiment — and it shows Et2 can be dark with the splice absent.

**What this means, stated properly:**

- ⛔ **Not established:** that the spliced word *causes* Et2 to go dark. The correlation holds only
  within EOS → EdgeNOS boots, and a second variable — what ran before — was uncontrolled.
- ✅ **Still true:** the A-D alternation, and the live-chip diff showing the splice reaches no GLORT
  structure. Neither depended on the causal claim.
- ✅ **Strengthened:** the warm-inheritance hypothesis. Both Et2-up boots followed EOS with Et2
  connected; the Et2-dark boot that had no splice followed EdgeNOS. That is the same pattern the
  power-cycle result first suggested, and it now has a second, independent instance.

**The commit "ROOT CAUSE -- one spliced replay word kills Et2" overstates its evidence.** The
finding it should have claimed is narrower: *within a clean EOS → EdgeNOS boot, the splice and Et2's
state alternated over four consecutive runs.*

### Doing this properly

Single observations are worthless here — this platform already fails roughly one boot in six, and
this session saw three consecutive mid-sequence resets. Any claim about Et2 needs **repetition per
arm and a stated count**, not one boot per condition:

```
arm 1   EOS -> EdgeNOS, unspliced      n boots, count Et2 up
arm 2   EOS -> EdgeNOS, spliced        n boots, count Et2 up
arm 3   EdgeNOS -> EdgeNOS, unspliced  n boots, count Et2 up
```

Arm 3 is the one that separates the splice from warm inheritance, and it is the arm that was never
run. At ~7 minutes a boot this is an hour of wall-clock, which is the actual price of the answer.

### ★★★ The answer: Et2's bring-up is NON-DETERMINISTIC, and the splice was never the cause

**2026-08-15.** A sixth boot, run as a clean EOS → EdgeNOS transition with the **unspliced** replay
(`71bffafe…`), the parser word verified at its default `00010103`, both EPL gates correct
(`EPL_CFG_A = 0x7e0d7899`, `EPL_CFG_B = 0x00090003`) — and Et2 sampled **every 30 s for six minutes**
rather than for a few seconds:

```
t=30s ... t=360s     PORT_STATUS = 0x0815     LANE_STATUS = 0x0000     (12 of 12 samples)
Et1 the whole time   PORT_STATUS = 0x0cc0
```

The full series, by condition:

| # | preceding state | replay | Et2 |
|---|---|---|---|
| A | EOS → EdgeNOS | spliced | dark |
| B | EOS → EdgeNOS | unspliced | **up** |
| C | EOS → EdgeNOS | spliced | dark |
| D | EOS → EdgeNOS | unspliced | **up** |
| E | EdgeNOS partial → EdgeNOS | unspliced | dark |
| F | EOS → EdgeNOS | unspliced | dark |

**The same condition produces both outcomes.** B, D and F are the identical experiment — same
image, same transition, same replay, same flash — and it comes up 2 times in 3. So:

- ⛔ **The splice hypothesis is refuted.** Not weakened, refuted: the unspliced replay produces a
  dark Et2 on its own.
- ⛔ **The four-run A-D alternation was coincidence.** At a base rate near 2 in 3, an alternating
  run of four has probability about (1/3)(2/3)(1/3)(2/3) ≈ **5%** — uncommon for one comparison,
  and entirely expected once you make several. I made several.
- ✅ **The real finding is bigger than the one I was chasing: Et2 link bring-up under EdgeNOS is
  intermittent.**

### What that retrospectively explains

Nearly the whole Et2 narrative in this document was single-boot observations of a coin flip:

- **"A write test damaged Et2"** (2026-08-14) — no. Et2 was never damaged. It links under EOS every
  time, and under EdgeNOS about two thirds of the time.
- **"It stayed at `0x0815` across cold boots"** — a run of tails, over a handful of boots.
- **"A power cycle does not clear it"** — same.
- **"One spliced word kills Et2, and it reverses"** — same, with an alternating pattern that looked
  like a control and was not.

**Every one of those conclusions came from one boot per condition on a port that does not come up
reliably.** The platform is documented as failing roughly 1 boot in 6 and this session watched three
consecutive mid-FULLSEQ resets; that should have set the prior. It did not.

### The method rule this earns

**On this platform, a single boot measures nothing.** Any claim of the form "X makes the port come
up" needs n boots per arm and a reported count, because the null hypothesis is not "no effect" — it
is "a two-thirds coin". Add it to the list next to *run the control before the experiment* and
*a round-trip proves self-consistency, not correctness*.

⚠ Two readings are also now suspect and should be re-taken with repetition before being relied on:
the Et2 SerDes three-column diff (`0x0b`/`0x0c`/`0x1f`) and the `sbus_tx_elec_idle_cntl` reading —
both were taken on a single dark boot, and a dark boot is now known to happen on its own.

### What is NOT affected

- **Et3 is a different, deterministic failure.** It has been dark on *every* boot ever recorded,
  under every replay, with the gates open or closed. A 0-for-many is not a coin.
- **The live-chip diff stands**: the spliced GLORT reaches no GLORT structure (0 differences of 512
  words across `GLORT_CAM`, `GLORT_RAM` and the L3AR profiles). That was a comparison of register
  contents, not of link outcomes.
- **The uninitialised parser memory at indices ≥76** stands, for the same reason.

### Next, properly

Measure the rate before theorising about the cause: **10 consecutive EOS → EdgeNOS boots, unspliced,
Et2 sampled for 6 minutes each, count the ups.** That establishes the base rate, which every future
Et2 claim has to be tested against. Only then is it worth asking what varies between a good boot and
a bad one — and the honest place to look is FULLSEQ's own ordering and timing, not the replay
contents, since the contents are now known to be identical across both outcomes.

### ★★★ THE MEASUREMENT: Et2 comes up on half of identical boots

**2026-08-15.** Ten controlled boots, one arm held constant — EOS → EdgeNOS, unspliced replay, Et2
sampled every 30 s for three minutes. `tools/et2-baserate.sh`.

```
boot  1 UP    2 UP    3 UP    4 dark   5 dark
      6 UP    7 UP    8 dark   9 dark  10 dark

RESULT: Et2 up 5 / 10 valid boots   (tainted = 0, failed = 0)
95% Wilson interval: 24% .. 76%
```

Every boot verified the same `fwd4.txt` md5 (`71bffafe…`), the same parser word (`00010103`), the
same Et1 state (`0x00000cc0`, ten for ten) and a boot-to-boot reproducible uptime of 196-197 s at
sampling. Nothing drifted. **Et2 is a coin.**

### What that retires

| claim | how it was reached | verdict |
|---|---|---|
| "a write test damaged Et2" | 1 boot | dead — Et2 was never damaged |
| "it stayed dark across cold boots" | ~3 boots | a run of tails |
| "a power cycle does not clear it" | 1 boot | same |
| "one spliced word kills Et2, and it reverses" | 4 boots, alternating | **P = 0.062, about 1 in 16** |
| yesterday's spliced boots, 0 up of 2 | 2 boots | **P = 0.25** — unremarkable, splice stays refuted |

### ⛔ And it makes A/B testing Et2 by booting infeasible

At a 50% base rate, separating two arms needs, at 80% power:

```
30-point difference   ~88 boots per arm
40-point difference   ~49 boots per arm
50-point difference   ~32 boots per arm
```

At ~12.5 minutes a boot that is **6.5 hours per arm** for a difference large enough to be obvious by
other means. Anything subtler is unreachable. **So Et2 questions cannot be answered by booting.**
The outcome has to be caught *within* a boot: `fm6000-fullseq.sh` now samples Et2's `PORT_STATUS`
and `LANE_STATUS` alongside Et1 at every settle step, so a good boot and a bad one can be diffed for
where they diverge.

⚠ The one exception, and it is the reason the SPICO test is worth running: **a hypothesis that
predicts a *zero* is cheap to test.** `P(0 up in k | 50%) = 0.5^k`, so five boots reach p = 0.03 and
seven reach p = 0.008. Predicting an extreme costs an hour; predicting a shift costs a day.

### The per-boot trace, and one reading of it retracted

**2026-08-15.** alpha10 boots with `fm6000-fullseq.sh` logging Et2 alongside Et1 at every settle
step. First dark-boot trace:

```
STEP5 (replay done)  PORT_STATUS=000008c0 pcsRx=1        <- Et1, ALREADY LINKED
t=3s .. t=21s        et2 PORT_STATUS=00000815  pcsRx=0  LANE_STATUS=0
t=24s                et2 PORT_STATUS=00000c15  pcsRx=0  LANE_STATUS=0
final                et1=000008c0/00000940   et2=00000815/00000000
```

⛔ **Retracted on sight:** I first read the `0x0815 -> 0x0c15` change at t=24s as a lock beginning
and failing. It is bit 10, `Receiving`, and **Et1 toggles the same bit in the same log** — `0x0cc0`
at every settle sample, `0x08c0` at the end. It is an activity flicker present on a working port,
so it says nothing about Et2's failure. Caught by reading the control column in the same trace,
which is the only reason it did not become another hypothesis.

★ **What the trace does establish: Et1 is already linked at STEP5, before the settle loop starts.**
It does not come up during STEP7; it arrives there up. That reframes the Et2 question into two
alternatives that want completely different fixes:

| observation on a GOOD Et2 boot | meaning |
|---|---|
| Et2 already up at STEP5 | the divergence happens **inside the replay**. The settle loop merely watches an outcome decided minutes earlier — which would explain why every register diff taken *after* settling has looked identical, including the 231-register three-column diff |
| Et2 climbs during STEP7 | a **training-time race** |

⚠ The STEP7-only trace cannot distinguish them, because **STEP5 logged Et1's register only.** Fixed
in the build tree: STEP5 now logs Et2 too. One line, and it splits the question cleanly.

## ★★★ 2026-08-15: the divergence is INSIDE the replay, not in the settle loop

alpha10's per-boot trace, good boot against dark boot, bit 10 masked (it is noise — see the
retraction above). **The only differences in the entire trace are Et2's own lines:**

```
dark   t=3s .. t=24s   et2 PORT_STATUS=00000815  pcsRx=0  LANE_STATUS=00000000
good   t=3s .. t=24s   et2 PORT_STATUS=000008c0  pcsRx=1  LANE_STATUS=00000940
```

**Et2 is already fully locked at t=3s, the very first settle sample, and never changes.** It does
not climb during STEP7 — it arrives there up, exactly as Et1 does. On a dark boot it is `0x0815`
from the first sample and stays there.

Everything else in both traces is byte-identical: same `rc=0`, same `PIN=00000208`, same `sched`,
same memfill results, same Et1 behaviour. The replay ran the same way; only Et2's outcome differs.

### Why this matters more than the coin itself

**The outcome is decided inside STEP5 — the 299,803-write replay — before any settle sampling
begins.** Two consequences:

1. **Every register diff this project has taken on Et2 was taken after the fact.** The 231-register
   three-column SerDes diff, the EPL comparisons, the `0x0b`/`0x0c`/`0x1f` analysis — all sampled a
   chip whose fate was sealed minutes earlier. That is a sufficient explanation for why they kept
   coming back clean, and it retires "the difference is not visible in any register" as a
   conclusion: we were looking in the right place at the wrong time.
2. **It is a race inside the replay**, not a training-time race in the settle loop. Ordering and
   timing among 300k writes, not SerDes convergence.

⚠ The earliest sample is t=3s *after* STEP6, so this excludes the settle loop but does not pin the
moment inside the replay. `fm6000-fullseq.sh` now logs Et2 at STEP5 as well; the next image narrows
it further.

## ⛔ Transit traffic is blocked on portd's RX demux, not on forwarding

With Et2 up, both netdevs came up correctly for the first time (`et1 10.101.101.26/29`,
`et2 10.101.101.34/29`, both `carrier=1`, fibd programmed 14 routes). The transit test still
returned 100% loss, and the reason is **not** the dataplane:

```
peer   10.101.101.34 lladdr 44:4c:a8:31:5d:ab STALE    peer LEARNED our MAC
peer   swp7 rx 1026 packets                            traffic really arrives
switch 10.101.101.33 FAILED (6 probes)                 we never see the replies
switch et1 rx=51 tx=32     et2 rx=0 tx=13
```

**TX works on both ports** — Et2 transmitted 13 frames and the peer learned our MAC from them.
**RX only ever reaches et1**, which is precisely the limitation `fm6000_portd.c` documents in its
own header: injection stamps each port's egress GLORT in tag w1, but demuxing a *punted* frame back
to a TAP needs the source GLORT out of the frame's tag, and **which halfword carries it is
unconfirmed**. `PORTD_SRCWORD` defaults to 2; that default is either wrong or the demux is inert.

**So A4 and B1 are not blocked on topology any more, and not on Et2 either — they are blocked on
one unconfirmed field.** The experiment is a single boot: bring up with `PORTD_DEBUG=1`, which
prints the tag of every punted frame, and compare the tags of frames arriving from Et1 against those
from Et2. The halfword that differs is the answer.

⚠ portd cannot be restarted without a chip reset, so this costs a boot — and Et2 is up on only half
of them. Budget two.

### ⛔ CORRECTION: the demux blocker is not `src_word`, it is that punted frames carry NO TAG

I read `PORT3-BRINGUP.md`'s own decoded punt format — "tag word 1 is the GLORT: source on RX" —
saw that `fm6000_portd.c` defaults `src_word = 2`, and concluded the demux was reading the wrong
halfword. **The live capture says the problem is one level down.**

`PORTD_DEBUG=1` on a boot with both ports up:

```
portd: punt demux on tag w2 (PORTD_SRCWORD); unmatched -> et1
  TAP<-ASIC (no tag) len=90:
    33 33 00 00 00 05 80 a2 35 81 ca b4 86 dd 6c 0c
    DMAC                SMAC                ^^^^^ ethertype at offset 12
```

**There is no F64 tag.** DMAC, SMAC, ethertype — a complete, clean Ethernet frame. `on_punt` scans
for an ethertype at `+12` before testing the tagged layout at `+20`, matches at `off=0`, and takes
the untagged path, which hard-codes `ports[0]`. `port_for_tag()` is never called, which is why the
run produced **zero** `punt tag` lines. No value of `src_word` can matter when there is nothing to
read.

⚠ **And the same frame type WAS tagged in the earlier capture.** The `rxdump` layout recorded above
has identical DMAC `33 33 00 00 00 05` and SMAC `80 a2 35 81 ca b4` — the same OSPF/IPv6 multicast
frame — with `07 01 03 ef 00 01 ff ff` spliced in. So tagging is **configuration-dependent and is
currently off**, not a property of the frame.

**The real question for A4/B1 is therefore: what makes the ASIC prepend F64 on punt, and why is it
not set now?** Candidates, none tested: a per-port or CPU-port attribute selecting ISL/F64 on
redirect; the `MOD_*_TAG` tables (`MOD_TX_PORT_TAG`, `MOD_DST_PORT_TAG`); or the special-delivery
parser rule this project already added for the F64 frame type. That last one is suggestive — the
frame type was decoded and wired into `parser_program.py`, and the current image runs *our* parser.

`src_word` is nonetheless changed from 2 to 1 in `fm6000_portd.c`, because word 1 is what the
capture shows and word 2 was an argument from the TX layout about the RX layout. **It is inert until
frames are tagged**, and must not be mistaken for the fix.

### The tag control IS set — so the untagged punt is not a missing configuration

`MOD_TX_PORT_TAG` is 76 entries, one per port, with a 2-bit `Tag`. EOS sets it to **2 on exactly two
ports — port 0 (the CPU) and port 1** — and 0 on all 74 others. That is the obvious "tag frames on
egress to this port" control, and port 0 being the CPU matches the independent port-numbering
evidence.

**It is set correctly on the live chip:**

```
0015f280 = 00000002    MOD_TX_PORT_TAG[0]   CPU
0015f281 = 00000002    MOD_TX_PORT_TAG[1]
0015f294 = 00000000    MOD_TX_PORT_TAG[20]  Et2
0015f2a8 = 00000000    MOD_TX_PORT_TAG[40]  Et1
0015f300 = 00000002    MOD_DST_PORT_TAG[0]
```

So the untagged punt is **not** a missing register write, and that eliminates the most obvious
cause. Why frames still arrive without an F64 tag is open.

### What is now precisely characterised

```
peer -> switch et2 address 10.101.101.34 : 3 pings, 100% loss
switch et1 rx  27 -> 212                   frames DO arrive, in volume
switch et2 rx  0                           and none are attributed to et2
portd log      "TAP<-ASIC (no tag)"        no F64 tag on any of them
```

The failure chain, end to end:

1. Frames for Et2 reach the CPU punt ring — `et1 rx` climbs when Et2 traffic is generated.
2. They carry no F64 tag, so `on_punt` takes its untagged path.
3. That path hard-codes `ports[0]`, so every frame is delivered to **et1's** TAP.
4. Linux discards Et2's ARP replies as arriving on the wrong interface, so the neighbour for
   `10.101.101.33` stays `FAILED`, so the switch cannot even send a reply.
5. **et2 is receive-dead at the netdev layer**, and every bidirectional protocol on it fails.

⚠ Nothing here is a dataplane fault. Et2 links, transmits, and its frames reach the CPU. The break
is entirely in per-port attribution on the punt path.

**The open question, stated tightly:** the same frame type (OSPF/IPv6 multicast, DMAC
`33 33 00 00 00 05`, SMAC `80 a2 35 81 ca b4`) was captured **with** an F64 tag by `fm6000_rxdump`
earlier in this project and **without** one now, while `MOD_TX_PORT_TAG[0]` reads 2 in both cases.
Something other than that register decides it. Candidates not yet tested: tagging may apply only to
frames redirected by an explicit CPU action (`L2AR_ACTION_DMT` `CmdA=3`) and not to multicast that
reaches the CPU by flooding; or our generated parser may not set a flag EOS's did.

**The cheap discriminator** is to capture with `fm6000_rxdump` on this same boot and see whether the
tag is present there — if rxdump sees tags and portd does not, the difference is in portd, not the
ASIC. It costs a boot, because rxdump and portd contend for the same punt ring.

### ✅ THE CORRECTION WAS WRONG. The tag IS present, and `src_word = 1` IS the fix

`fm6000_rxdump`, alpha10, portd not running, `MOD_TX_PORT_TAG[0] = 2`:

```
[0] len=90  33 33 00 00 00 05|80 a2 35 81 ca b4|07 01|03 ef 00 01 ff ff|86 dd ...
[2] len=90  01 00 5e 00 00 05|80 a2 35 81 ca b4|07 01|03 ef 00 01 ff ff|08 00 ...
[3] len=82  33 33 00 00 00 05|80 a2 35 81 ca b4|07 01|03 ef 00 01 ff ff|86 dd ...
[5] len=71  01 00 5e 00 00 05|80 a2 35 81 ca b4|07 01|03 ef 00 01 ff ff|08 00 ...
                                                 w0    w1    w2    w3
4 of 7 captured frames carry the tag. w1 = 0x03ef = Et1's SGLORT.
```

**The ASIC tags punted frames exactly as `PORT3-BRINGUP.md` recorded months ago.** So the chain is:

```
tag word 2 = 0x0001        portd reads w2 because src_word defaulted to 2
no port has glort 0x0001   port_for_tag matches nothing
-> return 0                every frame goes to et1's TAP
```

That is precisely the observed `et1 rx=212, et2 rx=0`, and `src_word = 1` fixes it.

### ⛔ How I got it wrong, because the mechanism matters

I ran `PORTD_DEBUG=1` and saw a single dumped frame reading `DMAC | SMAC | 86 dd` with no tag, plus
zero `punt tag` lines, and concluded the ASIC had stopped tagging. Both observations were real and
both were misleading:

- **`PORTD_DEBUG=N` dumps N frames, and I passed 1.** The one frame it printed was one of the
  **short malformed ring entries** (`len=12`, `19`, `20` also appear in the rxdump capture) — not a
  representative frame. I generalised from n=1 on the same day I wrote that a single boot measures
  nothing.
- **Zero `punt tag` lines is what a *wrong* `src_word` looks like too.** `port_for_tag` is only
  called on the tagged path; the untagged frame I happened to dump never reached it. Absence of the
  debug line was read as absence of tags.

The original reasoning — that word 1 holds the source glort, from this repo's own decoded punt
format — was right, and I abandoned it on weaker evidence than it was based on. **`src_word = 1`
stands, and it is not inert.**

⚠ Also visible and unexplained: **3 of 7 ring entries are short and malformed** (12, 19, 20 bytes,
with plausible-looking Ethernet fragments). They are not frames. Worth a look on their own — portd
counts them via `n_rx_drop`, so they are being silently discarded today.

## ★★★ ANSWERED: Et2's outcome is decided INSIDE the replay, not in the settle loop

**2026-08-16.** alpha11/12 log Et2 at STEP5 — the moment the 299,803-write replay finishes, before
any settling. Both arms now captured:

```
GOOD boot   STEP5   et2 PORT_STATUS=00000cc0  pcsRx=1  LANE_STATUS=00000940
DARK boot   STEP5   et2 PORT_STATUS=00000815  pcsRx=0  LANE_STATUS=00000000
```

**Et2 is already fully locked when the replay completes, or already dark.** It does not climb during
STEP7; the settle loop only ever observes a result that was fixed minutes earlier. Et1 behaves the
same way — up at STEP5 on every boot — so this is how the chip works, not an Et2 peculiarity.

### What this retires

**"The difference is no longer visible in any register."** That conclusion, and the two days behind
it, rested on diffs taken *after* settling: the 231-register three-column SerDes comparison, the EPL
lane blocks, `0x0b`/`0x0c`/`0x1f`, `analog_to_core_obs`. Every one of them sampled a chip whose fate
was already sealed. They were not looking in the wrong place — they were looking at the right place
at the wrong time, which is why they kept coming back clean and why closing every difference changed
nothing.

### What it means

**Et2's ~50% failure is a race inside the replay** — ordering or timing among 300k register writes —
not SerDes training convergence. That redirects the whole investigation:

- ⛔ **Stop diffing post-boot register state.** It cannot see this.
- ✅ **Instrument the replay itself.** `fm6000-fullseq.sh` already reports per-block progress; the
  question is which block leaves Et2 locked or dark. Sampling `0xe4000` between blocks would bisect
  300k writes down to one block in a handful of boots — the same bisection that found the SPICO
  answer, applied inside STEP5.
- The `PACE` knob (`init-m1` passes `PACE=1500000`) is the obvious first variable, since a timing
  race is exactly what pacing changes. `ET2-COPPER-LINK.md` already records a suspicion that copper
  "seems to need it; evidence is still thin" — that thin evidence now has a mechanism to attach to.

⚠ This does not identify the block or the mechanism. It says where to look, and rules out the place
this project has been looking for two days.

## ★★★ Et2's link is established INSIDE the replay, and the successful path is deterministic

**2026-08-16.** alpha13 samples Et2 every 16k ops inside `fm6000_fullreplay`. Three independent GOOD
boots produced **byte-identical** traces:

```
114688 ops (mmio=22432  sbus=30752) et2=0x00000015/00000000   unprovisioned
131072 ops (mmio=38816  sbus=30752) et2=0x00000015/00000000
147456 ops (mmio=55200  sbus=30752) et2=0x00000015/00000000
163840 ops (mmio=71584  sbus=30752) et2=0x00000815/00000000   SerXmit on, no lock
180224 ops (mmio=87743  sbus=30827) et2=0x00000cc0/00000940   LINKED
196608 ops (mmio=104127 sbus=30827) et2=0x000008c0/00000940
212992 ops (mmio=120511 sbus=30827) et2=0x000008c0/00000940
```

**Et2 links between op 163,840 and 180,224**, and the SBus counter moves `30752 → 30827` across
exactly that window — **75 SBus transactions** bring the lane from transmitting to locked.

★ **Three good boots are identical down to the SBus tally.** The successful path is not merely
repeatable, it is deterministic. So whatever differs on a dark boot, **it is not the instruction
stream** — the same writes are issued in the same order.

### Why only 7 samples, not ~17

The print sits after the SBus `continue`:

```c
if(a==0xF001u){ ... sbus(v,pend); continue; }   /* SBus ops never reach the print */
wr(a,v); mmio++;
if((n & 0x3fff)==0){ ... print ... }
```

**A sample only fires when the 16,384th op happens to be an MMIO write.** Early in the replay SBus
dominates — the SPICO upload alone is ~30k transactions — so most multiples land on SBus lines and
are skipped. At op 114,688 only 22,432 ops were MMIO, which confirms it. ⚠ To sample the first
~110k ops the print must be moved above the SBus `continue`.

### What this leaves

Two candidates for the ~50% failure, and they want different fixes:

1. **The hardware responds differently to identical writes** — the SerDes sometimes fails to train on
   the same stimulus. A physical-layer convergence problem; `PACE` and DFE are the levers.
2. **The divergence is before op 114,688**, in the unsampled region. Not excluded by these traces.

⚠ **And the instrument may be perturbing the result.** alpha13 adds an MMIO read per 16k ops — a
delay in the very sequence whose timing is suspect. Et2 has come up on **3 of 3** boots with alpha13
against a measured **5 of 10** without it. That is p ≈ 0.125 and proves nothing yet, but it is the
direction the timing hypothesis predicts, and if the dark arm stays absent through more boots the
null result is itself the finding: *inserting delay into the replay improves Et2's link rate.*

### ⚠ The observer effect, and the same statistical error made twice

alpha13 adds one MMIO read per 16k ops inside the replay. Et2 has since linked on **4 of 4** boots,
against a measured **5 of 10** without it.

⛔ **I first reported this as heading for significance at 6/6 (p = 0.016). That is the wrong test**,
and it is the identical mistake made against SPICO earlier the same day: `0.5^k` treats the baseline
as a *known* 50%, when it was estimated from 10 boots. Comparing two samples needs Fisher's exact:

```
4/4 vs 5/10   p = 0.126
5/5 vs 5/10   p = 0.084
6/6 vs 5/10   p = 0.058     still not significant
```

**6/6 would not clear 0.05.** Roughly 8/8 is needed, and the run is capped at 6. The binomial figure
is seductive precisely because it is the easy one to compute; the lesson was written into
`SPICO-RE.md` hours earlier and then repeated anyway.

### The methodological bind this creates

If the sampling really does perturb the outcome, then **the instrument suppresses the very event it
was built to capture** — the dark arm may never appear because the extra delay prevents it. The
traces would then describe a modified system, and "4 identical good traces" would be a property of
alpha13 rather than of the replay.

**The way out is to test the delay deliberately instead of reading it off an artifact:** vary `PACE`
with the in-replay sampling removed, n boots per arm, and measure. `PACE` is already a documented
knob (`init-m1` passes `PACE=1500000`) and `ET2-COPPER-LINK.md` already records a thin suspicion
that copper needs it. That is a designed experiment with a stated count, which is what this question
has needed from the start.

⚠ What survives regardless: **the four good traces are byte-identical**, including the SBus tally.
Whatever differs on a dark boot, it is not the instruction stream — and that conclusion does not
depend on the sampling being neutral, because it is a comparison *among* sampled boots.

## ★★★ Et2's lane is NOT marginal — eye score measured under EOS

**2026-08-16.** Using the EOS diagnostics found the same day (`show interfaces <if> phy detail`,
`CliPlugin/FocalPointV2PhyCli.py`), with all three ports linked:

```
            eyeScore  dfeMode  dfeCrse  dfeFine   dfeParms   eFifoErr
Et1 (SR)     0x2a40    0002     0002     0002    3f0a025f      0      always links
Et2 (CR)     0x3212    0002     0002     0002    2315176f      0      links ~50% under EdgeNOS
Et3 (SR)     0x3340    0002     0002     0001    3f07007f      0
```

**Et2's eye score is higher than Et1's**, the port that has linked on every boot ever recorded. It
sits mid-range across the three, DFE converged to the same coarse/fine state (2/2), and the elastic
FIFO error count is zero on all three.

### What this rules out

⛔ **The lane is not physically marginal.** The leading physical explanation for a ~50% link rate —
a weak or borderline signal that sometimes fails to train — is refuted by direct measurement. Et2 has
more eye margin than a port that never fails.

⛔ **DFE non-convergence is not the cause either.** `dfeMode`/`dfeCrse`/`dfeFine` are `0002/0002/0002`
on Et2, identical to Et1 and effectively identical to Et3.

### What it leaves

**The intermittency is in our bring-up sequence, not the silicon or the cable.** Combined with the
in-replay traces — four byte-identical good boots, Et2 linking between op 163,840 and 180,224 across
75 SBus transactions — the picture is:

> the hardware is capable of linking reliably; EOS achieves that; our replay achieves it about half
> the time, with an instruction stream that is byte-identical between successes.

⚠ **On eyeScore semantics.** The units and direction are undocumented in our notes; the argument here
does not depend on them, because it is a *comparison against two known-good ports on the same chip
and the same boot*. Et2 is not an outlier in either direction. If a future reading treats eyeScore
as an absolute threshold, that assumption needs establishing first.

⚠ Also note this is EOS's bring-up, not ours. It says the lane *can* achieve this margin, not that it
does under EdgeNOS. **The same measurement under EdgeNOS is the obvious next step** — but it needs
the SDK, which we deliberately do not link, so it would mean reading the same SerDes registers
directly (`SerdesStat 0x00100f0f`, `SerdesSigDet 0x000001fe` and the rest are all in the dump above).

---

## ★★★ 2026-08-17: the replay does NOT tear Et2 down. The replay-race hypothesis is dead

The alpha16 hunt captured its pair: one GOOD boot and one DARK boot, sampled every 1,024 ops through
the interesting region, with the *executed* replay preserved as `/mnt/flash/fwd-executed.txt`
(220,972 ops). Both boots execute a byte-identical write stream.

| | SerXmit on | lock acquired | outcome |
|---|---|---|---|
| GOOD | 163,840 | **175,104** (+11,264) | `LANE_STATUS=0x940` held to the end |
| DARK | 163,840 | **190,464** (+26,624) | lost ~5,120 ops later |

The transmitter enables at the *same* op on both boots. What varies is how long training takes to
converge — and the slow convergence is the one that fails.

### The teardown window contains no teardown

`resolve-teardown.py` narrowed the loss to ops 194,560 → 195,584 and resolved that window against the
executed file. All 1,024 writes are MMIO, all in one region, `0x034xxx` — inside `L2L_BASE 0x30000`.
Address stride 1, value incrementing by one every other word (`0xcb,0, 0xcc,0, … 0x2ca,0`): a bulk
fill of 512 sequential L2L entries. No port register, no SBus, no configuration write of any kind.

And the control settles it: **the GOOD boot executed those identical writes with `LANE_STATUS=0x940`
throughout.** They are exonerated.

### The stronger result: the replay stops touching the port a third of the way from the end

> **The last write to any EPL — any port — in the entire replay is op 157,123.**
> After it, 63,849 ops (29% of the replay) execute and not one touches a port.

So there is no write to find. The port is configured once and abandoned; everything after is
autonomous SerDes link training running in hardware, which converges fast on some boots and
slowly-then-not-at-all on others. The replay's remaining 29% is just wall-clock time during which
that coin lands.

This retires the whole line of investigation, and retro-explains three dead ends:

- **the splice "root cause"** — splicing a word cannot matter to a port nobody writes;
- **PACE** — pacing only changes how much time the untouched training gets;
- **the A/B/A/B alternation** — noise, as the 10-boot run already indicated.

⚠ This does **not** contradict the earlier finding that the outcome is fixed *inside* the replay. It
is: the replay's duration is when training happens. What is now excluded is that a *write* decides it.

### The tail of Et2's EPL writes is a recorded polling loop

The last ~1,236 writes to Et2's block are one 3-write cycle repeated:

```
LINK_IP (+0x04) <- 0x07ffffff     write-1-to-clear every pending link interrupt
LINK_IM (+0x02) <- 0x07fffffe     unmask bit 0
LINK_IM (+0x02) <- 0x07ffffff     re-mask bit 0
```

~412 iterations on Et2 against ~153 on Et1. That is EOS servicing link-state interrupts while it
waited — an **adaptive** loop, of which `fwd4.txt` is a recording of one particular run. We replay a
fixed iteration count: the number EOS happened to need on capture day. Et2 needs more than Et1
because it is the copper/DAC port and trains slower.

⚠ Do not over-read this. Clearing an interrupt-pending bit does not drive a training state machine;
the loop length is a *symptom* of how long training took, not a cause. Replaying more iterations of a
mask toggle is not obviously a fix. It is recorded because it names `LINK_IM`/`LINK_IP` and shows the
sequence contains captured waits, which matters for any future generator.

## ★★★ Et2 can be brought to full lock after boot, with no reboot

`fm6000_lanelink 2` (port 2 = alta 20 = EPL16 lane 0 = Et2, SBus dev 0x45, an *observed* mapping) run
against a live dark Et2:

- 10 trials, **2 reached `LANE_STATUS=0x940`** — the good-boot value — measured at +5s.
- Lock never appears immediately; it arrives seconds after the op stream ends. Training is asynchronous.
- It was the **DFE (RX adaptation)** step that did it. After the 168 link ops `PORT_STATUS` was still
  `0x815`; only after the 94 DFE ops did it move.
- **Re-running `lanelink` on a locked lane tears the lock down** (trials 8 and 10 started at `0x9D5`
  and ended dark). Any retry loop must check first and never touch a live link.

This changes the economics of the whole investigation: a trial used to cost a reboot and a 50% coin,
and now costs seconds. It is also the first time Et2 has been brought up under EdgeNOS at all.

⚠ The locks obtained this way are **marginal**: `PORT_STATUS=0x9D5` has bit 8 `HiBer` set, where a
good boot reaches a clean `0x8c0`. They also flap — one retry run broke out of its loop on a lock
that was gone by the very next read. `lanelink`'s DFE finds a worse solution than the boot path.

### ⛔ CORRECTION: that is not a lock rate. The lane oscillates, and single samples measured the flap

Sampling Et2 continuously after driving it up shows `LANE_STATUS` **alternating** between `0x940` and
`0x0000` indefinitely — 7 of 12 samples locked over two minutes at 10s spacing, then 13 of 22 at 4s
spacing. `pcsRx` sits at `0x3FF` (all ones) against Et1's clean `0x1`. Simultaneous control on the
same chip, same sweep:

```
et1 locked 22/22        et2 locked 13/22
```

Et1 is rock solid while Et2 oscillates at roughly 60% duty cycle. So:

> **`fm6000_lanelink` does not bring Et2 up. It produces a lane that continuously re-trains and loses
> lock.** A single delayed read cannot tell "this attempt succeeded" from "this read landed in an up
> phase", which is exactly what every trial in this section did.

This retracts the framing of the numbers above, and with them the two comparisons built on it:

| reported | what it actually was |
|---|---|
| "2/10 attempts reached lock" | 10 single samples of a flapping lane — a duty-cycle estimate, not a success rate |
| "polarity corrected: 0/10 vs 2/10, p = 0.47" | two sets of single samples of a flapping lane. The test never had the power its framing implied, and the underlying quantity was not what was being compared |
| "peer-bounce: 0/5" | same defect, fewer samples |

The finding that survives is narrower and still worth having: **the DFE step visibly moves a dark Et2**,
and the lane can reach `0x940` post-boot without a reboot. What it cannot yet do is *hold* it.

**The right instrument for anything in this area is a duty cycle over a stated window with a
simultaneous Et1 control, not a single read.**

### ✅ ANSWERED: a good boot's Et2 is rock solid, and the boot outcome really is binary

Measured on freshly booted hardware, duty cycle with Et1 sampled in the same sweep as the control:

| boot | `PORT_STATUS` | Et2 duty | Et1 control |
|---|---|---|---|
| dark boot A | `0x0815` | **0 / 20** over 100s | 20/20 |
| dark boot B | `0x0815` | **0 / 20** over 100s | 20/20 |
| **good boot** | `0x08C0` | **20 / 20** over 100s, then **60 / 60** over 300s | 20/20, 60/60 |

The good boot's Et2 was still locked ten minutes in, `pcsRx=1`, **`HiBer` clear**. So:

> **A good boot's Et2 is indistinguishable from Et1 — a clean, stable link.** A dark boot's Et2 is
> genuinely, completely dark: zero locked samples, not a flap that a single read happened to miss.

⚠ **This partly walks back the correction above.** The flapping is real, but it is an artifact of
`fm6000_lanelink` driving the lane — it was measured on a box that had been up 8 hours and had been
driven repeatedly. It is **not** what a fresh boot does, in either outcome. Single-sample
classification is unsound for a *driven* lane and sound for a *fresh boot*, and the historical
"5 of 10 boots" figure is not impeached by it.

That leaves three distinct states, and the third is ours, not the hardware's:

| state | `PORT_STATUS` | duty | `HiBer` |
|---|---|---|---|
| good boot | `0x08C0` | 60/60 | clear |
| dark boot | `0x0815` | 0/20 | — |
| `lanelink`-driven | `0x09D5` | 13/22 | **set** |

**`fm6000_lanelink` does not reproduce a good boot.** It produces a qualitatively different, worse
link. Whatever the boot path does to get a clean lock, the tool's captured sequence does not do it —
so it is a recovery hack, not a model of bring-up, and it should not be used to infer how bring-up
works.

### The board's polarity map, and a real bug in our own tool

`SERDES_TX_CFG` (+0x3a) bit 30 is `TxPolarityInvEn`, which compensates a differential pair routed
P/N-swapped on the PCB. Final values in the replay:

| port | `TX_CFG` w0 | `TxPolarityInvEn` |
|---|---|---|
| **Et1** (works) | `0xc0000581` | **1** |
| **Et2** (coin) | `0x80001581` | **0** |

Two independent sources agree: these exact values appear in **EOS's own `fwd4.txt`** (so our generator
is faithful — byte-identical on these registers), and EOS's **`CotatiP4.fdl altaSfpPorts`**, already
transcribed into `fm6000_serdes_ports.h`, records `txpol=1` for port 1 and `txpol=0` for port 2.
Et1's pair is swapped on the board; Et2's is not. RX polarity (`SERDES_RX_CFG` +0x39 bit 25) is 0 on
every port in both files — nothing to fix there.

**`fm6000_lanelink` ignored this.** `patch_tx_cfg()` masked only `TxOutputEqPre`/`Post`, so bit 30
passed through from the Et1-recorded template and every driven port got **Et1's inversion**. Driving
Et2 inverted a lane that must not be. Under 64b/66b that still locks — inverting the sync header turns
`01` into `10`, also legal — while the descrambler sees garbage. That is a HiBer lock, and HiBer is
exactly what driven Et2 reported. Fixed: `patch_tx_cfg()` now sets bit 30 from `p->txpol`.

⚠ **The fix did not measurably help.** With bit 30 corrected: **0/10**, against the 2/10 baseline —
Fisher p = 0.47, inconclusive in either direction at that power. It is corrected because it is
demonstrably wrong versus EOS, *not* because it is the cure. Reporting it as the fix would be the
same error as the splice.

### Two underpowered tests, recorded so they are not mistaken for results

- **Peer-bounce coordination** (down/up the far-end swp7 around our retrain, on the theory that
  10GBASE-CR training is a mutual handshake and the peer was not retraining): **0/5**. Against the
  2/10 baseline, Fisher p = 0.52. At a true 20% rate, 0/5 happens by chance a third of the time.
  This test had almost no power and should not be cited either way. ~30 per arm would be needed.
- The peer was verified `LOWER_UP`, 10G, link-detected afterwards, so the run of failures is not
  explained by a downed peer.

⚠ Note the peer reports `Link detected: yes` while our `LANE_STATUS` reads 0. Our transmitter
(`SerXmit`) stays on even when dark, so the far end locks onto us regardless. **Far-end link state is
not evidence about our side** — the link is asymmetric, and that is the whole phenomenon.

### New instrument: `tools/fm6000-status.sh`

Reads `PORT_STATUS` / `LANE_STATUS` / `pcsRx` / `TX_CFG` / `RX_CFG` / `LINK_IM` / `LINK_IP` live off
the chip via `devmem`, any time, without a reboot. Register space is indexed in 32-bit **words**
(`rd()` does `M[w]` on a `uint32_t*`), so byte address = BAR0 + word*4; BAR0 comes from sysfs rather
than hardcoded. Every previous link reading came from `/mnt/flash/fullseq.log`, which is written once
per boot and truncated by the next — that is how two dark traces were lost.

First run recorded Et1 `LINK_IP=0x00001B87`: link interrupts have been accumulating **unserviced**,
because EdgeNOS never runs the service loop the replay recorded.

## 2026-08-20: the EPL/SerDes side is now provably correct, and the lane is still dark

Re-tested on alpha32, with `fm6000_lanelink` (which postdates everything above).

**`fm6000_lanelink 3` works.** 963 ops, and it moves lane 1's SerDes config to exactly the
values a working EOS lane 1 holds — including the per-lane `SERDES_TX_CFG` value that this
document notes cannot be copied from lane 0:

| offset | before | after `lanelink 3` | lane 0 (et1, up) | EOS live lane 1 |
|---|---|---|---|---|
| `0x39` | `00280280` | **`002a0281`** | `002a0281` | `002a0281` |
| `0x3a` | `80000080` | **`c0001581`** | `c0000581` | `c0001581` |
| `0x3b` | `00000803` | **`00000c83`** | `00000c83` | `00000c83` |
| `0x3c` | `000001ee` | **`000001fe`** | `000001fe` | `000001fe` |

et1 and et2 are unaffected by the run — no collateral damage.

**And the lane stays dark**: `PORT_STATUS=0x00000015`, `LANE_STATUS=0`, `pcsRx=0`.

What is now ruled out on our side:

- **SerDes is alive.** SBus reads to the lane's device (`0x4a`) all return `result=4`. Lane 0
  is `0x49`. Registers `0x0f` and `0x14` read identically on both lanes; `0x20`/`0x21` differ,
  but those are state and we have no bit definitions for them, so nothing is concluded there.
- **Laser and module state are normal.** SCD `0x5030` reads `0x00000180` — *identical to ports
  1 and 2, both of which are linked*. Bit 6 clear = laser enabled. (Port 4 reads `0x0187`.)
- **`+0x04` is not a missing config write.** It reads `0` on lane 1 and `0000bb87` on lane 0,
  which looks like a gap and is not: `0x04` is `LINK_IP`, *interrupt pending*. Lane 0 has
  pending interrupts because traffic is flowing. Same trap this document already records for
  `SERDES_IP`.

⚠ **Do not run `fm6000_i2c_bringup` to inspect the transceiver.** Despite the name it is the
pre-enumeration sequence — COLD-BIST memory init, PCIe SerDes bring-up, PCI rescan — not an
EEPROM reader. On a running system it is disruptive.

### The remaining variable is the far end, and it is not measurable from here

Et3 is cabled to `eth1` of the test system at `10.22.1.56`. That host is a **container**: both
its `eth0` and `eth1` are veth pairs (`eth1@if15`, `ethtool` driver `veth`), so `eth1`'s
`LOWER_UP` says nothing about the physical NIC — the real interface is the veth peer on the
container host, which is not reachable from the container.

Corroborating the recable: the AS5610's `swp5` still carries `10.99.99.1/24`, the same subnet
as the test host's `eth1` (`10.99.99.2/24`), and reads **`NO-CARRIER, state DOWN`** with an
`INCOMPLETE` ARP entry for `.2`. So that segment moved off swp5.

**Open question for the lab, not for the code:** is the container host's physical NIC up, and
what media/rate is it? Our lane is configured for 10GBASE-R (a lock is `LANE_STATUS=0x940`).
A 1G far end would never lock and would look exactly like this.

### ⚠ Correction, same day: the far end is NOT the problem

The section above ended by asking whether the far end was live. It is, and the earlier framing
was wrong.

The far end is a **Chelsio 10G SFP+** (`enp4s0d1`, OUI `00:07:43`), `UP` and enslaved to bridge
`vmbr1` together with the test container's `eth1`. It reports `NO-CARRIER` — but that is
**explained by our side being silent**, not by the far end being dark: `PORT_STATUS` bit 11 is
`SerXmit`, and Et3 has it clear. et1 reads `0x0ec0` (bit 11 set, transmitting); Et3 reads
`0x0015`. We never transmit, so the Chelsio can never see us.

**A module is installed in cage 3 and it is receiving light.** The SCD per-port registers split
cleanly:

| ports | scd | state |
|---|---|---|
| 1, 2, **3** | `0x00000180` | 1 and 2 are linked; low bits 0-2 clear |
| 4-8 | `0x00000187` | empty cages; low bits 0-2 set |

Bits 0-2 are the SFP status group (module-present / TxFault / RxLOS in some order). Port 3
matches the two *working* ports exactly, so the module is present and not asserting
loss-of-signal. Bit assignment is inference; the populated-vs-empty contrast is not.

⚠ `PORT_STATUS = 0x15` means nothing on its own. **All six unconfigured ports read exactly
`0x15` / `LANE_STATUS=0`** — it is the never-brought-up default, not a signal or fault report.

### How far the "identical to a working lane" claim now goes

Read off the live chip, EPL14 lane 0 (et1, up) vs EPL14 lane 1 (Et3, dark), after
`fm6000_lanelink 3`:

| register | et1 | Et3 |
|---|---|---|
| `MAC_CFG` all 4 words (`0x10`-`0x13`) | `2000033c 400003e0 00002414 00001841` | **identical** |
| `PCS_10GBASER_CFG` (`0x25`) | `00000000` | **identical** |
| `SERDES_RX_CFG`/`TX_CFG`/`0x3b`/`0x3c` | see table above | **matches EOS's live lane 1** |
| `EPL_CFG_A`/`CFG_B` (EPL-wide) | `7e0d7899` / `00090003` | identical (same EPL) |

Register *coverage* is identical too: `fm6000_eplinit`'s lane-0 offsets and
`fm6000_lanelink 3`'s lane-1 offsets are the same set, offset by 0x80 —
`01 02 04 10 11 12 13 18 19 25 28 34 35 37 39 3a 3b 3c 40 41`.

Only status registers differ (`PCS_10GBASER_RX_STATUS` 1 vs 0). Four consecutive `lanelink 3`
runs leave `PORT_STATUS` at `0x15` unchanged, and et1/et2 are never disturbed.

Names above are from the SDK register map (`asic/fm6000/tools/sdk_regmap.py`), which is also
what identified `0x10` as a **4-word** `MAC_CFG` and `0x26`/`0x27` as PCS RX/TX status.

**So the gap is not in the EPL/MAC/PCS register block, and not in the far end.** It is in the
SerDes analog/SBus domain. Both lanes' SerDes answer SBus reads (`result=4`, dev `0x49` lane 0,
`0x4a` lane 1) and differ at regs `0x20`/`0x21` (`fe`/`0e` vs `c0`/`80`) — but we have no bit
definitions for the SerDes-internal registers, and the SDK descriptor table covers MMIO
registers only, so nothing is concluded from that yet. Recovering those definitions is the
next concrete step.

⚠ SPICO firmware is **not** the gap: `fm6000_spico` uploads via receiver `0xFD`, the SPICO
*broadcast* device, so every SerDes instance gets it — lane 1 is not starved.

### The SerDes core, and what the SBus result codes actually mean

`fm6000_sbusdump 0x49,0x4a` (lane 0 = up, lane 1 = dark) reads the SerDes core registers the
EPL block says nothing about. Several differ. But two things have to be understood before any
of it is interpreted.

**1. Result code 1 is SUCCESS, not failure.** `fm6000_sbus.c` says bits 28:26 are "a result
code that `fm6000_lanelink` has always treated as non-zero means the op failed". That reading
is wrong: **reads return 4 and manifestly work**. These are the standard Avago SBus codes —
`0x01` write complete, `0x04` read complete, with the failure codes elsewhere. So reads *and*
writes are both being accepted by the bus.

(`fm6000_lanelink` is not actually damaged by this: its check is `if (r < 0)`, and `sbus()`
returns the code only when not-Busy, so 1 and 4 never abort it. Only a timeout does.)

**2. A write completes and the readback does not change.** Writing `0x22 <- 0xef` on the dark
lane (step 8's "set bits 0 and 1") returns `result=1`, and the readback is still `0xec`. Same
for `0x0d`. The consistent explanation is that read and write address **different internal
spaces at the same register number**, which is normal for this SerDes family: the read returns
an *observable*, the write goes to a *control*.

⚠ **Consequence: control state cannot be verified by reading it back**, and an earlier reading
in this session — that lane 1's `0x22` lacking bits 0,1 while lane 0 has them proved "the
enable bits were never set" — **does not hold**. That compared observables against a
control-register step. The lane-to-lane difference is real and is a symptom; it is not proof
about the enable path.

Corroborating that these are observables: lane 0's values **drift between reads** (`0x22`
went `0x43` -> `0x67` across two dumps) because its equaliser is adapting on a live link, while
the dark lane's are static. Control registers would not drift.

**The only valid test of a SerDes write is behavioural.** Setting `0x22` bits 0,1 in isolation
left `PORT_STATUS` at `0x15` — but step 8 is the eighth of eighteen steps, so that is not
evidence against the algorithm either.

### Next: `fm6000_serdes_enable`

`EOS-SOURCES.md` already carries the full 18-step lane-enable algorithm recovered from
`fm6000EnableSerDes`, register by register — the `0x22` clear/set bracket, the `0x0f` PLL-lock
wait, the `0x14` signal-detect wait, then DFE tuning. Nothing implements it; `fm6000_lanelink`
replays a captured op list instead, which is why it can be byte-perfect on the EPL side and
still leave the lane dark.

That tool is the next piece of work, and its acceptance test is already defined: `PORT_STATUS`
bit 11 (`SerXmit`), 0 today and 1 on a working lane.

### ⚠ The blocker, isolated: SerDes core writes complete and do nothing

`EOS-SOURCES.md` ends its lane-enable section with "before implementing this: verify that our
SBus writes land". They do not, and this is now measured rather than suspected.

| dev | reg | before | wrote | result | readback |
|---|---|---|---|---|---|
| `0x4a` (Et3) | `0x22` | `ec` | `ef` | 1 = write complete | `ec` |
| `0x4a` | `0x0d` | `a5` | `b5` | 1 | `a5` |
| `0x4b` (port 5) | `0x3b` | `44` | `5a` | 1 | `44` |
| `0x4b` | `0x1d` | `f8` | `33` | 1 | `f8` |

Four registers, two devices, every one unchanged. The transaction is well formed — `fm6000_sbus`
does `0xF002 <- data; 0xF001 <- 0; 0xF001 <- cmd|Exec;` poll Busy, read `0xF003`, which is the
same routine `fm6000_spico` uses.

**And that routine demonstrably works**: `fm6000_spico` writes device `0xFD` (the SPICO
broadcast) every boot to upload IMEM, and the chip reports `SPICO LOADED + RUNNING`. So the
write path is sound in general; the **SerDes core devices specifically ignore writes**.

The likely reading — untested, and not to be acted on until it is — is that these registers are
owned by the SPICO microcontroller once it is running, and are effectively read-only from the
SBus. `fm6000_sbus` already has an `irq <target-dev> <code> [arg]` subcommand, which is the
SPICO-interrupt mechanism such parts use instead of direct register pokes. Whether the lane
enable must go through interrupts, or whether the SPICO must be halted first, is the question
to answer next.

⚠ Do **not** test this by writing to `0xFD`. It is the broadcast device: it reaches every SerDes
on the chip, including et1's and et2's, which are carrying traffic.

**This blocks `fm6000_serdes_enable`.** There is no point implementing an 18-step RMW algorithm
while every write is a no-op. Sequence: settle the write mechanism first, then implement.

### Correction to an earlier entry in this section

An entry above proposed that read and write hit *different internal spaces* at the same register
number. That does not survive: the SDK's own steps are **read-modify-writes** on those very
registers (`fm6000EnableSerDes` reads `0x22`, modifies, writes `0x22`), which is only coherent
if both halves address the same register. The observed behaviour is not two spaces — it is
writes having no effect at all.

## Why ports 1 and 2 come up and port 3 does not — the direct answer

Counted straight out of the executed replay:

| device | port | EPL MMIO writes | **SerDes SBus ops** |
|---|---|---|---|
| `0x49` | 1 (EPL14 lane 0) | 459 | **44** |
| `0x45` | 2 (EPL16 lane 0) | 1236 | **45** |
| `0x4a` | **3 (EPL14 lane 1)** | 391 | **0** |
| `0x4b` | 5 (EPL14 lane 2) | 391 | **0** |

EOS's capture contains the SerDes lane bring-up for exactly the two ports that were in use when
it was taken. **Port 3's SerDes device receives zero SBus operations in the entire replay.**

That is the whole asymmetry, and it explains every observation in this document at once:

- Port 3's **EPL/MAC/PCS registers are written** (391 of them), which is why they compare
  byte-identical to a working lane — the MMIO half of bring-up happens for every lane.
- Port 3's **SerDes is never enabled**, because the SBus half only ever targeted `0x49` and
  `0x45`. The lane is not misconfigured; it was never turned on.
- `fm6000_lanelink 2` works and `fm6000_lanelink 3` does not, and that is not a contradiction:
  port 2's SerDes was already enabled by the replay, so lanelink only has to **retrain** it.
  For port 3 lanelink would have to perform a **cold enable**, and it cannot, for the reasons
  `EOS-SOURCES.md` gives — the sequence is read-modify-write plus two hardware waits, and a
  replayed capture carries neither.

### ⚠ Correction: "SerDes core writes complete and do nothing" was overbroad

The entry above concluded from four registers (`0x22`, `0x0d`, `0x3b`, `0x1d`) that SerDes
writes never take effect. That does not hold:

- Those four are registers `fm6000_lanelink` only ever **reads**. Its write set is `0x01`,
  `0x02`, `0x06`, `0x17`, `0x2a`, `0x2b`; it reads `0x1f`-`0x27`. The test sampled the
  read-only side of the map.
- Writes to registers in lanelink's *write* set also leave the readback unchanged — so
  **readback is not a valid check on this bus at all**, which is the durable lesson and is
  already recorded in `fm6000_sbus.c`.
- Judged behaviourally instead, writes plainly do work: `fm6000_lanelink 2` retraining et2 out
  of a HiBer lock (`0x0CC0`, `pcsRx=1`) is a write sequence having a visible effect.

So the blocker is **not** that writes are ignored. It is that nothing has ever performed the
cold SerDes enable for lane 1, and the only faithful way to have it remains implementing
`fm6000_serdes_enable`.

## Running the captured sequence AT BOOT — tested, does not work (alpha33)

The last untested condition was timing: every attempt until now was post-boot, on a chip with
traffic already flowing. alpha33 wires `fm6000_lanelink 3` into `fm6000-fullseq.sh` in the same
phase where et2's retrain loop runs, and boots it.

    [fs]   et2 retrain attempts=0 et2=000008c0/00000940 pcsRx=00000001
    [fs]   et3 attempts=3        et3=00000015/00000000 pcsRx=00000000
    [fs]   post-spico et1=00000cc0/00000940  et2=000008c0/00000940

Three attempts, still dark. **Harmless** — et1 and et2 came up clean on the same boot — but it
costs ~24s, so it is left in with `PORT3` defaulting to **0**.

⚠ Also ruled out along the way: **SPICO state is not the discriminator.** The replay starts the
SPICO at line 131,048 (`dev 0xFD reg 0x0c <- 8`, after the IMEM upload) and does not touch port
1's SerDes until line 259,139. Ports 1 and 2 were therefore enabled *with the SPICO already
running* — the same condition as every post-boot attempt. An earlier theory that they got in
"before the SPICO owned the registers" is wrong.

Also ruled out: pacing (`lanelink -d 2000` and `-d 20000`, no change), and SBus devices
`0x01`-`0x04` (168 ops each, but all **reads** of regs `0x00`/`0x0f` at lines 7-2002 — early
readiness polling, not configuration, so lanelink omitting them is correct).

## What EOS actually writes, and whether it is blob

**It is blob.** The SerDes bring-up lives in `fwd4.txt` and executes at boot: 44 ops to device
`0x49` (port 1) and 45 to `0x45` (port 2). Authoring it therefore serves both goals at once —
it removes 89 EOS-derived ops *and* is the only route to port 3.

**And the data is fully in hand.** Extracted from the replay, the two sequences are **identical
in every written value** and differ only by one extra poll of register `0x1f`:

    21 01 1f    21 02 3f    20 00 00    21 17 10    21 06 00
    22 1f  22 24  22 20  22 21  22 22  22 23  22 24        <- polls
    21 17 00    21 2a 0e    21 2b 02
    22 1f  22 24  22 25  22 26  22 27  22 1f               <- polls
    21 2a 16 / 21 2a 0e  x11                               <- toggle pair

So the values are **lane independent** — the concern that RMW makes them unreplayable does not
apply to the write half. `21 17 10` is step 7 of the SDK algorithm ("RMW `0x17` set bit 4")
appearing verbatim, which cross-validates the extraction against the disassembly.

What a capture cannot carry is the **polls**. Port 1 needed one fewer iteration of the `0x1f`
read than port 2 — on a warm lane. A cold lane needs an unknown number, and a replay issues a
fixed count and moves on. That is the whole remaining gap, and it is what `fm6000_serdes_enable`
has to supply: these exact writes, with the reads turned into real wait-until-ready loops.

## `fm6000_serdes_enable` — implemented, and what it proved (2026-08-20)

`asic/fm6000/fm6000_serdes_enable.c` implements the enable as an **algorithm**: the ten RMW
steps whose operation is known exactly, plus both waits with their real parameters (1 ms
interval, 5000 retries). Four steps are deliberately skipped and it says so at runtime —
steps 3-6 write values computed by SDK arithmetic that is not decoded, and steps 2/14/17/18 are
separate SDK functions.

Run against port 3:

    before: PORT_STATUS=00000015  SerXmit=0
    rmw  reg 22  00 -> 00   (clear bits 0,1)
    rmw  reg 17  c0 -> d0   (xor bit 4)
    rmw  reg 22  00 -> 03   (set bits 0,1)
    wait reg 0f -> 3f after 1 ms   (PLL lock, b0+b3)
    rmw  reg 06 / 03 / 1f / 26      (set b3 / set b0 / mask 3f / set b0)
    rmw  reg 0d  a5 -> b5   (set bits 4,0)
    wait reg 14 -> d4 after 1 ms   (signal detect, b6)
    after : PORT_STATUS=00000015  SerXmit=0  LANE_STATUS=00000000

**Both waits pass.** That is the first positive result on this lane:

- **PLL lock** — reg `0x0f` = `0x3f`, bits 0 and 3 set.
- **Signal detect** — reg `0x14` = **`0xd4`**, bit 6 set. Earlier in the same session that
  register read `0x14`, bit 6 **clear**. So `sbus_rx_ib_sig_strength_obs` is now asserting:
  the SerDes is seeing signal from the Chelsio. That independently confirms the cable, the
  module and the far end, from the silicon's own point of view.

Corollary: **SerDes writes do take effect.** Reg `0x14` changing from `0x14` to `0xd4` across
our write sequence is an observable moving in response to our writes — which the readback test
could never have shown.

**The lane still does not transmit**, so the remaining gap is in the skipped steps.

### Next target: `fm6000SetTxConfig`

Step 14, and the obvious candidate for `SerXmit`. Disassembled at `0x483a80` (2904 bytes) it is
three read-modify-writes on registers **`0x3d`, `0x3e`, `0x41`**, the last of which does
`or 0x2` then `or 0x1` — enable-shaped. The nibble masks (`and 0xf`, `and 0x3` with `shl 4`)
line up with the pre/post/drive tx-equalisation fields the port table already carries.

⚠ **But the addressing differs and must be resolved first.** `fm6000EnableSerDes` computes
`(lane << 8) + 0xd11RR`; `fm6000SetTxConfig` computes `(lane << 8) + 0xb05RR`. Our SBus
transaction carries only `(op, dev, reg)`, so what distinguishes the `0xd11` and `0xb05`
prefixes — a different ring, a different device derivation, or something else — has to be
settled before these three writes can be issued. Do not guess it.

## The SBus device inventory, measured (2026-08-20)

Probing every device 0x00-0x7f with two registers known to vary per device (`0x20`, `0x21`)
and discarding the bus defaults (`c0`/`80`, `00`/`00`, `01`/`01`) leaves **seven devices**:

| dev | r20 | r21 | what |
|---|---|---|---|
| `0x01`-`0x04` | `80` | `80` | the four the replay polls 168x each at init (reads of `0x00`/`0x0f`) |
| `0x45` | `c6` | `2a` | port 2 SerDes (EPL16 lane 0) |
| `0x49` | `fe` | `0e` | port 1 SerDes (EPL14 lane 0) |
| `0x4a` | `fb` | `31` | **port 3 SerDes** (EPL14 lane 1) |

**There is no separate TX-config device.** That settles the `0xd11` / `0xb05` prefix question
the previous section left open, and settles it against the obvious reading:

- `fm6000ReadSBus` (`0x479794`) dispatches on address *ranges* — `0xd1100..0xd14ff` and
  `0xb0500..0xc04ff` both fall through to the **same** handler (`0x476cb0`), differing only in
  which descriptor table they pass (`ebx+0x5754` vs `ebx+0x58f4`, and `ebx` = `0x60420C`).
  Those tables are `{address, 0}` arrays — bookkeeping, not name tables and not device select.
- So the prefixes are register **namespaces inside the SDK**, not different rings, masters or
  devices. `fm6000SetTxConfig`'s registers `0x3d`, `0x41`, `0x3e` are on the **lane device** —
  `0x4a` for port 3 — the same device `fm6000EnableSerDes` uses.
- The earlier derivation "TX device = serdes + 0x05" (predicting `0x3d`/`0x3e`) is **falsified**:
  those device numbers, and arbitrary controls `0x3f` and `0x2c`, all return identical defaults.

⚠ Note only three lane SerDes respond — `0x45`, `0x49`, `0x4a`, i.e. ports 2, 1 and 3, which are
exactly the three cages with a transceiver installed (SCD `0x180`; ports 4-8 read `0x187`).
Devices for the other five lanes do not answer. Whether that is power gating or absence is not
established, and it means an earlier read of "dev 0x4b reg 0x3b = 0x44" was a bus default, not
port 5's SerDes.

### Tried and insufficient

Setting reg `0x3e` bits 0 and 1 on `0x4a` — the unambiguous `or 0x2; or 0x1` half of
`SetTxConfig` — changes nothing (`0xbe` -> `0xbf`, `PORT_STATUS` stays `0x15`). et1/et2
unaffected. The remaining unknowns are the field composition of `SetTxConfig`'s three writes
(nibble masks over the pre/post/drive parameters) and steps 3-6 of the enable, whose values are
computed by SDK arithmetic that is still undecoded.

## `SetTxConfig` is NOT the missing piece — closed by measurement

Before decoding its three writes' field arithmetic, the cheaper question: do its target registers
even differ between a working and a dark lane?

| reg | 0x49 (port 1, up) | 0x4a (port 3, dark) | 0x45 (port 2, up) |
|---|---|---|---|
| `0x3d` | `aa` | `aa` | `aa` |
| `0x3e` | `be` | `be` | `be` |
| `0x41` | `01` | `01` | `01` |

**Identical across all three.** Port 3's TX configuration is already what the working lanes have,
so running `fm6000SetTxConfig` would change nothing. That closes the branch and makes decoding
its bitfield arithmetic unnecessary. (For the record, its first write composes
`v[1:0] = arg&0x3`, `v[5:2] = arg&0xf` via the `v ^= (v ^ new) & mask` insert idiom.)

## The three-way diff, and what it found

A better instrument than more disassembly: dump all 64 SerDes registers on **both** working lanes
and the dark one, and keep only registers where the two working lanes **agree** and the dark lane
**differs**. Agreement between two independent good lanes filters out per-lane state.

| reg | up (0x49, 0x45) | Et3 (0x4a) |
|---|---|---|
| `0x01` | `25` | `05` |
| `0x0b` | `00` | `20` |
| `0x0c` | `01` | `04` |
| `0x0d` | `a4` | `a5` |
| `0x15` | `02` | `00` |
| `0x1a` | `0f` | `e0` |
| `0x1e` | `0b` | `07` |

⚠ Note `0x0d` is `a4` on both **working** lanes — bit 0 **clear** — while `fm6000EnableSerDes`
step 15 sets bits 4 and 0 of `0x0d`. So either that bit is cleared again later in bring-up, or
step 15's register mapping needs re-checking. Our `fm6000_serdes_enable` currently drives it to
`b5`, which no working lane holds.

### Writing the working values across: no link, and a mixed result

Writing all seven working-lane values onto `0x4a` left `PORT_STATUS` at `0x15`. Re-running the
diff shows why it is not a simple story:

- `0x01` and `0x0c` **dropped off the differ-list** — those writes landed.
- `0x0b`, `0x0d`, `0x15`, `0x1a`, `0x1e` **still differ** — hardware-owned or read-only.
- **New** differences appeared at `0x0f`, `0x10`, `0x16`, `0x1f`, i.e. the lane's state moved.
  Et3's `0x0f` fell from `0x3f` to `0x0b`, losing PLL-status bits.

So SerDes writes reach some registers and not others, and the ones that matter for bring-up are
in the second group. This is consistent with everything else here: the lane is not missing a
value we can simply write, it is missing a **sequence** that leaves the hardware in the state
those registers report.

⚠ Port 3's SerDes has been written to by hand during this work. A reboot reprograms it from
`fm6000-fullseq.sh`; et1 and et2 were never touched and stayed up throughout.

## ⚠ Correction (2026-08-20): "we are not transmitting" was wrong

An entry earlier today read `PORT_STATUS` bit 11 (`SerXmit`) = 0 on Et3 and concluded that the
far end's `NO-CARRIER` was explained by "our side being silent". That is not what is happening,
and the optics say so directly. Re-measured live on `i2c-11`, A2h:

| | raw | value |
|---|---|---|
| TX bias | `1104` | 8.71 mA |
| **TX power** | `16f4` | **587.6 µW (−2.31 dBm)** |
| **RX power** | `14b7` | **530.3 µW (−2.76 dBm)** |
| status `0x6e` | `30` | no soft LOS, no soft TX fault |

`sfp3_txdisable=0`. **The laser is on and emitting, and light is arriving.** `SerXmit` clear
means the MAC/PCS is not feeding valid 10GBASE-R encoding into a transmitter that is otherwise
lit — not that the port is dark. Both directions carry light; neither end can lock to it.

This also re-confirms the earlier finding that the I2C side is fully solved: `scd` +
`scd_hwmon`, 61 buses, SFP cages on master 3 buses 0-7 = `i2c-9`..`i2c-16` for Ethernet1..8, so
port 3 is `i2c-11`. Nothing about the laser, the module or the cage needs further work.

## ★ Where to look: the RX equalizer (DFE)

The EPL side is now conclusively excluded. A three-way diff of all 128 words of the lane block —
et1 and et2 as controls, Et3 as subject, keeping only registers where **both working lanes agree
and the dark one differs** — yields exactly four, and every one is a *status* register:

| offset | up | Et3 | register |
|---|---|---|---|
| `+0x26` | `00000001` | `00000000` | `PCS_10GBASER_RX_STATUS` |
| `+0x38` | `00000940` | `00000000` | `LANE_STATUS` |
| `+0x3e` | `00100f0f` | `0c100fe0` | `SERDES_STATUS` |
| `+0x41` | `000004f0` | `000004b0` | `SERDES_IP` (interrupt pending) |

**No configuration register differs anywhere in the block.**

`SERDES_STATUS` is the only difference that is not simply "not locked", and the SDK field table
gives its layout (a 19-field run spanning bits 0-42, matching its declared 2-word width):

    bits  0-15  AnalogToCore      bits 26-27  RxSigStrength
    bits 16-17  Rx_8b10bCommaDet  bit  28     RxElecIdleDetect
    bits 18-19  Rx_8b10bDisparityErr   bit 37 TxRdy
    bits 20-21  Rx_8b10bOutOfBandErr   bit 38 RxRdy
    bits 22-23  Rx_8b10bSlipInProgress bits 39-42 TxPhaseOut
    bits 24-25  RxPatternCmpPass

Decoded:

| field | up | Et3 |
|---|---|---|
| `AnalogToCore` | `0f0f` | **`0fe0`** |
| `RxSigStrength` | **0** | **3 (saturated)** |
| `RxElecIdleDetect` | 0 | 0 |

So on the dark lane the receiver has **strong signal** (strength at maximum), a **locked PLL**
(reg `0x0f` bits 0 and 3), **healthy optical power** (−2.76 dBm, more than the working port
receives) — and still no block lock, with a different analog-to-core value.

That is the signature of an **RX equalizer that has not adapted**, and it is the one part of
bring-up we have never performed: `fm6000_serdes_enable` explicitly skips steps 17-18
(`fm6000StartSerDesDfeTuning`, `fm6000CheckSerDesDfeTuningState`), and DPDK's open FM10000 driver
lists DFE iCal/pCal as a required stage with 3000 ms / 2000 ms budgets.

### The three things to do, in order

1. **Disassemble `fm6000StartSerDesDfeTuning`** (`0x4877a1`, 2097 bytes) and
   `fm6000CheckSerDesDfeTuningState` (`0x48d9e2`, 913 bytes). Both are exported symbols; this is
   the same technique that recovered the two waits exactly.
2. **Finish the interrupt result path** so the DFE diagnostic works. `fm6000_sbus irq`'s request
   encoding is now correct, but its *trigger and result read* (`0x0c <- 0x18`, `0x0c <- 0x08`,
   then reading regs `0x00`/`0x01`/`0x02`) are inherited from the old wrong version and are not
   verified. The SDK's own result helper is the second static function called by
   `fm6000InterruptSpicoV2`, at **`0x47793d`**. With that right, interrupt `0x20` gives a live
   eye-score/DFE readout for a lane that will not train — the diagnostic this whole effort has
   lacked.
3. Only then implement DFE tuning, and re-test.

⚠ Do **not** go back to the EPL side, `SetTxConfig`, the optics, the module, or the laser. Each
has now been excluded by measurement, and the exclusions are recorded above.

## DFE tuning implemented — it converges, and it moved the analog state (2026-08-20)

**What the DFE is.** At 10 Gbps the channel smears each symbol into its successors
(inter-symbol interference). The decision feedback equaliser subtracts the estimated ISI of
already-decided bits from the current sample, reopening the eye so the slicer can decide 0 from
1. Its taps are specific to the physical channel, so they must be *trained* — iCal is the
initial search, pCal tracks drift afterwards. **An untrained DFE leaves the eye shut even with
abundant signal**, which is precisely Et3: `RxSigStrength` saturated, PLL locked, −2.76 dBm
arriving, no block lock.

**Recovered from the SDK**, both exported symbols, same technique as the two waits:

- `fm6000StartSerDesDfeTuning` (`0x4877a1`): read reg `0x17`, clear its low 5 bits, write back;
  then write `0x2a`; then `0x2b`. Registers confirmed from the address setup at
  `0x487921-0x48796e` — and they are the same three the EOS capture writes, including its
  `2a = 0x16/0x0e` alternation.
- `fm6000CheckSerDesDfeTuningState` (`0x48d9e2`): state read from reg **`0x1f`**, fields
  extracted with `shr 4` and `shr 2`. Measured: bits 3:2 read **2 on both working lanes** and
  **1 on the dark lane**, so 2 is converged. That is the SDK's own completion test, and the
  capture polls `0x1f` repeatedly for exactly this reason.

**Result on port 3:**

    dfe  reg 17  c0 -> c0   (clear low 5 bits)
    dfe  reg 2a <- 0e, reg 2b <- 02   (start tuning)
    dfe  reg 1f -> 39, state=2 CONVERGED after 311 ms
    wait reg 14 -> d4 after 151 ms    (signal detect -- a real wait now, not instant)

**The DFE converges, and the analog state followed**: `SERDES_STATUS.AnalogToCore` moved
`0fe0` -> `0f0f`, now byte-identical to both working lanes. Reg `0x1f` left the differ-list.
The lane still does not lock.

### ⚠ New lead: our own sequence drives two registers AWAY from the working value

Re-diffing after the run, two of the remaining differences are ones **we caused**:

| reg | both working lanes | Et3 after our run | note |
|---|---|---|---|
| `0x0d` | `a4` (bit 0 **clear**) | `a5` (bit 0 **set**) | our step 15 sets bits 4 and 0 |
| `0x14` | `14` (bit 6 **clear**) | `d4` (bit 6 **set**) | our step 16 *waits for* bit 6 to set |

Both working lanes sit at values our implementation treats as the target to move away from. So
either those two steps' register mapping is wrong, or the bits are cleared again later in a part
of bring-up we have not implemented. `SERDES_STATUS.RxSigStrength` tells the same story: **3
(saturated) on Et3, 0 on both working lanes** — and the SDK has a `MaskRxSigStrength` field,
so a working lane may simply have signal-strength reporting masked off.

**That is where to look next**: re-check enable steps 15 and 16 against the disassembly, and find
what clears `0x0d` bit 0 and `0x14` bit 6 on a lane that ends up working.

## Verified: EOS does NOT run `fm6000EnableSerDes` at boot

All twelve `fm6000EnableSerDes` writes were re-checked against their actual address locals
(`[ebp-0xc]`=`0x00`, `-0x10`=`0x03`, `-0x14`=`0x06`, `-0x18`=`0x0d`, `-0x1c`=`0x17`,
`-0x20`=`0x1d`, `-0x24`=`0x1f`, `-0x28`=`0x22`, `-0x2c`=`0x26`, `-0x30`=`0x36`, `-0x34`=`0x3b`).
**Every mapping is correct**, including step 15 -> reg `0x0d`. So the implementation reads the
disassembly correctly.

The discrepancy is elsewhere: **EOS's captured boot sequence writes only regs `0x01`, `0x02`,
`0x17`, `0x06`, `0x2a`, `0x2b` plus a reset.** It never touches `0x0d`, `0x22`, `0x03`, `0x1f`,
`0x26`, `0x00`, `0x1d`, `0x36` or `0x3b`. `fm6000EnableSerDes` is a different (cold or
diagnostic) path that the boot flow does not use — so running it drives the lane into a state
EOS never produces, which is what `fm6000_serdes_enable` without `-D` had been doing.

⚠ One guess corrected by the reboot: `0x0d = a5` on Et3 is **native**, not contamination from our
writes — a pristine post-`fullseq` lane reads `a5` while both working lanes read `a4`. Only
`0x14 = d4` was ours (from asserting signal detect), and it returns to `14` after a reboot.

## Current best state: three registers from a working lane

On a pristine lane: `fm6000_lanelink 3` (the captured writes) + `fm6000_serdes_enable -D 3`
(DFE tuning with the real wait). DFE converges in 76 ms and **reg `0x1f` reaches `0x29`, the
exact working-lane value**. The whole SerDes core diff is then:

| reg | up | Et3 |
|---|---|---|
| `0x0d` | `a4` | `a5` — writing `a4` does not stick; hardware-owned |
| `0x1e` | `0b` | `07` |
| `0x2b` | `04` | `03` — the capture writes `02`; working lanes settle at `04`, ours at `03` |

`SERDES_STATUS.AnalogToCore` matches the working lanes, DFE state matches, PLL locked, signal
present, optics good — and `LANE_STATUS` is still `0`.

`0x2b` is the interesting one: it is *written* `0x02` by the capture and *ends* at `0x04` on a
working lane, so it advances on its own. Ours stops at `0x03`. That looks like a phase or
progress counter for a multi-step tuning process that completes on a good lane and stalls one
step short here — which would fit the capture's `0x2a` alternation (`0x16`/`0x0e` x11) being an
iteration our single-shot DFE call does not perform.

**Next: replicate the `0x2a` toggle loop**, polling `0x2b` until it reaches `0x04` rather than
issuing the writes a fixed number of times.

## ★★ The SerDes is up: SERDES_STATUS byte-identical to both working lanes

Adding the iteration the capture cannot carry finished the SerDes side.

`fm6000_serdes_enable -D` now does: start tuning (reg `0x17` low 5 bits cleared, `0x2a <- 0x0e`,
`0x2b <- 0x02`), poll reg `0x1f` bits 3:2 for state 2, then **toggle reg `0x2a` between `0x16`
and `0x0e` while polling reg `0x2b` until it reads `0x04`** — the working lanes' value — instead
of counting to eleven as the trace does.

    dfe  reg 2a <- 0e, reg 2b <- 02   (start tuning)
    dfe  reg 1f -> 29, state=2 CONVERGED after 107 ms
    dfe  reg 2b -> 04 after 2 iterations

Result, read straight off the chip:

```
SERDES_STATUS word0 (+0x3e):  et1=00100f0f  et2=00100f0f  Et3=00100f0f
SERDES_STATUS word1 (+0x3f):  et1=00000060  et2=00000060  Et3=00000060
```

**Identical on all three lanes.** Word 1 `0x60` is bits 5 and 6 = **`TxRdy`** (field bit 37) and
**`RxRdy`** (field bit 38). Et3's SerDes reports transmitter and receiver ready, and
`AnalogToCore` and `RxSigStrength` now match the working lanes too.

The whole EPL diff is down to three registers, and all three are lock indicators:

| offset | up | Et3 | |
|---|---|---|---|
| `+0x26` | `00000001` | `00000000` | `PCS_10GBASER_RX_STATUS` |
| `+0x38` | `00000940` | `00000000` | `LANE_STATUS` |
| `+0x41` | `000004f0` | `000004b0` | `SERDES_IP` |

### Where the fault now is

Not the SerDes. Every SerDes-side indicator matches a working lane, yet `PORT_STATUS` bit 11
(`SerXmit`) is still 0 and the PCS reports no block lock.

The plausible reading — untested — is **ordering**: on ports 1 and 2 the SerDes became ready
*during* the replay, before and while the EPL was programmed, so the PCS saw a ready SerDes when
it started. Here the EPL was programmed at boot and the SerDes only became ready afterwards, and
the PCS has no reason to re-evaluate. If so, what is needed is a **PCS restart** after DFE
completes, not more SerDes work.

⚠ Do not guess which bit does that. `PCS_10GBASER_CFG` (`+0x25`) and `MAC_CFG` (`+0x10`, 4 words)
are the candidates, and the way to find it is the same one that has worked throughout: read what
the SDK's own port-enable path writes, rather than toggling bits on a live chip.

⚠ Also note reg `0x1f` drifts back to `0x25` (state 1) some time after converging. Whether that
is the DFE re-adapting because the PCS never consumes the recovered data, or a genuine loss of
convergence, is not established.

## The last step is not a register write — it is an event handler EdgeNOS does not have

Chasing "make the PCS re-evaluate" through the SDK rather than by toggling bits:

- **`fm6000SetPcsRxReset`** (`0x435d82`) computes `(lane << 7) + 0xe0037` — i.e. **lane_base
  `+0x37`** — and manipulates **bit 23** (`and eax,0x1; shl eax,0x17`). All three lanes read
  `0x000c0002` with bit 23 clear. **Pulsing bit 23 on Et3 changed nothing.**
- **`fm6000SetPortState`** (`0x42c5ac`) touches only `0xe0304`. Not the PCS starter.
- **`fm6000Init10GBaseR`** (`0x362193`) writes `PCS_10GBASER_CFG` (`0xe0025`) — but that register
  reads `0` on the working lanes too, so it is not the differentiator either.
- **`fm6000SerDesEventHandler`** (`0x364a89`, **14,726 bytes**) drives exactly six registers:

| reg | |
|---|---|
| `0xe0041` (x2) | `SERDES_IP` — interrupt pending, write-1-to-clear |
| `0xe0040` | `SERDES_IM` — interrupt mask |
| `0xe003a` / `0xe0039` | `SERDES_TX_CFG` / `SERDES_RX_CFG` |
| `0xe0037` | the PCS reset above |
| `0xe0034` | `SERDES_CFG` |

**That is the missing piece, and it is runtime logic, not configuration.** EOS completes port
bring-up with an interrupt-driven handler that reacts to SerDes state changes — it clears
`SERDES_IP`, adjusts TX/RX config, and pokes the PCS reset. EdgeNOS has no equivalent: our boot
programs registers once and never reacts.

The evidence for this being the live gap is `SERDES_IP` itself, one of the three registers still
differing after the SerDes is fully up:

| reg | working lanes | Et3 |
|---|---|---|
| `+0x41` `SERDES_IP` | `000004f0` | `000004b0` — **bit 6 clear** |

The working lanes have that interrupt pending; ours does not. On ports 1 and 2 a SerDes event
fired during the replay, the handler ran, and bring-up completed. On Et3 the SerDes reaches
`TxRdy`/`RxRdy` — verified byte-identical — and nothing is listening.

⚠ So the remaining task is **not** "find the missing register write". It is to implement the
part of `fm6000SerDesEventHandler` that runs when a lane becomes ready. That is a new piece of
work with a clear specification: six registers, one function, fully disassemblable.

## Replicating the handler's writes is not sufficient — and why

`fm6000SerDesEventHandler`'s two `SERDES_IP` sites write **`0x20`** (`0x3650fb`) and **`0xe0`**
(`0x366cf1`) — write-1-to-clear on bits 5, and 5/6/7. Its other five registers are held in
locals `[ebp-0x144]`=`0xe0037`, `-0x140`=`0xe0034`, `-0x13c`=`0xe0039`, `-0x138`=`0xe003a`,
`-0x134`=`0xe0040`. It also calls `fm6000StartPortLaneDfeTuning` twice, so DFE is part of it.

Replicated on Et3 after a full SerDes bring-up — clear `SERDES_IP` with `0xe0`, rewrite
`SERDES_CFG`/`RX_CFG`/`TX_CFG` with their (already correct) values so the write happens *after*
the lane is ready, then pulse the `+0x37` PCS reset. **No link.** The write landed
(`SERDES_IP` `0x4b0` -> `0x410`), so the mechanism works; the effect does not.

### The actual discriminator: SERDES_IP bit 6

| | `SERDES_IP` (`+0x41`) | bits 5,6,7 |
|---|---|---|
| et1, et2 | `000004f0` | **5, 6 and 7 all pending** |
| Et3 (pristine) | `000004b0` | 5 and 7 pending, **6 never fires** |

Both working lanes hold that interrupt pending. Et3 never raises it, through every sequence
tried. If `SERDES_IP` shares `SERDES_IM`'s field order — the SDK field table gives IM as
`MaskRxSigStrength`(4), `MaskKrRxFault`(5), `MaskKrRxTrained`(6), `MaskKrSignalDetect`(7),
`MaskKrTrainingFailure`(8), `MaskRxRdy`(9) — then bit 6 is **KrRxTrained**. ⚠ That is an
inference from IM's layout, not established for IP, and KR training is a backplane feature
whose relevance to a 10GBASE-SR fibre port is unclear. Do not act on it without confirming
IP's own field layout.

⚠ **Clearing `0xe0` moved Et3 further from the working state, not closer.** The working lanes
have those bits *set*. A reboot restores the pristine `0x4b0`; do that before the next
measurement.

### Honest status

The SerDes side is finished and verified. The remaining gap is not a register value — every
configuration register on both the EPL and SerDes sides now matches a working lane, and
`SERDES_STATUS` is byte-identical including `TxRdy` and `RxRdy`. What differs is one interrupt
that never asserts, and the 14 KB of conditional logic in `fm6000SerDesEventHandler` that
decides what to do about it. Implementing that properly means decoding the handler's control
flow, not transcribing its six register writes — which has now been tried and does nothing.

## ★★★ Port 3's SerDes appears to be in NEAR-END LOOPBACK

Two decodes converge on this.

**1. `PORT_STATUS` decoded properly.** Its field layout, from the SDK field table (a 9-field run,
confirming the comments in `tools/fm6000-status.sh` were right):

    bits 0-1 LinkFaultDebounced   bit 6 RxLinkUp    bit  9 Transmitting
    bits 2-3 LinkFaultMac         bit 7 HeartbeatOk bit 10 Receiving
    bits 4-5 LinkFaultRx          bit 8 HiBer       bit 11 SerXmit

Et3's `0x15` is therefore **`LinkFaultDebounced`=1, `LinkFaultMac`=1, `LinkFaultRx`=1** — the
receiver is signalling local fault because the PCS has no 64b/66b block lock, so the MAC emits
fault ordered-sets instead of data. `SerXmit=0` is a *consequence*, not an independent problem.
Everything reduces to: **no block lock**.

**2. `fm6000SetSerDesNearLoopback`** (`0x47feed`) writes register **`0x0d`, setting bit 0**
(`or eax,0x1` at `0x480210`, address `(serdes<<8) + 0xd110d`).

And that is the register this document has repeatedly flagged and repeatedly dismissed:

| reg | et1 (up) | et2 (up) | Et3 (dark) |
|---|---|---|---|
| `0x0d` | `a4` — bit 0 **clear** | `a4` — bit 0 **clear** | `a5` — bit 0 **SET** |

**Port 3's SerDes has near-end loopback enabled**: its receiver is fed from its own transmitter
rather than from the fibre. That single fact would explain the entire symptom set — saturated
`RxSigStrength`, a DFE that converges quickly on a short internal path, healthy optics that the
receiver never actually looks at, and no block lock.

It is present on a **pristine** post-`fullseq` lane, so it is not something this investigation
caused. The likely reading: it is the power-on default, EOS's bring-up clears it for the lanes
it configures, and nothing in our boot clears it for port 3 — the same shape as every other
port-3 gap.

### ⚠ The bit will not clear by writing it

`fm6000_sbus write 0x4a 0x0d 0xa4` does not take. That is established against **two controls**
via the three-way diff (`0x0d` stays `a5` while both working lanes read `a4`), not by readback —
readback is not a valid check on this bus.

So the next task is to find what gates writes to `0x0d`. The SDK field table shows this SerDes
family has explicit **gate** registers (`sbus_rx_far_loopback_gate`, `sbus_tx_data_gate`,
`sbus_rx_data_gate`, `sbus_rx_error_monitor_gate`, `sbus_tx_pre_emphasis_gate`), and
`fm6000EnableSerDes` step 18 pairs near-loopback with `fm6000SetSerDesRxDataGate(.., 0)`. Read
`fm6000SetSerDesRxDataGate` and the gate registers next; the write almost certainly has to be
unlocked rather than repeated.

### ⚠⚠ CORRECTION: bit 0 of reg `0x0d` is NOT the loopback enable

The section above claimed port 3's SerDes is in near-end loopback because reg `0x0d` bit 0 is
set on Et3 and clear on both working lanes. **That reading is wrong.** Disassembling
`fm6000SetSerDesNearLoopback` properly (`0x4801f0`-`0x48022a`):

```
    and  dl,0x7f              ; clear bit 7 of the read value
    eax = (arg != 0) ? 0x80 : 0
    or   eax,edx              ; bit 7 := the enable argument
    or   eax,0x1              ; bit 0 ALWAYS set, regardless of argument
    write reg 0x0d <- eax
```

**Bit 7 is the loopback enable. Bit 0 is set unconditionally** — it is a side effect of the
function having run, not a mode.

And both working lanes *and* Et3 read bit 7 **set** (`a4` and `a5` both have it), so the read
value cannot be used to decide loopback state at all — consistent with read and write hitting
different spaces on this bus, which is already recorded in `fm6000_sbus.c`.

What the bit-0 difference does say is narrower but still real: **`SetSerDesNearLoopback` has run
on Et3 and has not run on either working lane.** On a pristine post-`fullseq` boot Et3 reads
`a5`, so something in our boot path invokes it — `fm6000_lanelink`'s op list does not write
`0x0d`, so the caller has not been identified.

⚠ Two failed attempts on this register are now recorded, both from reading a value whose write
semantics differ: first dismissing `0x0d` as "hardware-owned" on a readback check, then reading
bit 0 as the loopback mode. **Do not infer this register's meaning from its read value.** The
only sound approaches are to decode the writer, or to change it and judge by `PORT_STATUS`.

### Also stubbed: `fm6000SetSerDesRxDataGate`

`0x490761`, 19 bytes: it stores its argument and returns 0. It does nothing. The "find the gate
that unlocks `0x0d`" plan is therefore dead as stated — at least via that function.

## ★ The loopback discriminator: the fault is OURS, downstream of the SerDes

The question the whole investigation reduced to: can our receiver lock to *any* valid signal, or
is the far end the problem? Near-end loopback answers it without touching the lab.

Using the correct bit (7, per the corrected decode): loopback ON = write `0x0d <- 0xa5`,
OFF = `0x0d <- 0x25`.

| | DFE convergence | DFE iterations | `LANE_STATUS` |
|---|---|---|---|
| loopback **ON** | 64 ms | **4** | `00000000` |
| loopback **OFF** | 72 ms | **22** | `00000000` |

**No block lock in either state.**

Two things follow.

**The writes are taking effect.** 4 iterations versus 22 is exactly the difference between a
short internal path and a real fibre channel, so the loopback bit is being honoured even though
the read value stays `a5` in both cases — more confirmation that readback is meaningless on this
bus and that behaviour is the only valid instrument.

**The fault is on our side, downstream of the SerDes.** With loopback on, the receiver is fed
from our own transmitter over a clean internal path. Even in the worst case — the MAC emitting
local-fault ordered sets because it sees no link — those are still valid 64b/66b symbols with
correct sync headers, and the PCS should achieve block lock on them. It does not. So the SerDes
is recovering *something*, the DFE adapts to it, and the PCS never syncs.

That excludes the far end, the Chelsio, the fibre and the optics as causes of the *lock* failure
(they were already excluded as causes of anything else), and localises the remaining fault to
the **SerDes -> PCS data path**: either the PCS is held in a state where it does not consume the
lane, or the lane's data is not being delivered to it.

⚠ Caveat: the loopback bit's state could not be *verified*, only inferred from the iteration-count
difference. A stronger version of this test would use the SerDes's own PRBS generator and checker
(`sbus_rx_prbs_data_obs`, `sbus_rx_pattern_cmp_pass_obs`, `RxPatternCmpPass` in `SERDES_STATUS`)
to prove bit recovery independently of the PCS. That is the recommended next experiment.
