# 0.3.0-alpha66 — SBus authored and schedulable; NOT shipped, because it costs Et2

`fm6000_sbusseq` reproduces the whole 3,721-write SBus block byte-identically, and
`build_schedule.py` now places all twelve of its interleaved segments — 85.9%
coverage, residual down to 12,711 writes. **That schedule is not shipped.** Measured
over five boots per arm it takes the copper DAC port down 5 times out of 5.

Shipped configuration, verified this release:

    SCHEDULED: 20 generators run live, 16432 residual writes applied (0 problems)
    post-spico et1=000008c0/00000940  et2=000008c0/00000940
    routes=43   fibd: programmed 14 route(s)   unicast THROUGH: 0% packet loss
    Et1 LANE=1  Et2 LANE=1

| | alpha63 | alpha64 | alpha65 | **alpha66** |
|---|---|---|---|---|
| vendor data on flash | 5.1 MB | 1.6 MB | 296 KB | **296 KB** |
| vendor writes | 283,339 | 90,396 | 16,432 | **16,432** |
| generator blocks live | — | 0 | 20 | **20** |
| available but unshipped | — | — | — | **12,711 / 32 blocks / 85.9%** |

## Place longest blocks first

Placing in name order let a short, highly repetitive segment land *inside* a
longer block's span and block it. `fm6000_sbusseq` segment 7 is 54 writes that
occur **12 times** in the stream, and one occurrence sits inside segment 8 — 540
writes occurring exactly **once**. Name-order placement took the short one first,
and segment 8 could then never be placed.

Longest-first fixes it: a long block claims its span before a short one can squat
in it. 31 blocks → 32, and every generator is now all-or-nothing.

`build_schedule.py` also now **names what it could not place** and refuses to stay
silent about a partially placed generator:

    ⚠ PARTIALLY PLACED, DO NOT SHIP THIS SCHEDULE: <tool>

## ⚠ Et2: measured, and the SBus schedule is NOT shipped

This went through two wrong answers before a measurement settled it, and the
sequence is worth keeping.

**Round 1**, one boot per arm: scheduling SBus with 11 of 12 segments left Et2
down; restoring the 20-block schedule brought it back. Reported as a regression.

**Round 2**, one more boot per arm: the 20-block schedule — the one credited with
fixing Et2 — produced Et2 **down**. That looked like proof the link simply varies,
so the regression claim was retracted.

**Round 3**, five boots per arm:

| schedule | Et2 link | |
|---|---|---|
| 20 blocks, SBus in the residual | up, up, down, up, up | **4/5** |
| 32 blocks, SBus generated | down, down, down, down, down | **0/5** |

4/5 against 0/5 is p ≈ 0.02 by Fisher's exact test. **Scheduling the SBus
generator does break Et2.** The round-1 claim was right; the round-2 retraction was
wrong.

Both mistakes have the same shape: a conclusion drawn from one or two observations
of a signal that is genuinely noisy. The retraction was no better evidenced than
the claim it replaced — it just happened to be louder about uncertainty. Et2 is a
marginal copper DAC link and needs **N boots per arm**, not one.

**So the shipped schedule stays at 20 blocks / 16,432 residual writes** (296 KB),
where Et2 works 4 times in 5. `fm6000_sbusseq` is built into the image and proven
byte-identical, but is not in the schedule. The 12,711-write schedule that gave
85.9% coverage is real and reproducible — `build_schedule.py` will emit it — and it
is not shipped, because it costs the DAC port.

**Why it breaks Et2 is not established.** The generator's writes are byte-identical
to the residual's, so it is not content. The remaining difference is how they are
delivered: `fm6000_fullreplay` walks a file, while the generator walks tables in a
process that starts and exits per segment. SBus is a serial bus and SerDes bring-up
is timing-sensitive, so delivery timing is the obvious suspect — untested.

⚠ **`carrier=1` on the netdev proves nothing.** The kernel reported `et2 carrier=1`
while `LANE_STATUS` was `0` and the port was dead in silicon. That flag is the TAP.
Read `post-spico` and per-port `LANE_STATUS` — Et1 `0xe3826`, Et2 `0xe4026`.

⚠ **"It still forwards" is not a port check.** Forwarding survived all ten boots
because the OSPF peer is on Et1. A test that needs one of two ports cannot tell you
the other died.

## The SBus generator

3,721 writes on four addresses, reproduced **byte-identically** from 320 steps in
7 program units. The transaction is `REQUEST <payload>` / `COMMAND 0` /
`COMMAND 01.DD.RR.VV`, and 1,239 transactions use only **13 distinct
(device, register) pairs**:

    1. one setup transaction, 21.fe = 0a
    2. 84 x an eight-transaction toggle on device 0x22, registers 01-04,
       each written 00 then 0f -- no parameter varies across the 84 iterations
    3. a short preamble on 21.fd / 22.fd
    4. ~50 x a nine-transaction unit alternating targets 0x45 and 0x49
       with REQ alternating 0e / 16

⚠ Compress by **sequence periodicity, not run length**. The toggle alternates, so
plain RLE collapsed 1,239 transactions into 1,182 "steps" — transcription with
extra ceremony.

⚠ The **structure** is recovered exactly; the register **semantics** are not. The
SDK's SBus field table names registers 0x00–0x35, but these writes address devices
whose register space does not line up with it, and `0xfd`/`0xfe` are outside it
entirely. This is an honest transcription of a *program* rather than of a data
table — better than 3,721 literals, and short of authorship from intent.

## What is left

    resid.txt      12,711 writes (229 KB)   the only vendor data in the path
      FFU           3,545                   includes the ATOMIC_APPLY strobes
      L2L           1,357                   MAC-aging sweeper, runtime activity
      MOD             979
      PARSER          970
    ucode_l2.raw     546 KB                 readable tables, 100% write-once
    ucode_tail.raw   165 KB                 same
    spico blob        12 KB                 copper DAC only
