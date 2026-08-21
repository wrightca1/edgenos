# 0.3.0-alpha42 — the small write-once tables, authored from SDK geometry

L2F, LBS, ALU, POLICER and SSCHED replaced by a generator. **957 writes over 829
addresses.** Provenance **119,347 of 124,889 = 95.6%** (alpha40: 94.8%).

## What changed

`asic/fm6000/fm6000_smalltables.c` (from `asic/fm6000/tools/gen_smalltables.py`).

The first cut was an address/value dump — which is relocation, not authoring, and
does not satisfy "understand and have it work". It was replaced by a structural
form in which **every address is computed, never transcribed**:

    addr = base + sum(index[k] * stride[k]) + entry * pow2ceil(words) + word

All 833 table addresses resolve under that formula with **no residue**, which is
what makes the naming checkable rather than asserted.

## The geometry came from the SDK, and it closes exactly

`sdk_regmap.py` now recovers six more columns per register — dimension maxima,
words per entry, and two axis strides — for all 703 registers, not just the
address and width it took before. The tables tile with no gaps:

| register | shape | span ends at | next register |
|---|---|---|---|
| `L2F_TABLE_4K` | 8 banks × 4096 × 3w | 0x1a0000 | `L2F_TABLE_256` |
| `L2F_TABLE_256` | 4 banks × 256 × 3w | 0x1a1000 | `L2F_PROFILE_TABLE` |
| `SSCHED_TX_NEXT_PORT` | 32 × 1w | 0x008020 | `SSCHED_TX_INIT_TOKEN` |

`--check` now tests **strides**, not just addresses, against four values measured
off the wire — including `L3AR_RAM3` (words=1, stride=0x20), the one this project
originally got wrong. The table would have prevented that error outright.

### The stride trap

An entry's pitch is **pow2ceil(words)**, not `words`. `L2F_TABLE_4K` holds 3-word
entries on a 4-word pitch — one word in four is padding. Assuming a pitch of 3
mis-resolved 24 addresses and would have put every entry after the first in the
wrong row, silently. Same class as the RAM3 stride.

## LBS_CAM is per-port, and the port set agrees for the fourth time

The SDK gives it 76 entries — the port count. The replay writes 55, at indices
**0, 1, 3, 20–47, 52–75**, which is exactly the active-port set the CM watermarks
and the MAPPER QoS maps each reached independently.

Every one of the 55 satisfies `entry = (X << 16) | (~X & 0xffff)`, zero
exceptions — a value stored beside its own inverse, i.e. a match/mask pair packed
into one word. Loopback suppression matches a 16-bit source GLORT exactly, one
entry per configured port.

## ⚠ alpha41 shipped a real defect; alpha42 fixes it

SSCHED is **not** a table. EOS runs an ordered protocol:

    RX/TX_REPLACE_TOKEN = 0
    64 × ( RX_INIT_TOKEN = t ; TX_INIT_TOKEN = t )   ← seeds 64 buffer tokens each
    RX/TX_NEXT_PORT[0..19]                           ← the scheduler rings
    RX_SLOW_PORT[0]
    RX/TX_INIT_COMPLETE = 1                          ← latches the freelist
    RX/TX_REPLACE_TOKEN = 0

`INIT_TOKEN` takes **64 distinct values in sequence** — it is a port that pushes
one token per write, not a register holding a value. alpha41 treated it like every
other table and collapsed it to its final value, seeding **one** token instead of
64 and splicing the other 63 writes out of the replay.

**`--verify` could not catch this.** The last value written is identical either
way, so a byte-comparison against the final-state image passes. Only counting
writes per address exposes it, which is now `--counts`, run alongside `--verify`.

Also fixed: `RX_SLOW_PORT[1..4]` are **not ours**. They are written as 0 at init
and then ~130 more times as ports come up, accumulating to 0xffe0/0xfefe/0xfff0/
0xfff. That is runtime port state. Claiming them spliced every one of those
updates out of the replay. Only `RX_SLOW_PORT[0]`, written exactly once, is ours.

`ALU_Y[alu=4]` is the frame-**length** comparison table: bulk-initialised to
0x4000 (no limit), then three entries overwritten with 1500, 9212 and **1600** —
and 1600 is exactly the MTU `edgenos-up.sh` sets on et1/et2. Final value is
correct there, so it is an explicit, justified exception in `--counts`.

## Measured

| | alpha40 | alpha42 |
|---|---|---|
| executed writes | 124,892 | 124,889 |
| ours | 118,390 (94.8%) | **119,347 (95.6%)** |
| et1 | `0940` | `0940` |
| et2 | `0940`, pcsRx=1 | `0940`, pcsRx=1 |
| et2 retrain attempts | 0 | 0 |

Both ports clean-locked (`PORT_STATUS=000008c0`, HiBer clear, pcsRx=1).

**Dataplane: transit PASSES**, 5 of 5 frames captured leaving et1 at the peer's
swp6, with SMAC/DMAC rewritten and TTL 0x3f (63, from the peer's 64). OSPF
adjacency up, 38 kernel routes, `fibd: programmed 14 route(s)`.

Built with `CONTROL_PLANE` set — alpha41's first build silently omitted zebra and
ospfd, which the script reports but does not fail on.

md5 `67fa2a6eb98785276753d54dcf540946`, 19,025,640 bytes, verified on the switch.
