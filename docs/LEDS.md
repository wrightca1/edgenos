# Front-panel LEDs on the 7150

**2026-08-06.** The plumbing is complete and works. **Nothing drives it** — the front panel is dark.

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

## ⚠ The one thing that needs a human

**I cannot see the front panel.** Everything above is inference from registers.

The check takes ten seconds: boot EdgeNOS, confirm a port is up
(`fm6000reg 0000:02:00.0 0xe3826` reads `1`), and **look at the switch**.

- **Port LED lit** → hardware-driven, nothing to build. Fix `scd-setup.sh` to stop
  declaring bogus addresses (or drop the LED objects entirely) and close this out.
- **Port LED dark** → the SCD needs software after all, and the register is somewhere
  we have not found. Next step would be a full-BAR diff between EOS booted with a
  port up and the same port down, looking specifically outside `0x6000-0x6400`.

Until someone looks, treat "LEDs work" as **unverified** either way.

## What a policy would look like

Small and low-risk once the above is settled — poll each port's `PORT_STATUS` (`0xe3800` for Et1,
`0xe4000` for Et2, and the corresponding EPL blocks for the rest) and set `status<N>` accordingly:
link down → off, link up → on, activity → blink. Plus `status_sys` green once the dataplane is up
and the thermal loop is healthy.

It is the most *visible* remaining gap — an operator's first check on a switch is the front panel —
and it is one of the cheapest.
