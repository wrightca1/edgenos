# 0.3.0-alpha37 — CM watermark tables authored

The largest single block still replayed from EOS after L3AR. **Provenance crosses 89%.**

## What changed

`asic/fm6000/fm6000_cmwm.c` (from `asic/fm6000/tools/gen_cmwm.py`) programs the six per-port
congestion-management watermark tables — **6,512 writes**:

| table | base | shape |
|---|---|---|
| `RXMP_PRIVATE` | `0x112800` | 76 ports x 12 classes |
| `RXMP_HOG` | `0x113000` | 76 x 16 — a single constant, `0xffffffff` |
| `TXMP_PRIVATE` | `0x113800` | 80 x 16 |
| `TXMP_HOG` | `0x114000` | 80 x 16 |
| `RXMP_PAUSE_ON` | `0x115000` | 76 x 12 |
| `RXMP_PAUSE_OFF` | `0x115800` | 76 x 12 |

Indexed `port * 16 + traffic_class`, and the contents collapse to a handful of **port groups**
each with one per-class vector: ports 1-2, 4-19 and 48-51 take "no limit" in the RX tables while
0, 3, 20-47 and 52-75 take real watermarks, and TX ports 76-79 are zeroed. The C is emitted
**structurally** — 154 lines of groups and vectors, not 6,512 opaque words.

`fm6000_cmminit` covered only 72 addresses; these tables were untouched. Zero overlap with any
existing generator.

`--verify` is a strict byte comparison **in both directions** — every word we emit matches the
image, and nothing in the six ranges is left unemitted: **6,512 identical, 0 differing, 0 missing,
0 unemitted.**

## Measured

| | alpha36 | alpha37 |
|---|---|---|
| executed writes | 130,672 | 130,672 |
| ours | 110,863 (84.8%) | **117,375 (89.8%)** |
| et1 / et2 | `0940` / `0940` | `0940` / `0940` |

+6,512 exactly.

## ⚠ Load-tested, because a transit test cannot validate watermarks

Watermarks decide when the chip drops and when it asserts PAUSE. A wrong value does not fail
loudly — it appears as loss under load, which the 5-packet transit test would never catch. So
alpha37 was compared against alpha36 on an identical load: 2000 x 1400-byte frames through the
switch, counted at the peer's `swp7` (ingress) and `swp6` (egress) interface counters.

| image | out of 2000 | loss |
|---|---|---|
| alpha36 (no CM watermarks) | 1957, 1961 | 39-43 |
| alpha37 (with CM watermarks) | 1950, 1960, 1945 | 40-55 |

**The ranges overlap; the change does not clearly introduce loss.** With n=2 and n=3 the
difference in means (41 vs 48) is within run-to-run variance, and a larger sample would be
needed to claim any effect either way.

⚠ **But the ~2% loss is real and PRE-EXISTS on both images.** 2000 frames go in and ~1955 come
out, with `tcpdump` reporting 0 kernel drops and the sender confirmed at 2000 on the wire, so
the frames are lost inside the switch. That is a genuine open issue, not an artefact and not
something this change caused. It is the first time the transit path has been load-tested at all;
the previous validation only ever sent 5 packets.

Also noted: `edgenos-up.sh` returned RC=1 on one run of this boot where it returns 0 normally.
Not investigated; flagged in case it recurs.

md5 `d8541a5b410207a624be7ee56e0ddb76`, verified on the switch.
