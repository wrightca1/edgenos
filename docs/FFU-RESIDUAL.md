# The FFU residual — decoded, and deliberately not transcribed

FFU is the largest block in the residual with no generator: **3,545 writes**, 34% of
what is left. This is what it is, and why authoring it is not the next step.

## Structure

    FFU_SLICE_CAM            2,480 writes   508 addrs   127 distinct values
    FFU_BST_ACTION             282           120         18
    FFU_SLICE_SCENARIO_CAM     280           118         26
    FFU_SLICE_SCENARIO_CFG     252           110         36
    FFU_SLICE_ACTION            88            18          9
    FFU_BST_KEY                 60            23         23
    FFU_ATOMIC_APPLY            59             1          2
    + 5 smaller registers       44

**3,545 writes, 234 distinct values.**

`FFU_SLICE_CAM` decodes as **127 entries across 17 slices** — slices 1 and 2 hold 31
each, 12–15 hold 10 each, the rest single figures. Writes are entry-major: all four
words of one entry, then the next.

Each entry is revisited, and 99 of the 127 are revisited **only to toggle bit 0 of
words 0 and 2** — the ternary valid bit, written invalid-then-valid. The remaining
28 carry genuinely different values between visits, so this is not a uniform
idiom that a loop can express.

`FFU_ATOMIC_APPLY` pulses `1` and `2` alternately — two commit domains — always
preceded by `SLICE_MASTER_VALID` / `BST_MASTER_VALID` going to zero. That is the
commit protocol: quiesce, write, apply.

## Why it does not compress

Sequence-periodicity compression, the technique that turned SBus's 3,721 writes into
320 steps, achieves **nothing** here: 3,545 writes → 3,545 distinct slots, every unit
with a repeat count of 1. SBus was a loop. FFU is a table with real content.

## Why it is not being transcribed

Measured against the two things that matter:

| | |
|---|---:|
| FFU writes also present verbatim in the vendor microcode | **0** |
| FFU writes carrying this deployment's IP addresses | **67** |
| trivial fill (`0x00000000` / `0xffffffff`) | 427 |
| distinct non-trivial vendor-derived pairs it would add to this repo | **~1,417** |

Transcribing it would **not** worsen the microcode-overlap figure that governs
redistribution — that is genuinely zero. It would add roughly 1,417 vendor-derived
pairs as TABLE-grade content, and `docs/PROVENANCE.md` already calls TABLE "the
weakest keep".

And it would move **67 writes carrying the generating switch's own addressing** —
its management IP, the transit network, a public /24 — into a repository that has
been deliberately scrubbed of addresses. Those cannot be committed; they are
deployment configuration and have to come from the operator either way.

So the trade on offer is: move ~64 KB off the operator's flash, in exchange for
~1,417 vendor values in our source and a hole where 67 site-specific ones used to
be. That is relocation, not authorship, and this project's standing rule is not to
trade understanding for a smaller blob.

## What authoring it would actually require

The same thing `l3arslice1` needed: read the rules and write what they *mean*. For
FFU that means the key format, and it is not established. `FFU_SLICE_SCENARIO_CFG`
selects key sources with `ByteMux_0..3` (6-bit selectors) and `Top4Mux`, and across
the 151 populated scenarios only 16 selectors are used — but three of them (r53,
r58, r60) are outside the r1..r42 range the parser writes, so the selector space is
not just parser registers, and the mapping is unknown. See `docs/PARSER-PROGRAM.md`.

Until that is resolved, 127 FFU rules can be read as bit patterns and not as
policy, and a generator for them would be a transcription wearing a loop.

## Where that leaves the residual

    10,292 writes (185 KB)
      FFU        3,545   understood, deliberately not transcribed (this document)
      L2L        1,357   MAC-aging sweeper — runtime activity, arguably belongs here
      PARSER       970
      MOD          771
      ESCHED       444   12 addresses, 37 writes each — a two-phase protocol
      HASH         314
      + others   2,891

`ESCHED` is the most SBus-like of what remains: 12 addresses carrying 444 writes.
That ratio is a protocol, and protocols have been the ones worth authoring.
