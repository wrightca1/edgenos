# Platform services on the 7150: what runs, what doesn't

**2026-08-13.** Measured on a live EdgeNOS box rather than inferred from the code. The integration
plan records "2 of 9" platform services and flags fan/thermal as the safety gap; that flag is now
**out of date in a good way** — thermal is implemented and running. The real gaps are elsewhere.

## ✅ Thermal and fans — working, verified live

`init-m1` starts `thermal-control.sh -i 5` (and prints a loud warning if the script is missing).
On the running box:

| | |
|---|---|
| `thermal-control.sh` | running |
| hwmon0 | `fans` (raven-fan-driver) |
| hwmon1 | `max6658` at `2-004c` — SCB SMBus master 0 bus 2 @ 0x4c |
| board temp (`temp1`) | **34 °C** (crit 90) |
| FM6000 die (`temp2`) | **38 °C** (crit 100) |
| fan1–4 RPM | 11741 / 11741 / 11741 / 12160 |
| pwm1–4 | **102** — exactly `PWM_FLOOR`, the designed 40 % floor for a cool box |

The control loop's safety rules are the right ones: any read failure or missing hwmon goes to PWM
255, never below the floor, full speed above 85 °C die, hysteresis before ramping down, and a
stopped-but-present fan forces full speed. Nothing here needs work.

## ✅ I2C / SCD — working

- Drivers loaded: `scd`, `scd_hwmon`, `raven_fan_driver`, `lm90`, `lm75`, `max31790`, `ucd9000`,
  `pmbus_core`, `at24`, `regmap_i2c`, `i2c_dev`.
- **61 I2C adapters** enumerated (`i2c-0` … `i2c-60`); the SCD SMBus masters are declared and the
  buses are live.
- The temperature sensor is bound and read correctly, which proves the board-management path
  end to end.

## ❌ PSU monitoring — not implemented

`ucd9000` and `pmbus_core` are **loaded but nothing is bound to them**:
`/sys/bus/i2c/drivers/ucd9000/` contains only `bind`/`unbind`/`module`/`uevent` — no device links.
There are exactly **two** hwmon devices, `fans` and `max6658`, so there is no PSU hwmon at all.

Consequence: **nothing reports PSU presence, power-good, input/output voltage, or failure.** A dead
or removed supply would go completely unnoticed. On a redundant-PSU box that is precisely the
failure you most want to know about.

The board-management buses that carry it are already declared — `scd-sfp-topology.sh` documents
`new_smbus_master 0x8000 0 8` and `0x8080 1 8` as "accel 0/1 = board mgmt (temp/psu/cpld/Si5338)" —
so the missing piece is instantiating the PSU device on the right bus/address and exposing it,
not any new bus plumbing. `EDGENOS_SUMMARY.md` records the equivalent work already done for the
5610 (PSU1 status in CPLD `0x02`, PSU2 in `0x01`, presence active-low, power-good = bit 1), which
is the shape to copy, with the 7150's own addresses.

## ❌ SFP EEPROM / DOM — not instantiated

No `optoe2` or `at24` client devices exist. `init-m1` runs `sfp-enable.sh` but **not**
`scd-sfp-topology.sh`, which is the script that declares the eight SFP SMBus masters and documents
the `echo optoe2 0x50 > /sys/bus/i2c/devices/i2c-<N>/new_device` step.

Consequence: no media type, no vendor/serial, and **no optical diagnostics** — no TX/RX power, no
temperature per module. That matters directly for the port-3 work: "is light arriving" currently
has to be answered from SCD `rxlos` bits rather than from the module's own DOM readings.

## Summary against the plan's "2 of 9"

| service | state |
|---|---|
| scd-setup | ✅ |
| sfp-enable | ✅ |
| **fan / thermal** | ✅ **implemented and running** — the plan's "safety gap" is closed |
| **PSU** | ❌ drivers loaded, no device bound, no monitoring |
| **SFP EEPROM / DOM** | ❌ masters documented, devices never instantiated |
| diag, retimer, swp-l3, LED | ❌ not started |

Ranked by what would actually bite: **PSU monitoring first** (silent failure of a redundant
component), then **SFP DOM** (it is a diagnostic we keep wanting and working around).
