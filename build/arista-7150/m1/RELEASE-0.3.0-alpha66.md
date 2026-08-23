# 0.3.0-alpha66 — SBus scheduled, residual 16,432 → 12,711

`fm6000_sbusseq` now places all twelve of its interleaved segments, cutting the
vendor residual by exactly the SBus block.

    SCHEDULED: 32 generators run live, 12711 residual writes applied (0 problems)
    routes=44   fibd: programmed 14 route(s)
    unicast THROUGH : 0% packet loss

| | alpha63 | alpha64 | alpha65 | **alpha66** |
|---|---|---|---|---|
| vendor data on flash | 5.1 MB | 1.6 MB | 296 KB | **229 KB** |
| vendor writes | 283,339 | 90,396 | 16,432 | **12,711** |
| generator blocks live | — | 0 | 20 | **32** |
| stream covered by our code | — | 0% | 81.8% | **85.9%** |

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

## ⚠ CORRECTION — the Et2 claim in the first draft of these notes was wrong

That draft said scheduling SBus with 11 of 12 segments took **Et2 down in
silicon**, and that restoring the 20-block schedule "brought it straight back".
Both halves rested on a **single boot each**, and neither survives repetition:

| schedule | Et2 link, per boot |
|---|---|
| 20 blocks, SBus in residual | up, **down** |
| 32 blocks, SBus generated | down, down, down |

The 20-block schedule — the one credited with fixing Et2 — produced Et2 **down**
on a later boot with nothing changed. So **Et2 varies between boots regardless of
the schedule**, and the regression was attributed to a change that may have had
nothing to do with it.

Et2 is the **copper DAC** port, and this project already records that copper is
the one case that needs the SerDes firmware, while fiber links without it. A
marginal DAC link is the more likely explanation, and it was mistaken for a
regression because each arm was measured once.

⚠ **What this means for the numbers above:** the SBus schedule is not *shown* to
be harmful, but neither is it shown to be safe. Et2 is too noisy to attribute
anything on one boot per arm. Before this schedule is relied on, Et2 needs a real
measurement — the same schedule booted N times and the link rate counted — and
that has not been done.

⚠ **`carrier=1` on the netdev proves nothing.** During the first investigation the
kernel reported `et2 carrier=1` while `LANE_STATUS` was `0` and the port was dead
in silicon. That flag reflects the TAP, not the SerDes. Read `post-spico` and the
per-port `LANE_STATUS`.

⚠ **"It still forwards" is not a port check.** Forwarding survived every one of
these boots because the OSPF peer is on Et1. A test that only needs one of two
ports cannot tell you the other one died.

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
