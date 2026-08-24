# Residual reductions that are measured but not adopted

## `fm6000_mapperinit` — 361 writes, ready, not shipped

The MAPPER block in the residual is 361 writes over 361 addresses, every one
written exactly once. `fm6000_mapperinit` emits **exactly the same 361
`(address, value)` pairs** — identical as a set, verified — so this is not an
authoring problem at all.

It never placed because the two differ in **order**. `build_schedule.py` matches an
exact ordered subsequence, and the generator emits its table in a different order
from the one the stream uses.

Substituting the generator's output at the position of the first MAPPER write, in
stream order, works and takes the residual from **8,430 to 8,069** writes
(152 KB → 145 KB):

    SCHEDULED: 48 generators run live, 8069 residual writes applied (0 problems)
    routes=45   ip route get 10.101.1.241 -> via 10.101.101.25 dev et1
    unicast THROUGH: 0% packet loss

**It is not adopted, because Et2 came up on 1 of 3 boots with it and 2 of 3
without** — and N=3 decides nothing here. The shipped configuration has since
measured **5/5** over five boots, so the bar is now a like-for-like five-boot
comparison rather than a comparison against an ambiguous number. Neither number is distinguishable from the 4/5 baseline at N=3 — see
`docs/ET2-LINK-RATE.md` — so this is 361 writes bought with an unmeasured risk to a
port. The substitution also moves writes that were scattered through the stream to a
single earlier point, and ordering is the one thing this project has repeatedly
established you cannot get wrong.

To adopt it: boot the 8,430 configuration five times and the 8,069 configuration
five times, same day, same cable, reading `post-spico` late. If Et2's rate is
unchanged, take the 361 writes.

## The sequence test, for whatever is next

For each address in a block, look at its list of values in order:

| shape | meaning | action |
|---|---|---|
| one value throughout | configuration | keep, write once |
| settles and is then rewritten | convergence | **author the end state** |
| oscillates and never settles | an engine running | **drop it** |
| genuinely different values, no pattern | a table | probably decline |

Applied so far:

    ESCHED_DRR_Q     444 -> 12   convergence
    L2L_SWEEPER_*  1,280 ->  0   oscillation
    FFU            3,545 -> --   table, declined (docs/FFU-RESIDUAL.md)

Remaining, with the test already run:

| block | writes | same-value | convergent | table |
|---|---:|---:|---:|---:|
| PARSER | 970 | 0 | **57** | 53 |
| MOD | 771 | 89 | 26 | 87 |
| HASH | 314 | 66 | 1 | 33 |
| SSCHED | 115 | 0 | 0 | 4 |
| CRM | 258 | 1 | 0 | 1 |

PARSER is the best remaining candidate: 57 of its 110 addresses are convergent, so
roughly half the block should reduce to its end state the way ESCHED did.

## The largest remaining opportunity, and why it is not simply taken

Running the sequence test over the **whole** residual, not one block at a time:

| shape | addresses | writes | if reduced | saving |
|---|---:|---:|---:|---:|
| same value throughout | 1,102 | 3,487 | 1,102 | **2,385** |
| convergent | 53 | 411 | 53 | **358** |
| genuine table | 720 | 3,190 | — | — |

**2,743 writes — 39% of the residual — look recoverable with no new generator at
all**, just by writing each idempotent value once instead of many times.

### ⚠ That figure is wrong, and the way it is wrong is the lesson

"Same value throughout" describes the **values**. It says nothing about whether the
writes are **separable**, and separability is what a collapse actually needs.

Test it directly: how many of those repeats are *back-to-back*, with nothing
interleaved between them?

    back-to-back identical writes (same address AND value, immediately repeated):
        9 of 7,088

Nine. And one of the nine is `FFU_ATOMIC_APPLY`, a commit strobe that must never be
collapsed. The real recoverable figure is **8 writes, 0.1%**, not 2,743.

The 292 `FFU_SLICE_CAM` addresses that hold one identical value across 1,354 writes
looked like the biggest prize. **290 of them have their repeats spread far across
the stream**, not adjacent — each re-write belongs to a different `ATOMIC_APPLY`
commit cycle. The FFU commits by quiescing `SLICE_MASTER_VALID`, writing, and
pulsing apply; collapsing the writes would leave later cycles committing data that
was written before the previous commit, which is not the same operation at all.

This is `alpha41` wearing different clothes. There, `SSCHED_INIT_TOKEN`'s 64
identical writes were collapsed to one, `--verify` passed because the final state
matched, and the scheduler broke because each write pushes a token. Here the final
state would also match. The state diff would also pass. And the chip would have
performed a different sequence.

**The rule that survives:** a repeated write is only removable if nothing happens
between it and its duplicate. Interleaving is the test, not equality of value. On
this residual that leaves essentially nothing, which means the remaining 7,088
writes are not padding — they are the sequence.

### The command ports, for the record

Two registers in the residual must never be collapsed under any circumstances:

    CRM_CTRL           1 address, 129 identical writes   indirect-access strobe
    FFU_ATOMIC_APPLY   1 address,  59 pulses             commit strobe

`CRM_CTRL`'s 129 writes are 129 distinct operations — load `CRM_REGISTER`, load
`CRM_COMMAND`, pulse `CRM_CTRL`. Collapsing performs one.
