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

## The open question

Et2's rate across successive residual reductions reads 4/5 → 5/5 → 2/3 → 1/3, but
the last two are under-sampled and the changes were not measured against each other
at equal N. It is possible the rate has genuinely drifted as writes were removed; it
is equally possible this is the same marginal link it always was.

Resolving it needs one configuration booted five times, then the next, on the same
day and the same cable — not three boots each, interleaved with other changes. Until
that is done, **a residual reduction should not be adopted on the strength of Et2
looking acceptable in three boots.** That is why `fm6000_mapperinit`'s 361-write
substitution is measured in `docs/RESIDUAL-CANDIDATES.md` and not shipped.
