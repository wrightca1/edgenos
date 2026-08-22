# 0.3.0-alpha64 — the dataplane comes up with no vendor replay on flash

`fwd4.txt` and `fwd5.txt` are **not present**. The switch links both ports, forms an
OSPF adjacency, programs 14 routes into silicon, and forwards unicast to and through
itself at 0% loss. Verified on **two consecutive boots**, the second of which had a
replay-free boot before it, so this is not chip state carried over from a replay.

    confirmed: no fwd4.txt on flash
    [up]  t=8s  kernel routes=5  et1 rx=15
    fibd: programmed 14 route(s)
    routes=44  et1_rx=36
    unicast TO switch : 0% packet loss
    unicast THROUGH   : 0% packet loss

## What made it work: order, and only order

Standalone mode has always had the right *writes*. alpha59's 128,414-address
register diff proved it — after the `l2arpre` fix, the only configuration that
differed from a working boot was uninitialised RAM the working boot also has, plus
two link-membership bits. And it still would not forward.

The generator transformation the bring-up performs is **deterministic**: it filters
each generator's addresses out of the replay and splices that generator's output back
in at a specific point. So its *output* can be computed once and kept. That output is
already written to `/mnt/flash/fwd-executed.txt` on every replay boot — it has been
there all along, for an unrelated reason (mapping trace op numbers back to writes).

`PREBUILT=1` replays that stream verbatim: every `gen_*` helper returns immediately,
so no filtering and no splicing happen. `init-m1` selects it by preferring
`/mnt/flash/bringup.txt` over `fwd4.txt`.

**90,396 writes instead of 283,339**, and it works. The same writes batched by block
do not. That closes the standalone question: the difference was never missing state,
it was the interleaving — which is what the earlier `RESIDUAL_FIRST` experiment
suggested and could not prove.

⚠ `PREBUILT` deliberately does **not** disable the direct-MMIO blocks — `coldreplay`,
`initsbus`, `memfill`, `l2linit`, `ffuinit`, `hashinit`, SPICO. Those run outside the
transform and their writes are *not* in the stream, because `gen_drop` removed their
lines from it. Skipping them would leave exactly the tables the stream's own
`ATOMIC_APPLY` strobes expect to commit sitting uninitialised.

## ⚠ This is NOT the shipping fix, and the log says so

    [fs]   provenance: 0 of 90396 executed writes come from our generators (0%)

That line is accurate and it is the whole caveat. In `PREBUILT` mode **no generator
runs**. `bringup.txt` is a frozen snapshot of what our generators emitted, with the
vendor's uncovered writes interleaved — 13,035 of its pairs are residual vendor
writes. As a file it contains vendor values inline, so **it cannot be redistributed**,
and the provenance machinery correctly reports 0% because our code is not executing.

What this changes for an operator:

| | before | now |
|---|---|---|
| vendor replay needed | **every boot** | **once**, to produce `bringup.txt` |
| persistent artifact | 5.1 MB `fwd4.txt` | 1.6 MB `bringup.txt` |
| what we ship | neither | neither |

That is a real improvement — the licensed EOS is needed once rather than forever —
but it is a smaller claim than "ships working", and the two should not be confused.

## The actual shipping fix, now that the diagnosis is certain

The prize is generators running **live** in the right order, with only the residual
supplying vendor values. The transformation already knows that order; what standalone
does today is throw it away by batching generators back-to-back and applying the
residual as one block at the end.

So: derive an interleaving **schedule** from the transformation — for each step,
either "run generator G" or "apply residual writes *i..j*" — and have `run_standalone`
follow it. A schedule is an ordering of our own writes, not vendor content, and the
only vendor data left would be the residual's 13,391 writes.

This release proves that schedule is sufficient, because executing it is exactly what
`bringup.txt` does. It does not yet build one.

## Build

    VERSION=0.3.0-alpha64 KERNEL=ex/linux-i386 BASE_INITRD=ex/initrd-i386 \
        sh ./build-release-swi.sh -o $PWD/edgenos-7150-0.3.0-alpha64.swi

md5 `b8e6ac19a1dfa68f73d72a6b6c4ed350`, 19,018,430 bytes, verified on the switch.

To reproduce: on a switch that has a replay, boot once, then

    cp /mnt/flash/fwd-executed.txt /mnt/flash/bringup.txt
    mv /mnt/flash/fwd4.txt /mnt/flash/fwd4.txt.HELD
    mv /mnt/flash/fwd5.txt /mnt/flash/fwd5.txt.HELD

and reboot.
