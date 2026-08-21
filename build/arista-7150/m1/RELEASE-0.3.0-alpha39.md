# 0.3.0-alpha39 — MAPPER tables authored

**Provenance 94.8%, and executed writes drop by 5,718.** The largest single reduction so far.

## What changed

`asic/fm6000/fm6000_mapper.c` (from `asic/fm6000/tools/gen_mapper.py`) authors MAPPER's
remaining tables — 565 addresses that accounted for **6,283 replayed writes**.

**Five sixths of it is boilerplate**, and saying what it is takes one line per table:

    QOS_PER_PORT_VPRI1  0x123f00   identity map, every configured port
    QOS_PER_PORT_VPRI2  0x124000   identity map, every configured port
    QOS_PER_PORT_W4     0x124100   all zero, every configured port

`0x76543210` / `0xfedcba98` is sixteen nibbles where nibble *n* holds *n* — an identity
priority map: VLAN priority in, the same priority out. All 55 configured ports carry the
identical pair; W4 is unused. That is 5,280 of the 6,283 writes.

**Port set cross-check.** Entries exist for ports **0, 1, 3, 20-47, 52-75** and no others —
the same active-port set `gen_cmwm.py` derived independently for the CM watermarks
(0/3/20-47/52-75). Two unrelated blocks agreeing on which ports are configured is good evidence
the index really is a port number.

The remainder — `SRC_PORT_TABLE` (742 writes), the MAC CAMs, L4 compares and QoS-to-ISL maps —
differs per entry and is emitted address by address.

`--verify`: **565 of 565 identical, 0 differing, 0 absent.**

## Measured

| | alpha38 | alpha39 |
|---|---|---|
| executed writes | 130,610 | **124,892** |
| ours | 117,825 (90.2%) | **118,390 (94.8%)** |
| et1 / et2 | `0940` / `0940` | `0940` / `0940` |

**−5,718 executed writes** — the QoS tables were written many times each in the replay and are
now written once. Transit passes: TTL `0x3f` = 63, MAC rewritten.

## ⚠⚠ An upward loss trend that is NOT explained

Load test, 2000 x 1400-byte frames through the switch, counted at the peer's interface counters:

| image | out of 2000 | loss |
|---|---:|---:|
| alpha36 (before CM and MAPPER) | 1957, 1961 | 39, 43 |
| alpha37 (CM watermarks) | 1950, 1960, 1945 | 50, 40, 55 |
| alpha39 (+ MAPPER) | 1940, 1944 | **60, 56** |

Mean loss goes **41 -> 48 -> 58**. The ranges nearly touch and n is 2-3 per image, so this is not
statistically established — but the trend is monotonic and it moves across exactly the two
changes that touch **queuing** (CM watermarks) and **classification** (MAPPER QoS). That is the
failure mode both were flagged for: wrong values do not fail a transit test, they show up as
loss under load.

**This needs a proper controlled measurement before either change is trusted at load** — more
samples per image, and ideally a bisect between alpha36 and alpha39. Both generators are
byte-verified against the image, so if the trend is real the cause is *ordering or timing*, not
values: `gen_list` moves writes to a different point in the boot sequence, and for CM and MAPPER
that has not been examined.

⚠ Also unexplained and pre-existing: ~2% of frames are lost inside the switch even on alpha36,
with the sender confirmed at 2000 on the wire and tcpdump reporting no kernel drops.

md5 `75b3bf233ae5ff3163704291ac208343`, verified on the switch.
