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

## ⚠ Unverified: does a write actually reach the hardware?

Writing brightness through sysfs is accepted and reads back correctly:

```
echo 1 > /sys/class/leds/status1/brightness   ->  reads 1
echo 2 > ...                                  ->  reads 2
echo 255 > /sys/class/leds/status_sys/...     ->  reads 255
```

but the backing SCD register stays zero throughout:

```
scd[0x060d0] = 0x00000000     (before and after)
scd[0x06050] = 0x00000000     (before and after)
```

So we can drive the *software* LED object; **it is not confirmed that a physical LED illuminates.**
Three possibilities, not yet distinguished:

1. the register is write-only / reads as zero,
2. the address declared by `scd-setup.sh` is wrong for this board,
3. the driver is not reaching hardware at all.

**The experiment that settles it:** boot EOS (which definitely lights the panel), bring a port up,
and read `0x60D0`/`0x6050`. If EOS shows a non-zero value there, our address is right and the gap is
policy; if EOS's LED state lives elsewhere, `scd-setup.sh` is declaring the wrong block. Five
minutes, and it decides whether this is a trivial feature or an addressing bug.

## What a policy would look like

Small and low-risk once the above is settled — poll each port's `PORT_STATUS` (`0xe3800` for Et1,
`0xe4000` for Et2, and the corresponding EPL blocks for the rest) and set `status<N>` accordingly:
link down → off, link up → on, activity → blink. Plus `status_sys` green once the dataplane is up
and the thermal loop is healthy.

It is the most *visible* remaining gap — an operator's first check on a switch is the front panel —
and it is one of the cheapest.
