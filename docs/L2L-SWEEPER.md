# The L2L sweeper: structure recovered

**2026-08-21.** Groundwork for authoring the L2L sweeper (`docs/BLOB-REMOVAL-PLAN.md`). The
block is 1,357 replay writes but only **144 distinct addresses** — the gap is the point, and it
changes what "authoring this" means.

## The register map (SDK descriptor table)

    0x0d000  SWEEPER_TIMER_CFG      w=2
    0x0d004  SWEEPER_TIMER_STATUS   w=2
    0x0d080  SWEEPER_CAM            w=4     20 slots
    0x0d100  SWEEPER_RAM            w=3     stride 4 (3 words used, 1 padding)
    0x0d200  SWEEPER_FIFO           w=1
    0x0d400  SWEEPER_FIFO_HEAD / _TAIL / _IP / _IM
    0x0d404  SWEEPER_WRITE_COMMAND  w=1
    0x0d408  SWEEPER_WRITE_DATA     w=4

⚠ `SWEEPER_RAM` is declared **w=3 but strides by 4** — three words used, one padding. Reading it
at stride 3 produces a plausible-looking dump in which a repeating value appears to shift
between entries. That is the tell for a wrong stride, and it is the same class of error as the
RAM3 stride in L3AR.

## CAM and RAM are not indexed alike — established from write order

The live CAM slots are the **odd** indices (1, 3, 5 … 19); the even ones are all-ones. RAM
entries are **contiguous** (0…14). So a naive `CAM[n] <-> RAM[n]` pairing is wrong.

The replay's write order settles it without guessing:

    RAM[0]                        <- written first, before any CAM
    CAM[0]  CAM[1]   -> RAM[1]
    CAM[2]  CAM[3]   -> RAM[2]
    ...
    CAM[18] CAM[19]  -> RAM[10]

**Each RAM entry serves a PAIR of CAM slots.** Within a pair the even slot is the all-ones
(universal) half and the odd slot carries the match, giving **10 live rules**, each with one RAM
action, plus `RAM[0]` as a default written ahead of everything.

## The RAM contents are highly regular

Fifteen entries hold six distinct values:

| entries | value |
|---|---|
| 0 | `24000000 ffffffd0 001003f4` |
| 1-2 | `00000000 bfffffc0 00000fff` |
| 3-4 | `0c000000 ffffffc0 00000ffc` |
| 5-7 | `08000000 ffffffc0 00000ffc` |
| 8-12 | `04000000 ffffffc0 00100ffc` |
| 13-14 | `04000000 ffffffc0 001003fc` |

## ⚠ Most of the 1,357 writes are OPERATIONS, not configuration

The tail of the block is a repeating cycle:

    IP (0xd402) -> IM (0xd403) -> WRITE_DATA x4 (0xd408-0xd40b) -> WRITE_COMMAND (0xd404)

That is an **indirect write port**: load four data words, then strobe the command register. It
is how MAC-table entries are pushed through the sweeper — the same shape as the SBus transaction
protocol, and the reason a single address (`0xd404`) accounts for many writes.

**So authoring this block splits in two:**

1. **Configuration — authorable.** `SWEEPER_TIMER_CFG`, the 20 CAM slots and 15 RAM entries,
   `FIFO_HEAD`/`IM`. About 110 addresses of real structure, six distinct RAM values, ten rules.
2. **The indirect writes — not configuration.** Replaying them means performing operations
   (pushing specific MAC entries), not programming a table. Whether we should perform them at
   all, rather than let the sweeper populate naturally, is a design question and not a
   transcription one.

## Still open before authoring

The CAM's field layout. The SDK field table yields the sweeper's *status* fields
(`SweeperIndex`, `SweepCount`, `Blocked`, `SweepComplete`, `FifoNonEmpty`, `SweeperReportMask`)
but the CAM/RAM entry layouts were not located. Without them the ten rules can be reproduced
bit-exactly but not *described*, which by the standing rule
([[edgenos-understanding-over-relocation]]) is relocation, not authoring. Find the CAM field
group first.
