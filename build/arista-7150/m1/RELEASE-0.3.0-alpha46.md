# 0.3.0-alpha46 — the egress scheduler, authored as a rule

**104 writes over 104 addresses.** Provenance **120,042 of 124,889 = 96.1%**
(alpha45: 96.0%) — exactly +104.

## The first generator with no value table

Every other generator in this tree carries EOS-derived values, however well
structured. This one does not. The whole block is one rule:

    for each of the 52 front-panel data ports:
        ESCHED_CFG_1[port] = 0xffffff
        ESCHED_CFG_2[port] = 0xffffff

`ESCHED_CFG_1` (0x2000) and `ESCHED_CFG_2` (0x2080) are 76 × 1 word by the SDK
geometry. The replay writes 52 of each, and **all 104 values are 0xffffff** — 24
bits all set, the scheduler credit field at maximum: every front-panel port
unshaped and equal. A value table here would have been 104 lines of noise
obscuring a one-line rule.

### The port set is derived, and checked both ways

The 52 written ports are **exactly the configured-port set minus {0, 1, 3}** —
the CPU and management ports. That is what an egress scheduler should do, and the
replay confirms it independently: port 0 is written with `0xfff800`/`0xfff000`,
a *different, shaped* value, not the uniform one.

So `FRONT_PANEL` is computed from `ACTIVE_PORTS - {0,1,3}`, and `--verify` tests
the derivation in **both directions**:

- no address the rule emits that the replay never wrote (`absent 0`)
- no ESCHED address carrying our value that the rule failed to emit (`unclaimed 0`)

If the hypothesis about which ports are front-panel were wrong, that check fails.
A value table cannot fail that way — it would simply encode the mistake.

## Measured

`--verify`: `identical 104, differing 0, absent 0, unclaimed 0`. `--counts` clean.
Both ports clean-lock `000008c0`/`0940`.

⚠ ESCHED is the egress scheduler — a wrong credit value shows up as loss under
load, not as a failed transit test. Validated against the EOS reference:

| | EOS 4.16.8M | alpha46 |
|---|---|---|
| paced 2 ms | 0.25% | 0.30 / 0.30 / 0.10% |
| unpaced burst | 41.5% | 41.95 / 42.15 / 41.30% |

Transit passes with MAC rewrite and TTL decrement; 44 kernel routes.

⚠ **Port 0 is deliberately not ours.** The replay writes it 37 times (CFG_1) and
61 times (CFG_2), always the same shaped value. The repeats are idempotent so
collapsing them would be harmless, but claiming the address would make this
generator own the CPU port's scheduling too. Out of scope; it stays in the replay.

md5 `78f44bd0a080a7461f84f4f83e8a1b4c`, verified on the switch.
