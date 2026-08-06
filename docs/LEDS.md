# Front-panel LEDs on the 7150

**2026-08-06. RESOLVED: the LEDs work, and there was nothing to build.**

Visually confirmed on the switch: with EdgeNOS booted and Et1 up, the front-panel port LED is
**lit**. The SCD drives the port LEDs directly from link state in hardware — no software policy is
required, and none should be written.

## The stack

| layer | role |
|---|---|
| **SCD FPGA** (PCI `3475:0001`) | physically drives the LEDs. Per-port at `0x60D0 + 0x10*(port-1)`, system status at `0x6050` (from Arista's own `CotatiP4.fdl`, line 111) |
| **`scd` + `scd-hwmon`** | Arista's GPL kernel drivers (`sonic-platform-modules-arista`), loaded by `init-m1` |
| **`scd-setup.sh`** | declares each LED at boot through the SCD driver's `new_object` interface: `led <addr> <name> <kind>` |
| **`scd-led.c`** | registers them as standard Linux LED-class devices under `/sys/class/leds/` |
| **`platform.py`** | `leds()` / `led_set()` helpers |

Live on the box: **57 LED-class devices** — `status1..status52` (one per front-panel port),
`status_sys`, and `fan1..fan4`.

## Current state: no policy

```
status1     brightness=0   max=255   trigger=[none]
status_sys  brightness=0   max=255   trigger=[none]
fan1        brightness=1   max=255   trigger=[none]
lit: 4 of 57
```

Every trigger is `[none]`. The only lit LEDs are the four fan ones, set by the fan driver rather
than by us. **Nothing maps link state → port LED or system health → status LED**, so all 53 status
LEDs are dark — including on the two ports that are up and forwarding.

## What the EOS comparison showed

The open question was whether `0x60D0`/`0x6050` are the right addresses. They are **not**.

Booted EOS with **both ports up and the panel definitely lit**, and read the SCD:

```
port1 0x60D0 = 0x00000000      <- zero, under EOS, with the LED lit
port2 0x60E0 = 0x00000000
sys   0x6050 = 0x00000000
0x6000-0x600c = 0x00a95f60     <- the only non-zero registers in the whole block
```

So `scd-setup.sh` is declaring the wrong addresses. `0x60D0 + 0x10*(N-1)` came from
`CotatiP4.fdl` line 111, but nothing lives there on this board.

**Then the important part:** the same block under EdgeNOS is *byte-identical*.

| register | EOS (panel lit) | EdgeNOS |
|---|---|---|
| `0x6000`-`0x600c` | `0x00a95f60` | `0x00a95f60` |
| `0x6050`, `0x60D0`, `0x60E0` | `0` | `0` |

Our LED-related hardware state matches a working EOS exactly. Combined with a port
bounce diff (`shutdown`/`no shutdown` on Et1 under EOS) which changed only the SFP
TX-disable bit `0x5010`, an interrupt flag `0x30b0`, and counters — **and no LED
register at all** — the likely explanation is that **the SCD drives the port LEDs
directly from link state in hardware**, with no software involvement.

If that is right there is nothing to implement: the LEDs should already work under
EdgeNOS exactly as they do under EOS, and the 57 sysfs objects are simply pointed at
the wrong (harmless, read-as-zero) addresses.

## Confirmed by looking at it

With EdgeNOS booted and Et1 up, **the front-panel LED is lit.** That settles it:

- The SCD drives port LEDs from link state in hardware.
- EdgeNOS needs no LED daemon. The earlier "53 status LEDs are dark" reading was an artefact of
  looking at sysfs objects that are not connected to anything, not of a dark panel.

## Leftover: `scd-setup.sh` declares 57 objects that control nothing

`scd-setup.sh` declares `led 0x60D0+0x10*(N-1)` per port and `led 0x6050` for status, from
`CotatiP4.fdl` line 111. Those addresses read **zero under EOS with the panel lit**, so they are
wrong for this board, and the resulting `/sys/class/leds/status*` nodes are inert — writing
`brightness` changes the sysfs value and nothing else.

They are **harmless** (we wrote 0/1/2/255 to them repeatedly during investigation with no ill
effect) but **misleading**: an operator would reasonably think those nodes control the panel.

Worth cleaning up — either drop the LED declarations, or find the real register block if we ever
want software override (e.g. an identify/locator blink). Neither is urgent, and neither affects
whether the panel works today.

## What a policy would look like

Small and low-risk once the above is settled — poll each port's `PORT_STATUS` (`0xe3800` for Et1,
`0xe4000` for Et2, and the corresponding EPL blocks for the rest) and set `status<N>` accordingly:
link down → off, link up → on, activity → blink. Plus `status_sys` green once the dataplane is up
and the thermal loop is healthy.

Not needed for normal operation, since the hardware already does link-state indication. It would
only be worth building for something the hardware cannot express — a locator/identify blink, or
folding thermal and dataplane health into `status_sys`.
