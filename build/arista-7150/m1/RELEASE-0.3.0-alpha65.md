# 0.3.0-alpha65 — generators run live, and the vendor data is down to 296 KB

alpha64 got the dataplane up with no vendor replay, by replaying the
transformation's frozen output. It worked and it was not shippable: **no generator
ran**, and the file carried the vendor's values inline.

This keeps the order and throws the frozen values away.

    STEP5-ALT SCHEDULED: generators run live, residual interleaved by schedule
      SCHEDULED: 20 generators run live, 16432 residual writes applied (0 problem(s))
    routes=44  et1_rx=38
    unicast TO switch : 0% packet loss
    unicast THROUGH   : 0% packet loss

No `fwd4.txt`, no `fwd5.txt`, no `bringup.txt` on flash. Two consecutive boots.

## The schedule

Each generator's output appears in the executed stream as **one contiguous block**,
so the stream decomposes into an alternating sequence of "apply *n* residual writes"
and "run generator *G*". `asic/fm6000/tools/build_schedule.py` produces it:

    RES 2227
    GEN fm6000_tbl3init
    GEN fm6000_l3arinit
    GEN fm6000_l3arslice1
    ...
    GEN fm6000_l2arseq
    GEN fm6000_eplseq
    RES 121
    GEN fm6000_modports
    RES 1944
    GEN fm6000_cmwm
    RES 7879
    GEN fm6000_mapper
    RES 2773

**33 lines.** 20 generator blocks covering 73,964 of the stream's 90,396 writes
(81.8%); the remaining 16,432 are what no generator covers yet.

A schedule is an ordering of our own writes. It contains no vendor values.

## What the operator now needs

| | alpha63 | alpha64 | **alpha65** |
|---|---|---|---|
| vendor bring-up data on flash | `fwd4.txt` 5.1 MB | `bringup.txt` 1.6 MB | **`resid.txt` 296 KB** |
| vendor writes in the path | 283,339 | 90,396 (frozen, incl. ours) | **16,432** |
| our generators execute | yes | **no** | **yes, 20 of them** |
| forwards | yes | yes | yes |

**A 17× reduction in vendor data from alpha63, and our code is running again.**

## ⚠ What this still is not

`resid.txt` is 16,432 vendor writes. It cannot be redistributed, so an operator still
needs a licensed EOS **once** to produce it. This is not yet a blob-free OS — it is
the same claim as alpha64 with the provenance problem fixed and the vendor payload
cut by 5.4×.

⚠ Placement is deliberately conservative. A generator is scheduled **only** if its
entire `-n` output matches the stream exactly and contiguously; anything that does
not match is left in the residual rather than guessed at. A mis-placed block would
reorder the bring-up, and ordering is the one thing this exercise established you
cannot get wrong. That is why 20 blocks are placed and not 40.

⚠ Generators applied by direct MMIO — `coldreplay`, `initsbus`, `memfill`,
`l2linit`, `ffuinit`, `hashinit`, SPICO — are legitimately absent from the schedule.
`gen_drop` removed their lines from the stream and the sequence runs them separately,
before it. They still run.

⚠ The schedule is read on **fd 9**, not stdin. Generators and `fullreplay` are
ordinary programs and will eat stdin, which would swallow the rest of the schedule
and silently truncate the bring-up.

## What is left to remove, in order of size

    resid.txt          16,432 writes   the only vendor data in the bring-up path
      of which SBUS     3,721          SerDes protocol on 2 addresses
                FFU     3,545          includes the ATOMIC_APPLY commit strobes
                L2L     1,357          MAC-aging sweeper — runtime activity
                MOD       979
                PARSER    970
    ucode_l2.raw      546 KB           readable tables, 100% write-once
    ucode_tail.raw    165 KB           same
    spico blob         12 KB           only needed for copper DAC

99.8% of the residual is repeated writes to addresses already written, so what
remains is protocol rather than configuration. Each block needs a generator that
performs the sequence, not one that reproduces values — which is exactly what the
ERL and CM_PAUSE two-phase generators already do for their blocks.

## Build

    VERSION=0.3.0-alpha65 KERNEL=ex/linux-i386 BASE_INITRD=ex/initrd-i386 \
        sh ./build-release-swi.sh -o $PWD/edgenos-7150-0.3.0-alpha65.swi

To produce a schedule on a switch that has a replay: boot once, then

    python3 asic/fm6000/tools/build_schedule.py fwd-executed.txt --gens <dir of tool -n dumps> \
        --out-schedule schedule.txt --out-residual resid.txt

and copy both to `/mnt/flash`. `init-m1` prefers them over everything else.
