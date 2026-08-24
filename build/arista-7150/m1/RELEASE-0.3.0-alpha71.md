# 0.3.0-alpha71 — the MAC-aging sweeper is not configuration: 1,280 writes dropped

`L2L` was 1,357 residual writes and 96% of them are `L2L_SWEEPER_*`. They are not a
table and they do not converge.

Of the 120 sweeper addresses:

    written the same value every time      25    configuration
    settle to a final value                 1
    still changing at the end              94    an engine, mid-walk

and the 94 do not drift toward anything — they **oscillate**:

    SWEEPER_CAM  ffffffff 0 ffffffff 0 ffffffff 0 ffffffff 0 ...
    SWEEPER_RAM  8000000 4000000 8000000 4000000 8000000 ...
    SWEEPER_IM   fffffffb 1 3 1 3 1 3 1 3 7 3 7 3 ...

That is EOS driving the MAC-aging sweeper as a command/data port and walking the
MAC table — software-driven table maintenance, captured while it happened to be
running. Replaying another machine's aging sweep is not configuration in any sense.

So: keep each of the 25 configuration writes **once**, drop all 1,280 writes to the
churning addresses.

| | alpha68 | alpha70 | **alpha71** |
|---|---|---|---|
| vendor data on flash | 185 KB | 177 KB | **152 KB** |
| vendor writes | 10,292 | 9,848 | **8,430** |
| stream covered by our code | 88.6% | 89.1% | **90.5%** |

**A 30× reduction from alpha63's 283,339 vendor writes.**

Measured over three boots:

    SCHEDULED: 47 generators run live, 8430 residual writes applied (0 problems)
    routes=45   ip route get 10.101.1.241 -> via 10.101.101.25 dev et1
    unicast THROUGH: 0% packet loss
    Et2: down, up, up (2/3 — within the 4/5 baseline for this marginal DAC link)

## ⚠ What this does not test

**MAC aging.** The sweeper's job is to remove stale entries. Dropping its captured
walk leaves the 25 configuration writes in place — `SWEEPER_TIMER_CFG`, the
interrupt mask, the RAM/CAM setup — so the block is configured, but nothing here
demonstrates that entries actually age out. A transit ping cannot show it: a switch
with a MAC table that never ages forwards perfectly until the table fills.

The honest claim is narrow: **these 1,280 writes are one machine's aging sweep, and
the box brings up and forwards without them.** Whether our own control plane needs
to drive the sweeper itself is a separate question, and it is open.

**Et2 at 2/3** is three boots. The baseline for this link is 4/5 and it has already
cost this project two wrong attributions, so 2/3 is "not a signal", not "fine".

## The rule that picked this target

`docs/FFU-RESIDUAL.md` declined FFU because its structure says only what the block
*contains*. ESCHED and now L2L were taken because their structure says what the
block *means* — a convergence in one case, a running engine in the other. Both
reductions came from reading the write **sequence**, not from compressing bytes:

    ESCHED   444 writes -> 12    "write the end state, not the walk toward it"
    L2L    1,280 writes ->  0    "this is an engine running, not a state to restore"

## What is left

    resid.txt      8,430 writes (152 KB)
      FFU          3,545   understood, deliberately not transcribed
      PARSER         970
      MOD            771
      HASH           314
      + others     2,830
    ucode_l2.raw     546 KB
    ucode_tail.raw   165 KB
    spico blob        12 KB   copper DAC only
