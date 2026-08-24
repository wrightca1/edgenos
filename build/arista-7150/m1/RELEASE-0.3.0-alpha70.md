# 0.3.0-alpha70 — the DRR quanta authored: 444 writes become 12

`ESCHED_DRR_Q` is twelve registers, one per traffic class, each holding a single
field — `q[24]`, the deficit quantum. Twelve numbers.

The vendor replay spends **444 writes** on them, 37 per address, all in one
contiguous run. Read in order they are not configuration but **convergence**:

    round  1      all twelve 0xa00
    round  2      TC11 -> 0x7d0
    round  3      TC10 -> 0x3e8
    ...           one class changes per round, TC11 down to TC0 -> 0x100,
                  then back up TC0 to TC8 -> 0x1450
    round 25      TC11 -> 0x39d0        <- settled
    rounds 26-37  the settled state, rewritten unchanged thirteen times

That is a control plane applying a queue configuration incrementally as it
discovers ports. Nothing observes the intermediate states.

`fm6000_eschedrr.c` writes the settled state and nothing else:

    TC0-2, TC4, TC6-8   0x1450  =  5,200
    TC3, TC5            0x05c8  =  1,480
    TC9, TC10           0x6590  = 26,000
    TC11                0x39d0  = 14,800

Verified on hardware — the readback after boot is the authored value, not a
survivor of some replayed sequence:

    DRR readback TC0/3/9/11: 00001450 000005c8 00006590 000039d0
    routes=45   ip route get 10.101.1.241 -> via 10.101.101.25 dev et1
    unicast THROUGH: 0% packet loss   et1 LANE=1  et2 LANE=1

| | alpha67 | alpha68 | **alpha70** |
|---|---|---|---|
| vendor data on flash | 229 KB | 185 KB | **177 KB** |
| vendor writes | 12,711 | 10,292 | **9,848** |
| generator blocks live | 32 | 47 | **48** |
| stream covered by our code | 85.9% | 88.6% | **89.1%** |

## ⚠ A ping does not test this

A DRR quantum controls how much each class may send per scheduling round. Getting
one wrong shows as **unfairness under load**, not as a failed ping, so the transit
result above says nothing about whether these twelve numbers are right — only that
they do not break connectivity. The check that would settle it is
`tools/load-test.sh` with traffic on several classes at once, and **it has not been
run**.

What *is* established is narrower and worth stating exactly: the settled state the
replay reaches is byte-identical to what this generator writes, on all twelve
registers. The claim being authored is that **only the settled state matters** —
that the 432 writes of convergence are a by-product. That claim is not proven by a
transit test either.

## Why this one was worth authoring when FFU was not

`docs/FFU-RESIDUAL.md` declined to transcribe FFU: it is a table of 127
deployment-specific classifier rules whose key format is not established, and
moving it into our source would have added ~1,417 vendor values and 67 site
addresses to the repository.

ESCHED is the opposite shape. Twelve registers, one named field, and a sequence
that is *visibly* a convergence rather than a configuration. Authoring it adds
**twelve** vendor numbers and removes 444 writes, and the reasoning — "write the
end state, not the walk toward it" — is a statement about the block rather than a
compression of its bytes.

That is the distinction worth carrying into what is left: author where the
structure tells you what the block *means*, decline where it only tells you what
the block *contains*.

## What is left

    resid.txt      9,848 writes (177 KB)
      FFU          3,545   understood, deliberately not transcribed
      L2L          1,357   MAC-aging sweeper -- runtime activity
      PARSER         970
      MOD            771
      HASH           314
      + others     2,891
    ucode_l2.raw     546 KB
    ucode_tail.raw   165 KB
    spico blob        12 KB   copper DAC only
