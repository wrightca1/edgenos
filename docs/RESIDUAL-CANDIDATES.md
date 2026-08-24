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

## The largest remaining opportunity, and why it is not simply taken

Running the sequence test over the **whole** residual, not one block at a time:

| shape | addresses | writes | if reduced | saving |
|---|---:|---:|---:|---:|
| same value throughout | 1,102 | 3,487 | 1,102 | **2,385** |
| convergent | 53 | 411 | 53 | **358** |
| genuine table | 720 | 3,190 | — | — |

**2,743 writes — 39% of the residual — appear recoverable with no new generator at
all**, just by writing each idempotent value once instead of many times.

### ⚠ "Same value throughout" does not mean idempotent

This project has already made this mistake. `alpha41` collapsed
`SSCHED_INIT_TOKEN`'s 64 writes to one, `--verify` passed because the final state
was identical, and the scheduler was broken: each of those writes **pushes a
token**. Writing the same value twice is not always the same as writing it once.

The residual contains at least one clear instance:

    CRM_CTRL     1 address, 129 identical writes

That is the indirect-access engine's strobe. Its 129 writes are 129 distinct
operations — load `CRM_REGISTER`, load `CRM_COMMAND`, pulse `CRM_CTRL` — and
collapsing them to one would silently perform a single operation instead of 129.
`FFU_ATOMIC_APPLY`'s 59 pulses are the same shape.

Sorted by writes, the same-value addresses are:

    FFU_SLICE_CAM              292 addrs  1,354 writes   storage
    L2AR_CAM                    35 addrs    784 writes   storage
    HASH_LAYER2_KEY_PROFILE     64 addrs    204 writes   storage
    CRM_CTRL                     1 addr     129 writes   ⚠ STROBE
    MOD_VALUE_RAM               33 addrs    129 writes   storage
    MOD_CAM                     52 addrs    104 writes   storage

The distinction that matters is **storage versus command port**, and a register's
name is a hint rather than proof — `SSCHED_INIT_TOKEN` reads like storage.

### How to take it safely

Collapse only where a repeated write cannot have a side effect, and prove it per
register rather than by pattern:

1. Exclude every address whose register is a command port (`CTRL`, `COMMAND`,
   `APPLY`, `TOKEN`, `REQUEST`, `TRIGGER`, `VALID`) — that is `CRM_CTRL` and
   `FFU_ATOMIC_APPLY` at minimum.
2. For what remains, collapse in **one block at a time** and boot, rather than all
   at once: a blanket change that breaks something gives no information about
   which register broke it.
3. Check the state diff, not just forwarding. `asic/fm6000/fm6000_csrdump.c` plus
   a working-boot snapshot is the tool; a transit test cannot see a scheduler that
   is one token short.
4. Five boots for Et2 per `docs/ET2-LINK-RATE.md`.

Taken carefully this is the difference between 7,088 and roughly 4,300 residual
writes. Taken carelessly it reproduces alpha41, which passed every check the
project had at the time.
