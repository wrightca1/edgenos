# 0.3.0-alpha45 — PARSER_INIT_FIELDS, the per-port parser seed

**194 writes over 194 addresses.** Provenance **119,938 of 124,889 = 96.0%**
(alpha44: 95.9%) — exactly +194, no overlap with any existing generator.

## What changed

`asic/fm6000/fm6000_parserfields.c` (from `gen_parserfields.py`) covers the 194
write-once PARSER addresses `fm6000_parserinit` leaves uncovered. They all belong
to **one register**:

    FM6000_PARSER_INIT_FIELDS   0x108200   w=2   [2 entries @2w] x [76 ports @4w]

so `addr = 0x108200 + port*4 + entry*2 + word`. All 194 resolve with no residue.

## What the 64-bit entry holds

Entry 0 is the live one — **entry 1 is all zeros for all 76 ports**. Entry 0 in
16-bit fields:

| bits | meaning |
|---|---|
| `[63:48]` | the port's source GLORT |
| `[47:32]` | 1 for a configured port, 0 otherwise |
| `[31:16]` | 1 for a configured port — **except ports 20 and 40** |
| `[15:0]` | `0x100 \| GLORT` for ports ≥ 20; 0 for ports 1 and 3 |

### ★ Two independent blocks agree on 54 values

`[63:48]` is the same per-port GLORT `LBS_CAM` carries as `(X << 16) | ~X`.
Checked field by field: **54 of 55 shared ports match exactly.** The single
exception is port 0, the CPU/management port, which is special in every other
block too.

LBS_CAM was decoded in alpha42 and this register in alpha45, by different routes,
neither written knowing the other's values. Fifty-four independent agreements is
the strongest evidence available that this field is what its name says — far
stronger than any byte-comparison against EOS, which only shows we copied
correctly, not that we understood.

### ★ Ports 20 and 40 name themselves

Their `[31:16]` is `0x03ee` and `0x03ef` rather than 1 — exactly the GLORTs
`edgenos-up.sh` assigns to **et2 and et1** (`portd: et1:03ef:... et2:03ee:...`).
So **port 20 is et2 and port 40 is et1**, a physical-to-logical mapping that until
now had to be inferred from link behaviour.

## Measured

`--verify` 194/194 byte-identical and `--counts` clean. Both ports clean-lock
`000008c0`/`0940`.

- **Transit PASSES** — frame captured leaving et1 at the peer's swp6 with SMAC/DMAC
  rewritten and TTL `0x3f` (63, from the peer's 64). This is the sharp test for a
  parser change: a mis-seeded parser mis-parses the frame rather than failing loudly.
- **Paced load** (EOS reference 0.25%): 1996 / **2000** / 1995 of 2000 — one pass
  forwarded every frame.

⚠ Write-once addresses only. PARSER's other 110 uncovered addresses are
multi-write and **109 of 110 are monotonic** — a bitmap accumulated as ports come
up. Claiming them would make `gen_list` splice away every one of those updates,
the `RX_SLOW_PORT[1..4]` mistake. They stay in the replay.

md5 `e69c84a4450c9a9b43f15e5c58b47ea2`, 19,034,739 bytes, verified on the switch.
