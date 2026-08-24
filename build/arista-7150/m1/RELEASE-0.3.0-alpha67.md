# 0.3.0-alpha67 — SBus ships: vendor data 296 KB → 229 KB, and Et2 goes 0/5 → 5/5

alpha66 authored the SBus block byte-identically and could not ship it: scheduling
it took the copper DAC port down on **5 boots out of 5**. The cause was in this
repository the whole time.

## `fullreplay` never replayed SBus literally

    if (a == 0xF002) { pend = v; continue; }              /* stash, no write */
    if (a == 0xF001) { if (!v) continue; sbus(v, pend); } /* transaction */

and `sbus()` writes the three words, then **polls `SBUS_COMMAND` until busy —
bit 25 — clears**, up to 200,000 reads.

The generator emitted the captured writes faithfully and never waited. On a serial
bus, issuing the next transaction before the previous one completes corrupts it,
and SerDes bring-up is exactly where that shows.

**Reproducing a captured trace is not the same as reproducing the behaviour that
produced it.** The trace contains writes the vendor's own replay engine
deliberately does *not* perform — the `COMMAND 0` idle write is consumed, not
issued — and it lacks the poll entirely, because a poll is a read.

Adding `sbus_wait()` after each transaction is the whole fix.

## Measured, five boots per arm

| arm | Et2 link |
|---|---|
| 20 blocks, SBus left in the residual (file-driven) | 4/5 |
| 32 blocks, SBus generated, **no handshake** | **0/5** |
| 32 blocks, SBus generated, **with handshake** | **5/5** |

The dry-run output is unchanged at 3,721 writes, so this is not a change of
content — only of when the next write is allowed to happen.

## Shipped

    SCHEDULED: 32 generators run live, 12711 residual writes applied (0 problems)
    post-spico et1=000008c0/00000940  et2=000008c0/00000940
    routes=43   fibd: programmed 14 route(s)   unicast THROUGH: 0% packet loss

No `fwd4.txt`, no `fwd5.txt`, no `bringup.txt` on flash.

| | alpha63 | alpha64 | alpha65 | alpha66 | **alpha67** |
|---|---|---|---|---|---|
| vendor data on flash | 5.1 MB | 1.6 MB | 296 KB | 296 KB | **229 KB** |
| vendor writes | 283,339 | 90,396 | 16,432 | 16,432 | **12,711** |
| generator blocks live | — | 0 | 20 | 20 | **32** |
| stream covered by our code | — | 0% | 81.8% | 81.8% | **85.9%** |
| Et2 | — | — | 4/5 | 4/5 | **5/5** |

**22× less vendor data than alpha63, and the DAC port is more reliable than the
file-driven replay it replaced.**

## Two device numbers now have names

`docs/PORT3-BRINGUP.md` names SBus device **`0x45` as Et2's EPL16 lane and `0x49`
as Et1's**. That explains the shape recovered in alpha66: the ~50-iteration unit
alternates between `0x45` and `0x49` because it is walking the two ports.

The rest of the register semantics are still not established — the SDK's SBus field
table covers registers `0x00`–`0x35`, and these devices' space does not line up
with it, with `0xfd`/`0xfe` outside it entirely.

## What is left

    resid.txt      12,711 writes (229 KB)   the only vendor data in the path
      FFU           3,545                   includes the ATOMIC_APPLY strobes
      L2L           1,357                   MAC-aging sweeper, runtime activity
      MOD             979
      PARSER          970
    ucode_l2.raw     546 KB                 readable tables, 100% write-once
    ucode_tail.raw   165 KB                 same
    spico blob        12 KB                 copper DAC only

⚠ The lesson generalises to every remaining block. FFU's 3,545 writes include the
`ATOMIC_APPLY` strobes at `0x3f0000`, which are a commit protocol, and `fullreplay`
may well interpret those too. **Before authoring any block, read what `fullreplay`
does with its addresses** — the captured trace is not the specification.
