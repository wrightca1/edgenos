# 0.3.0-alpha44 — the CM mapping tables and shared-partition watermarks

**397 writes over 397 addresses.** Provenance **119,744 of 124,889 = 95.9%**
(alpha43: 95.6%) — exactly +397, no overlap with any existing generator.

## What changed

`asic/fm6000/fm6000_cmrest.c` (from `asic/fm6000/tools/gen_cmrest.py`) covers the
write-once CM addresses that `fm6000_cmwm` (the six per-port watermark tables) and
`fm6000_cminit` (72 addresses) both leave uncovered:

| register | writes | shape | what it is |
|---|---:|---|---|
| `CM_TC_PC_MAP` | 152 | 76 × 2w | traffic class → port class, per port |
| `CM_BSG_MAP` | 106 | 76 × 2w | buffer/scheduler group, per port |
| `CM_PC_RXMP_MAP` | 76 | 76 × 1w | port class → RX memory partition |
| `CM_SHARED_RXMP_WM` | 16 | 16 × 1w | shared watermark per RX partition |
| `CM_RXMP_SOFT_DROP_WM` | 16 | 16 × 1w | soft-drop threshold per partition |
| `CM_SHARED_RXMP_PAUSE_ON_WM` | 12 | 12 × 1w | pause assert |
| `CM_SHARED_RXMP_PAUSE_OFF_WM` | 12 | 12 × 1w | pause deassert |
| `RXMP_MAP` / `TXMP_MAP` / `TC_MAP` / `GLOBAL_WM` | 7 | — | singletons |

Every address is computed from the SDK geometry, never transcribed; all 397
resolve with **no residue**. 76 is the port count — the same dimension `LBS_CAM`,
the CM watermarks and MAPPER all use. CM maps **all 76**, not just the 55 that
carry config elsewhere, consistent with a map that must return something defined
for any port index the pipeline can present.

## ★ The selection rule changed, because ranking by size was wrong

An earlier plan called SAF (339 writes) and HASH (314) "the smallest, best next
targets". Measuring the uncovered set says the opposite — **SAF and HASH have zero
write-once addresses left**, because their existing generators already took them.
What remains in them is entirely multi-write accumulating state.

| block | addrs | writes | multi-write | **write-once (authorable)** |
|---|---:|---:|---:|---:|
| FFU | 935 | 3813 | 935 | **0** |
| ERL | 967 | 1934 | 967 | **0** |
| L2L | 144 | 1357 | 144 | **0** |
| PARSER | 304 | 1164 | 110 | **194** |
| **CM** | 701 | 1005 | 304 | **397** |
| MOD | 306 | 979 | 306 | **0** |
| ESCHED | 171 | 841 | 67 | **104** |
| SAF | 168 | 339 | 168 | **0** |
| HASH | 100 | 314 | 100 | **0** |

**Rank by authorability, not write count.** The real order is CM (397) →
PARSER (194) → ESCHED (104); everything else needs a different technique.

SAF's repeats are 100-of-101 *monotonic* (bits only added), i.e. a bitmap
accumulated as ports come up — the `RX_SLOW_PORT[1..4]` pattern, where the rule is
not to claim the address at all. ERL is different again: 636 distinct values with
**zero** monotonic, so its repeats are changing values, not accumulation.

## Measured

Both `--verify` (397/397 byte-identical) and `--counts` (no write-count
divergence) pass. Both ports clean-lock `000008c0`/`0940`, HiBer clear, pcsRx=1.

⚠ CM decides when the chip **drops** and when it **PAUSEs**, so a wrong value here
shows as loss under load, not as a failed transit test. Validated against the EOS
reference at a stated pacing (`tools/load-test.sh`, docs/LOAD-LOSS-OPEN.md):

| | EOS 4.16.8M | alpha44 |
|---|---|---|
| paced 2 ms | 0.25% | 0.10 / 0.30 / 0.20% |
| unpaced burst | 41.5% | 41.20 / 42.60 / 41.35% |

No regression in drop or pause behaviour. Transit passes with MAC rewrite and TTL
decrement; OSPF up, `fibd: programmed 14 route(s)`.

md5 `736b159fb8cefa9dd1c5fee89b2e5fa0`, 19,040,808 bytes, verified on the switch.
