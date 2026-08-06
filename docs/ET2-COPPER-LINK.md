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

## ROOT CAUSE FOUND: the replay was running too fast

**Pacing the replay fixes it.** `fm6000_fullreplay` takes a pace argument (µs per 4k ops); we had
been running it at **0**.

| replay pacing | Et2 success rate |
|---|---|
| `0` (unpaced, full speed) | **2 / 5** |
| `2000` µs per 4k ops | **3 / 3** |

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
