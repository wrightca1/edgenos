# 0.3.0-alpha68 — generator slices: vendor data 229 KB → 185 KB

Three passes of placement instead of one, and a new tool that lets a generator's
output be delivered in pieces.

    SCHEDULED: 47 generators run live, 10292 residual writes applied (0 problems)
    post-spico et1=000008c0/00000940  et2=000008c0/00000940
    routes=41   unicast THROUGH: 0% packet loss

No `fwd4.txt`, `fwd5.txt` or `bringup.txt` on flash. Three boots, Et2 up on all
three.

| | alpha63 | alpha65 | alpha67 | **alpha68** |
|---|---|---|---|---|
| vendor data on flash | 5.1 MB | 296 KB | 229 KB | **185 KB** |
| vendor writes | 283,339 | 16,432 | 12,711 | **10,292** |
| generator blocks live | — | 20 | 32 | **47** |
| stream covered by our code | — | 81.8% | 85.9% | **88.6%** |

## What was still vendor data, and why

Splitting the residual by whether *any* generator already produces the write:

    covered by a generator that did not place : 4,748  (37%)
    no generator at all                       : 7,963  (63%)

The first group is not an authoring problem, it is a **placement** problem — our
code already emits those writes, byte for byte, and the scheduler could not find a
home for them.

The clearest case is `fm6000_l2arseq`. The bring-up splices its 29,110 writes
before the port loop, and then writes the last 3,684 of them — the in-loop bursts —
**again**, inside the loop. The generator is fully placed already, so nothing was
looking for a second home for its tail, and those 3,684 sat in the residual as
vendor data while our own generator produced them at `l2arseq[25426..29109]`.

## `fm6000_slice`

Rather than teach thirty generators a new flag, `fm6000_slice <tool> <first>
<count>` runs any of them in dry-run mode and applies one window of the result.
Values are still computed by our code at run time; only the delivery window changes.

⚠ It **refuses any window containing an SBus address**. `fullreplay` drives those as
transactions with a completion poll, and replaying them literally corrupts the bus —
which is precisely what took Et2 down 5 boots out of 5 in alpha66. Those blocks use
`fm6000_sbusseq`, which knows the handshake.

⚠ It applies **all or nothing**: if the child's output is shorter than the requested
window, it reports and exits rather than writing a partial slice.

## Three passes, and why the order matters

1. **Whole-block** placement for every candidate, longest first.
2. **Split generators** — cover one generator's output with several runs.
3. **Gap recovery** — cover a leftover run with a slice of a generator that is
   already placed elsewhere. This is the pass that finds l2arseq's second copy.

Pass 1 runs for everything before pass 2 or 3 can claim anything, so a
decomposition can never steal a position a generator would have matched outright.

## ⚠ MIN_RUN is a cost knob, and the first setting was wrong

A short run of writes is not evidence of placement — short sequences repeat all over
a bring-up stream. At `MIN_RUN=8` the gap pass "recovered" 199 runs, but 184 were
under 32 writes and **183 of them re-ran `fm6000_l2arseq`, whose output is 29,110
lines**, for a few writes each: 5.3M lines generated and discarded at boot, and
coverage went *down* from 85.9% to 85.3% because the fragments displaced real
matches.

Measured on this stream:

| MIN_RUN | slices | residual |
|---|---|---|
| 32 | 15 | **10,292** ← chosen |
| 64 | 11 | 10,533 |
| 128 | 7 | 10,842 |
| 256 | 3 | 11,587 |

## ⚠ Reading Et2 too early gives a false negative

Two of the three boots here read `LANE_STATUS = 0` about a minute after boot and
`1` a few minutes later, with `post-spico` showing `0x940` both times. **The link
is still training when the box first answers ssh.** Any Et2 measurement must be
taken late, and earlier per-boot figures in this project were only sound because
they happened to be read four minutes in.

## What is left

    resid.txt      10,292 writes (185 KB)
      FFU           3,545   includes the ATOMIC_APPLY commit strobes
      L2L           1,357   MAC-aging sweeper, runtime activity
      PARSER          970
      MOD             771
      ESCHED          444   12 addresses, 37 writes each
      HASH            314
    ucode_l2.raw     546 KB   readable tables, 100% write-once
    ucode_tail.raw   165 KB   same
    spico blob        12 KB   copper DAC only

7,963 of those writes have **no generator at all** and need authoring, led by FFU.
`fullreplay` treats every one of them as a plain write — it special-cases only SBus
and the `@SPICO_IMEM` marker — so the trace-versus-behaviour trap that cost alpha66
does not apply to what remains.
