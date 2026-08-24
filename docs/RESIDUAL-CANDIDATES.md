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
without.** Neither number is distinguishable from the 4/5 baseline at N=3 — see
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
