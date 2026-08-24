# Et2's link rate is the limiting instrument, not a pass/fail

Et2 is the copper DAC port. It links on some boots and not others, and this project
has now made **three** wrong calls about it — twice attributing a change to a code
edit, once retracting an attribution that was actually correct. This records what is
known so the next person does not make a fourth.

## The measurements, in order

| configuration | Et2 up | N |
|---|---|---|
| SBus in the residual, file-driven (alpha65) | 4/5 | 5 |
| SBus generated, **no completion poll** (alpha66) | **0/5** | 5 |
| SBus generated, **with completion poll** (alpha67) | **5/5** | 5 |
| sweeper churn dropped (alpha71) | 2/3 | 3 |
| + mapperinit substituted in stream order | 1/3 | 3 |

The alpha66/67 pair is the only one that is conclusive: 0/5 against 5/5 for a
one-line change, with a mechanism (the SBus busy-bit poll) that explains it.

Everything after that is N=3, and 2/3 and 1/3 are **not** distinguishable from a 4/5
baseline at that sample size. They are not evidence of harm and not evidence of
safety.

## What that means in practice

**Five boots per arm, or no claim.** At N=1 this project reported a regression, then
retracted it, then found the original report had been right — each step reversing on
one new observation. At N=3 the numbers still move around. Only N=5 has ever settled
anything here.

**Read the link late.** `LANE_STATUS` was `0` about a minute after boot and `1` a few
minutes later on the same boot, with `post-spico` showing `0x940` both times. The
link is still training when the box first answers ssh.

**Read `post-spico` and `LANE_STATUS`, not the netdev.** The kernel reported
`et2 carrier=1` while `LANE_STATUS` was `0` and the port was dead in silicon. That
flag reflects the TAP, not the SerDes. Et1 is `0xe3826`, Et2 is `0xe4026`; a dead
Et2 reads `PORT_STATUS = 0x0815`, a live one `0x08c0`/`0x0cc0`.

**A transit test does not check Et2.** The OSPF peer is on Et1, so unicast through
the box is 0% loss whether Et2 is up or down. Forwarding passing says nothing.

## The question, answered

Et2's rate across successive reductions read 4/5 → 5/5 → 2/3 → 1/3, and the middle
of that looked like a drift. It was not. The shipped configuration at **7,088
residual writes — 25× fewer than the replay it replaced — measures 5/5**, the same
as alpha67 and better than the file-driven baseline.

So the 2/3 and 1/3 readings were N=3 noise, exactly as they were labelled at the
time. Nothing in the reductions from 16,432 down to 7,088 writes has hurt this link.

**The rule stands anyway.** It cost five boots to establish that two earlier
measurements meant nothing, which is the point: at N=3 this link cannot distinguish
a real regression from a run of bad luck, and it has already produced three wrong
calls in this project. `fm6000_mapperinit`'s substitution stays in
`docs/RESIDUAL-CANDIDATES.md` until it gets the same five boots — its 1/3 is no more
meaningful than the 2/3 it was compared against, and that cuts both ways.
