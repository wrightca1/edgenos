# 0.3.0-alpha72 — the parser seed authored: 970 writes become 110

`PARSER_INIT_FIELDS` holds the per-port seed the parser starts every frame from —
two entries of two words per port. `fm6000_parserfields` already wrote entry 1
(zero for all 76 ports, disabling it) and entry 0 for the 21 ports this board does
not use. **Entry 0 for the 55 ports it does use came from the replay: 970 writes**,
because the control plane rebuilds the seed each time it brings a port up.

Read as an end state, entry 0 is two words and one rule:

    word0 = glort << 16 | (front_panel ? 0x0100 | lane : 0)
    word1 = lane  << 16 | 0x0001

**Verified: 55 of 55 ports reproduce exactly**, and the readback after boot is the
authored value —

    PARSER_INIT_FIELDS port 20:  03ee0102 00020001

`0x03ee` is et2's GLORT, matching what `portd` assigns. So 970 writes carry
55 ports × 3 numbers of real information.

| | alpha70 | alpha71 | **alpha72** |
|---|---|---|---|
| vendor data on flash | 177 KB | 152 KB | **128 KB** |
| vendor writes | 9,848 | 8,430 | **7,088** |
| generator blocks live | 48 | 47 | **50** |
| stream covered by our code | 89.1% | 90.5% | **91.9%** |

**40× fewer vendor writes than alpha63's 283,339.**

Verified: 50 generators, 7,088 residual, `routes=45`, `ip route get 10.101.1.241`
→ `via 10.101.101.25 dev et1`, unicast through at 0% loss, both ports `LANE=1`.

## What the 55 numbers are

Only `lane` is genuinely new content — the board's port-to-SerDes-lane index. That
is **board data, not vendor microcode**: it describes how this switch is wired, and
`/etc/prefdl`'s board description encodes the same mapping. Reading it from there at
run time would be better than holding a table, and is not done.

The GLORTs are already ours: `0x0001` everywhere except the two configured links,
`0x03ee` on port 20 (et2) and `0x03ef` on port 40 (et1) — the values `portd` assigns.
Ports 0, 1 and 3 are not front-panel and take `0` in word0's low half.

## ⚠ Et2's five-boot check is outstanding

`docs/ET2-LINK-RATE.md` says a residual reduction should not be adopted on the
strength of Et2 looking acceptable in a handful of boots. Et2 came up on **the one
boot** this was verified on. That is a functional check, not the comparison the rule
asks for: five boots of the 8,430 configuration against five of the 7,088 one, same
day and cable.

The generator itself is not in doubt — 110 of 110 settled values reproduced, and the
chip reads back what it wrote. What is unmeasured is whether removing 970 writes
from the stream shifts the DAC link's rate, and that has caught this project before.

## What is left

    resid.txt      7,088 writes (128 KB)
      FFU          3,545   understood, declined (docs/FFU-RESIDUAL.md)
      MOD            771
      HASH           314
      + others     2,458
    ucode_l2.raw     546 KB
    ucode_tail.raw   165 KB
    spico blob        12 KB   copper DAC only

FFU is now half of everything left. After the declined FFU, `MOD` is next: 771
writes, 89 same-value addresses, 26 convergent, 87 table.
