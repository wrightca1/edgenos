# Residual reductions that are measured but not adopted

## `fm6000_mapperinit` — 361 writes — ADOPTED (alpha73)

The MAPPER block in the residual was 361 writes over 361 addresses, every one
written exactly once, and `fm6000_mapperinit` emits **exactly the same 361
`(address, value)` pairs** — identical as a set. It never placed because the two
differ in **order**, and `build_schedule.py` matches an exact ordered subsequence.

Substituting the generator's output at the position of the first MAPPER write takes
the residual **7,088 → 6,727** (128 KB → 121 KB).

It was held back at 1/3 on Et2 against 2/3 without. Both numbers were N=3 and both
were noise: measured properly at N=5 it is **5/5**, the same as the configuration
without it.

    SCHEDULED: 51 generators run live, 6727 residual writes applied
    post-spico et1=00000cc0/00000940  et2=000008c0/00000940
    routes=45   ip route get 10.101.1.241 -> via 10.101.101.25 dev et1
    unicast THROUGH: 0% packet loss

That is the second time an N=3 Et2 reading pointed the wrong way. The rule earns its
keep in both directions: it stopped a good change being adopted on bad evidence, and
five boots then showed the change was fine.

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
