# 0.3.0-alpha47 — the egress rate limiter, sequence and all

**1,934 writes over 967 addresses** — the largest single generator in the tree.
Provenance **121,976 of 124,889 = 97.7%** (alpha46: 96.1%), exactly +1,934.

## The first block with no write-once part at all

Every one of ERL's 967 addresses is written **exactly twice**, 636 of them with
two different values. The authorability census put ERL at **zero** write-once
addresses, and `gen_list` in its collapsing form is unusable here — taking the
final value would drop 967 writes on the floor and silently change the protocol.

It is authorable anyway, because the two writes *are* a protocol and the protocol
is small:

    phase 1:  ERL_CFG[port][tc] = 0x40001000    all 76 ports x 12 classes
    phase 2:  ERL_CFG[port][tc] = 0x80001000    the 52 front-panel ports
              ERL_CFG[0][tc]    = per-class rate for the CPU port
              (the other 23 ports get 0x40001000 again)

Bit 30 in phase 1, bit 31 in phase 2: every entry is parked in one state, then
the ports that carry traffic are moved to the other. Verified exhaustively —
**all 912 first writes are 0x40001000**, all 624 front-panel second writes are
0x80001000, all 276 second writes to the other 23 ports are 0x40001000 again.

**Only the CPU port needs a table.** Phase 2 holds just 6 distinct values across
912 entries and 624 of them are the one constant. The sole per-entry data in the
block is port 0's 12 traffic classes, which carry real per-class rates — the CPU
port is shaped where the front-panel ports are not. That is the same split
`gen_esched.py` found, reached independently.

`ERL_CFG_IFG` = `0x14` for the 55 configured ports, also written twice — identical
values, so order is irrelevant there, but the *count* still matters.

## `--counts` now checks the value SEQUENCE

Previous generators checked that our write count per address matched the replay.
That is not sufficient for a protocol: two writes of the right count can still be
the wrong two values in the wrong order. `gen_erl.py --counts` compares the full
per-address value sequence against the replay, and passes.

## ⚠ A real behavioural change, deliberately taken

In the replay phase 1 begins near line 9,559 and phase 2 lands near 67,000.
`gen_list_early` splices the tool at the block's first write, so phase 1 stays
where EOS put it and **phase 2 moves ~57,000 writes earlier**. End state is
identical and per-address phase order is preserved; what changes is that the real
rate limits install sooner — the safe direction, but a change nonetheless.

That is why this was validated under load rather than by a ping:

| | EOS 4.16.8M | alpha47 |
|---|---|---|
| paced 2 ms | 0.25% | 0.20 / 0.20 / 0.35% |
| unpaced burst | 41.5% | 41.60 / 41.35 / 42.15% |

No regression. Both ports clean-lock `000008c0`/`0940`; transit passes with MAC
rewrite and TTL `0x3f`.

## Where this leaves the campaign

97.7% of executed writes now come from our generators. What remains:

| block | writes | why it is not this technique |
|---|---:|---|
| FFU | 3,813 | the `--lift-multi` decision — declined, relocation not authoring |
| SBUS | 3,721 | 4 addresses = an indirect port; the port-3 problem, not a table |
| L2L | 1,357 | blocked on the CAM/RAM entry field layouts |
| MOD | 979 | 306 addrs, all multi-write, 66 monotonic |
| SAF | 339 | monotonic accumulating bitmap — belongs in the replay |
| HASH | 314 | 100 addrs, all multi-write |
| PARSER rest | ~970 | 109 of 110 monotonic — belongs in the replay |

md5 `93f60505728b323f247f3a9c32aafcef`, verified on the switch.
