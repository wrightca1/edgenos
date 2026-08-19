# Provenance — what here is ours, and what is not

Written because "can we publish this?" is a question about *specific files*, not
a mood. Every item is one of four things: **ours**, **someone else's open code**,
**vendor-derived**, or **an external dependency**.

The equivalent file on the 7050TX-64 branch records that an earlier version of
itself claimed the platform layer "could be published today", and that a proper
file-by-file audit then found two more items. **The audit came first here.** The
full working is in the reverse-engineering repo as
`docs/PUBLICATION-AUDIT-20260819.md`.

---

## Ours, outright

Written against public documentation, our own measurements, and the SDK's
published API.

| file | what it is |
|---|---|
| `asic/bcm56860/bde_shim.c` | user-space BDE — PCI mapping, DMA pool, interrupt stubs |
| `asic/bcm56860/sdkpoc.c` | cold init, port bring-up |
| `asic/bcm56860/tapbridge.c` | hardware ports on the Linux network stack |
| `asic/bcm56860/l3sync.c` | FIB → chip route/host tables, field-processor punt rules |
| `platform/.../leddance.c` | front-panel LEDs |
| `platform/.../kernel/config-7050sx2-72q` | kernel configuration |
| `platform/.../tools/mkconfigbcm.sh`, `mkpolarity.sh` | the generators below |
| `platform/.../deploy/frr/*` | routing configuration |

## Someone else's open code — usable, with obligations

| source | licence | how used |
|---|---|---|
| Arista `scd-smbus.c` | GPL-2.0 | **transcribed** into `scdreset.c` — see below |
| Arista `scd-reset.c`, `scd-led.c`, `raven-fan-driver.c` | GPL-2.0 | read for register maps; no code copied |
| Broadcom OpenBCM SDK | Broadcom licence | API only; **not redistributed here** |
| FRR 8.4.4, glibc | GPL-2.0 / LGPL | shipped unmodified in the image |

⚠ **`scdreset.c` is GPL-2.0 and is marked as such.** Its SMBus master is, in the
file's own words, "transcribed from Arista's GPL driver ... transcription rather
than reverse engineering". Transcribing a driver's bitfield layout and protocol
produces a derivative work, so the licence follows it. The rest of the file
reads register *maps* from the same GPL sources, which is fact-gathering and
does not carry the licence — but the file is GPL-2.0 as a whole because the
SMBus section is in it.

Every SCD offset this platform uses is genuinely published by Arista in the
GPL-2.0 `aristanetworks/sonic` tree — watchdog `0x0120`, reset `0x4000`, power
`0x5000`, SFP LEDs `0x6100`, QSFP LEDs `0x6400`, SMBus `0x8000`. Checked, not
assumed.

⚠ **We ship GPL binaries and therefore owe their source.** The image carries FRR
and glibc. Distributing them obliges us to make the corresponding source
available; today the image ships them and says nothing. A real gap, cheap to
close, unrelated to whether the rest is ours.

## Vendor-derived — deliberately absent

| item | source | status |
|---|---|---|
| `config.bcm` port map | vendor OS's own logical→physical→MMU table | **REMOVED** — generate with `tools/mkconfigbcm.sh` |
| SerDes polarity table (62 lanes) | read live from the vendor OS | **REMOVED** — generate with `tools/mkpolarity.sh` |
| `serdes_preemphasis` (96 lanes) | vendor's TX equaliser values | **REMOVED** — not load-bearing; see below |
| CL72 config, front-panel port map from `.fdl` | vendor board description file | **REMOVED** |
| counter/memory base tables | harvested from the vendor's own libraries | **REMOVED** |
| S-Channel captures, register dumps, logs | vendor OS running | **NOT PUBLISHED** |

**Nothing vendor-derived is published on this branch.** The mechanism is ours and
is here; the board's numbers are read at runtime, on your switch, by tools that
only issue show commands and register reads.

Both generators are tested, not merely written:

```
port map:  78 / 78  identical to the working configuration
polarity:  62 / 62  identical
overall:  314 / 410 properties reproduced
```

The 96 not reproduced are all `serdes_preemphasis_lane*`, omitted on purpose.
They are **not load-bearing** — without them the SDK falls back to the chip's
reset TX FIR and links still come up and pass traffic; they affect signal
quality, not correctness. The *encoding* is public (OpenBCM `src/soc/phy/tsce.c`:
`(post << 16) | (main << 8) | pre`) and is documented in the generator's output.
Deriving the vendor's values back out of the TXFIR registers was attempted and
abandoned — `0xd110` reads `0x0460`, whose low 7 bits give `main = 96` correctly,
but no obvious packing of the remaining bits yields `post = 16`. Guessing it
would be the exact failure mode this project has hit repeatedly, so it is left
to whoever needs that last few percent of tuning.

## One dependency chain worth stating plainly

Several register tables in the reverse-engineering repo are generated from a
capture of **our own** `sdkpoc` running `bcm_init` — not from the vendor OS. That
distinction was checked rather than assumed, because a document was once
published in this project claiming "the vendor never inverts TX" from that same
trace: the measurement was right and the subject was wrong.

However, that capture was produced by running our SDK *with* the vendor-derived
`config.bcm`. The port map is a fact about how the board is wired rather than
anything creative, but the dependency is real and is recorded here rather than
glossed over. None of those generated tables are on this branch in any case.
