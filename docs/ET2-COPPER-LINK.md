# Et2 (10GBASE-CR / DAC) cold link — open problem

**2026-08-06.** Et2 does **not** link reliably under our cold bring-up in *any* configuration
tested. This file records the full experiment matrix so the next attempt doesn't repeat it.

## Ground truth

- Under **EOS**: Et2 is `connected`, 10G, `10GBASE-CR`, **0 input errors** — reliably, every boot.
  The DAC and the port are healthy. The gap is entirely in our bring-up.
- Under **our cold sequence**: Et2 has linked **exactly once**, and it has not been reproduced.
- **Et1 (10GBASE-SR, fibre) links every time, in every configuration below.** Only copper fails.

## Experiment matrix

| # | configuration | Et1 | Et2 |
|---|---|:--:|:--:|
| 1 | `fwd4` unmodified (SPICO firmware inline) | ✅ | **✅ `0x8c0` rx=1** |
| 2 | `fwd4` unmodified — *identical rerun* | ✅ | ❌ `0x815` rx=0 |
| 3 | `fwd4` unmodified, immediately after EOS had Et2 linked | ✅ | ❌ `0x815` rx=0 |
| 4 | `fwd5` (SPICO IMEM stripped) | ✅ | ❌ |
| 5 | `fwd5` + separate `fm6000_spico` load before the replay | ✅ | ❌ |
| 6 | `fwd6m` + `@SPICO_IMEM` marker injection at the original offset | ✅ | ❌ |
| 7 | as #6, with the `FD06` write-enable/disable bracketing restored | ✅ | ❌ |
| 8 | SFP-enable bounce (`SCD 0x5020` bit6) ×5 on a running chip | ✅ | ❌ |
| 9 | `fwd4` unmodified, after an EOS `shutdown`/`no shutdown` of Et2 | ✅ | **✅ `0x8c0` rx=1** |

**Et2 is INTERMITTENT: 2 successes in 4 `fwd4` runs.** Runs #1 and #9 worked; #2 and #3 did not,
with byte-identical inputs. So this is a marginal/racy link bring-up, not a missing register write.

**With both ports up (run #9) two-port operation was verified for the first time:**
60 frames to GLORT `0x03ef` → far-end swp6 **+60**; 60 frames to GLORT `0x03ee` → swp7 **+60**.
Per-port egress steering works on both ports simultaneously.

Original note kept for the record — run #1 was the only success at the time of writing** — not by rerunning the same script, and
not by recreating its apparent precondition (EOS having just had the link up).

## Leading theory: the replay runs too fast — but the evidence is thin

⚠️ **A previous revision of this file claimed "ROOT CAUSE FOUND". That claim was wrong** and is
retracted. It rested on a 3/3 result at `PACE=2000`, but `fm6000_fullreplay` sleeps `pace`
microseconds **once every 16384 ops**, not per op — so over ~390k writes it fires ~23 times and
`PACE=2000` adds **46 ms in total**. That is effectively no pacing at all, and 3/3 was luck.

Re-tested with a delay big enough to matter (`PACE=1500000` ≈ **34 s** added):

| replay pacing | real added delay | Et2 success |
|---|---|---|
| `0` (unpaced) | 0 | **2 / 5** |
| `2000` (≈ unpaced) | 46 ms | 3 / 4 |
| `1500000` | ~34 s | **1 / 1** |

One properly-controlled success is **not** proof. The theory is plausible and now the default, but
it needs more trials before anyone should believe it.

The mechanism fits every observation:

- **Intermittent.** A missing register write fails 100% of the time. A timing-dependent
  initialisation fails *sometimes* — exactly what we saw with byte-identical inputs.
- **Latched at bring-up.** Once Et2 misses lock, nothing recovers it: not 8 retries of the EPL
  sequence, not 6 replays of EOS's *complete* 2,632-write port bounce including the SBus lane reset
  (`op20`). A mis-sequenced SerDes init cannot be undone by a port bounce.
- **Only copper.** Et1 (10GBASE-SR fibre) links every time either way — an optical receiver needs no
  equalisation. Et2 (10GBASE-CR DAC) needs its RX equaliser to converge, and that needs settle time.
- **EOS always works** because it never bulk-writes: it polls, waits, and re-checks between steps.

The failure state decodes precisely from `PORT_STATUS`:

```
0x8c0 (good) = RxLinkUp | HeartbeatOk | SerXmit
0x815 (bad)  = LinkFaultDebounced=1, LinkFaultMac=1, LinkFaultRx=1, SerXmit=1
```

`LinkFaultRx = 1` is a **local** fault (0=none, 1=local, 2=remote): our PCS never achieves block
lock. In one sample `Receiving=1` was also set — signal present, but undecodable. Classic
equalisation failure.

`fm6000-fullseq.sh` now paces by default (`PACE=2000`, override with `PACE=<µs>`).

## What that means for earlier conclusions

Both of these were stated too strongly and are **withdrawn**:

1. ~~"The SPICO firmware is not required."~~ The bisect that produced this only ever checked **Et1**.
   It is sound for SR fibre and unproven for copper.
2. ~~"Stripping SPICO broke the copper link."~~ Concluded from run #1 vs #4. Runs #2 and #3 show
   plain `fwd4` fails too, so the difference is not the strip. The strip was verified surgical:
   exactly the 30,002 IMEM transactions (regs `0x04/05/06/07` on receiver `0xFD`) and nothing else —
   per-receiver transaction counts are otherwise identical between `fwd4` and `fwd5`.

Also still true, from earlier: the EPL `+5/+6/+7/+b/+c/+d` deltas vs EOS are **read-only status**,
not config, and loading SPICO alone (ALIVE, CRC OK) does not fix Et2.

## What is actually established

- Both SerDes lanes (SBus receivers `0x45` and `0x49`) receive **identical** programming — 15 real
  config transactions each, then a `2a` toggle loop between `0x0e` and `0x16`.
- The replay contains **no CR/copper-specific setup at all**: no per-lane polarity, drive,
  pre-emphasis or post-cursor anywhere, and `EPL_CFG_B` (`PcsSel=3`) is identical on both ports.
- The SPICO IMEM upload sits at **line 56,633 of `fwd4.txt`** (14.5% in), immediately after the MOD
  microcode. The replay *later* resets and starts the SPICO, so any firmware loaded **before** the
  replay is wiped — that part is real, and it is why a separate pre-load can never work.
- The asymmetry is one-directional: the far end reports "Link detected: yes" (it locks onto our TX)
  while our `pcsRx` stays 0 — **our receiver never locks**.

## Next step — the one experiment that would settle it

Capture EOS bringing Et2 **down→up** with `fmPlatformTraceRegOps` armed:

```
interface Ethernet2
  shutdown
  no shutdown
```

The "shut == cold" equivalence is already proven on this platform for Et1, so this yields exactly
the CR bring-up sequence our replay is missing — including whatever link-training step 10GBASE-CR
needs and 10GBASE-SR does not. Diff it against the `0x45`/`0x49` programming above.

Until then `fwd4.txt` stays canonical: it is the original artifact and the only one that has ever
produced a two-port link, even if only once.

---

## 2026-08-16: five measurements that change this document

### 1. ⛔ The lane is NOT marginal — measured, not inferred

EOS ships FM6000 SerDes diagnostics (`show interfaces <if> phy detail`, see `EOS-SOURCES.md`). With
all three ports linked:

```
            eyeScore  dfeCrse/Fine  eFifoErr
Et1 (SR)     0x2a40     0002/0002       0     always links
Et2 (CR)     0x3212     0002/0002       0     links ~50% under EdgeNOS
Et3 (SR)     0x3340     0002/0001       0
```

**Et2 has more eye margin than Et1**, the port that has never failed. DFE converged to the same
state. This refutes the physical readings of the problem — a weak signal, or equalisation failing to
converge. The copper lane is electrically healthy; ⚠ though note this measures **EOS's** bring-up,
so it shows what the lane *can* reach, not what ours achieves.

### 2. The rate is 5 of 10, not "intermittent"

Ten controlled boots, one arm, verified replay md5 and parser word each time, zero tainted:
**Et2 links on 5 of 10.** The experiment matrix above records much smaller samples ("unpaced 2/5,
genuinely paced 1/1"); this is the first properly counted figure, and it was taken **with the default
`PACE=1500000` already applied.**

### 3. The outcome is decided INSIDE the replay

`fm6000_fullreplay` now samples Et2 every 16k ops. Four independent good boots:

```
147456 ops  et2=0x0015/00000000   unprovisioned
163840 ops  et2=0x0815/00000000   SerXmit on, no lock     sbus=30752
180224 ops  et2=0x0cc0/00000940   LINKED                  sbus=30827
```

**Et2 links between op 163,840 and 180,224 — 75 SBus transactions.** It never climbs during the
settle loop; it arrives there already up or already dark.

### 4. ★ The successful path is deterministic

Those four good traces are **byte-identical**, including the SBus tally. So whatever differs on a
dark boot, **it is not the instruction stream** — the same writes are issued in the same order. That
is the strongest constraint this problem has: it rules out "a write is missing" and points at timing
or at the hardware's response.

### 5. ⚠ The instrument may perturb the result

The in-replay sampling adds one MMIO read per 16k ops. Et2 linked on **4 of 4** boots with it,
against 5 of 10 without. Fisher 4/4 vs 5/10 gives p = 0.126 — not significant, and 6/6 would still
only reach 0.058. But the direction is what the pacing hypothesis predicts, and it is the reason the
`PACE` experiment below is being run deliberately rather than read off an artifact.

## The `PACE` experiment, and why it is affordable

`PACE` is now settable from `/mnt/flash/pace.conf` (alpha14), so arms cost no rebuild.

Separating 50% from 90% needs ~49 boots per arm. But the hypothesis predicts an **extreme** in the
other direction — unpaced should fail nearly always — and a predicted zero is cheap: Fisher 0/7 vs
5/10 gives p = 0.041. **Running now: 7 boots at `PACE=2000`.**

⚠ A clean zero would actually contradict this document's own "unpaced 2/5". If links appear, the
older figure is being corroborated and the clean-zero story is the one that fails.

## What replaces "the one experiment that would settle it"

The `fmPlatformTraceRegOps` capture proposed above is still worth doing, but it is no longer the
only lead. **The 75-SBus-transaction window is a much narrower target**, and it can be compared
against EOS's own programming of device `0x45` without a new capture.
