# EdgeNOS Hardware Abstraction Layer (HAL)

One HAL across every switch. Board differences (SFP mux trees, CPLD layouts, fan/PSU
wiring, driver load order) live in exactly one per-board file; everything above the HAL —
edged, the web UI, `edgenos platform` — talks to the abstract interface and never touches
i2c buses or sysfs paths directly.

> If you find yourself reverse-engineering an i2c bus number or a sysfs path to answer a
> hardware question, that mapping belongs **in the HAL**, not in a one-off script. This doc
> exists because the SFP bus→port map was such a gap (see history at the bottom).

## Layout

```
core/platform/base.py         PlatformHAL (interface) + EdgeNOSPlatformBase (shared impl)
core/platform/current.py      platform detection + loader (ONL current.py analog)
platform/<board>/platform.py  per-board subclass: identity, drivers, sysfs paths, maps
```

- **`PlatformHAL`** — the interface every board answers: `thermals()`, `fans()`, `psus()`,
  `sfps()`, `leds()`/`led_set()`, plus counts. Anything a board doesn't implement raises
  `HALUnsupported` (callers degrade gracefully; `hal_report()` marks it "unsupported").
- **`EdgeNOSPlatformBase`** — board-agnostic machinery: driver load (`insmod`/order),
  bounded sysfs/i2c reads (`_read_bytes` runs in a thread so a hung mux bus can't block the
  caller), SFF-8472/8636 optic decode, generic hwmon `thermals()`.
- **`platform/<board>/platform.py`** — the *only* place board specifics live: `PLATFORM`
  (== switch-DB key), `MODEL`, `DRIVERS`, `INIT_SCRIPTS`, sysfs bases (`CPLD`), the
  `SFP_EEPROMS` glob, and the board's hardware maps (e.g. `_sfp_port_for_bus`).

## Query it — one command, any switch

The `edgenos` CLI auto-detects the board and dispatches to its HAL, so the **operator command
is identical on every switch** — no per-platform i2c/sysfs knowledge required:

```sh
edgenos interface [<port>]      # per-port: link, IPs, routes, optic + LIGHT LEVELS, counters
edgenos sfp [<port>]            # optic inventory by front-panel port (--json for machines)
edgenos platform hal            # full HAL report (thermals/fans/psus/sfps/leds)
edgenos platform show           # identity + info
```

```
$ edgenos interface swp6
swp6: UP  10G  mtu 1600  mac 80:a2:35:81:ca:b4
  ipv4  : 10.101.101.25/29
  ipv6  : 2001:470:882d:1024::2/64, fe80::…/64
  optic : SFP CISCO-FINISAR FTLX8571D3BCL-C2 (SN FNS16020TYU, bus 16)
  light : temp 32.7°C  vcc 3.31V  tx -2.68 dBm  rx -2.09 dBm  bias 7.46 mA
  routes: 10.101.101.24/29 proto kernel scope link src 10.101.101.25
          10.101.255.1 via 10.101.101.26 proto zebra metric 20
  stats : rx 2530 pkt / 0 err    tx 3106 pkt / 0 err
```

`edgenos interface` combines OS facts (link/addr/route/counters from `ip` + `/sys/class/net`) with
HAL facts (optic + DDM); `edgenos sfp` is a thin verb over `load_platform().sfps()`. Same code path
serves the web UI and edged. Add hardware verbs here (not in board scripts) so they stay uniform.

### Light levels (DDM/DOM)

`sfp_diagnostics(bus, kind)` returns live temp / Vcc / bias / TX+RX optical power (dBm):
- **SFP** (SFF-8472): real-time diagnostics on the **A2 page @0x51**, bytes 96–105. On this board 0x51
  is held by a no-op `dummy` i2c device (reserves the address, no sysfs), so the read is forced past
  it with `i2cget -f` — safe, since `dummy` does no i2c traffic.
- **QSFP** (SFF-8636): monitor fields live in the **0x50** image (lower page 22–57), reported per-lane
  (×4). Returns `None` if the module doesn't populate DDM (e.g. a dark/absent optic).

## Transceivers (SFP / QSFP) — the worked example

Reading an optic is board-agnostic: each optic EEPROM (SFF address 0x50) is bound by the
`at24` driver to a sysfs `eeprom` node, and the board only has to declare the glob:

```python
SFP_EEPROMS = "sys/bus/i2c/devices/*-0050/eeprom"     # both 5610 and 4610
```

`sfps()` globs those nodes, reads each with a bounded/threaded read (absent module or broken
mux bus → skip fast), SFF-decodes vendor/part/serial/type, and labels each with its
front-panel port. **The bound `eeprom` node exists only when a module is physically present**,
so `sfps()` is a live inventory, not a static list.

Each entry:

```json
{ "bus": "16", "present": true, "type": "SFP", "vendor": "CISCO-FINISAR",
  "part": "FTLX8571D3BCL-C2", "serial": "FNS16020TYU", "port": 6, "name": "swp6" }
```

### bus → front-panel port

The one piece that *is* board-specific is mapping an i2c bus to `swpN`. Boards override:

```python
def _sfp_port_for_bus(self, bus):    # -> 1-based port, or None if unknown
    ...
```

Default (base) returns `None` → the optic is still reported, keyed by its stable i2c `bus`,
just without a `swpN` label. A board fills this in once its mux topology is known.

#### AS5610-52X map (verified live 2026-07-20)

The kernel enumerates the DTS mux children contiguously, so the bus is a pure function of
the port — no table needed:

| ports | mux path | bus formula |
|---|---|---|
| SFP 1–48 | PCA9546 `@0x75` (ch0–3 = ports 1–32) / `@0x76` (ch0–1 = ports 33–48) → PCA9548 `@0x74` (ch0–7) | `bus = 11 + 9·⌊(p−1)/8⌋ + ((p−1) mod 8)` |
| QSFP 49–52 | PCA9546 `@0x77` (ch0–3) | `bus = 66 + (p−49)` |

Spot values: swp1=11, swp6=16, swp8=18, swp9=20, swp32=45, swp33=47, swp48=63, swp49=66, swp52=69.
The `+9` step per group of 8 is the parent PCA9548 bus consuming one number before its 8 children.

Presence/TX-disable/RX-LOS/TX-fault are PCA9506 GPIO expanders driven by
`services/sfp-enable.sh` (TX-enable on bus 65 @0x21/0x23); per-port DS100DF410 retimers sit at
`0x27` on each SFP bus and are programmed by `services/retimer-init.sh` + `sfp-enable.sh`.

#### AS4610-54T

48× 1G copper (`ge0`–`ge47`, no optics) + SFP+/QSFP+ (`xe0`–`xe5`) on PCA9548 `@0x70` ch0–5 →
**i2c buses 2–7** (`xeN` = bus 2+N; DTS labels port49–54). The DTS declares these as **`optoe`**
devices (optoe2/optoe1), but that module isn't in the current 6.1 image, so **no `eeprom` sysfs
node is bound**. The HAL therefore reads them over **raw i2c** via `SFP_I2C_PORTS = {2:"xe0", …}`
and `i2cdump` — same `sfps()`/`edgenos interface`/`edgenos sfp` output as the 5610. Copper `geN`
ports carry no optic and are instant.

> **Perf note (known kernel regression, not inherent slowness).** Reads over these muxed SMBus
> buses are currently ~0.05–0.1 s/*byte*, so a full optic read is several seconds. Root cause is a
> **per-transaction `msleep(1)` in the `xgs_iproc_smbus` completion poll**
> (`drivers/i2c/busses/xgs_iproc_smbus.c:iproc_smb_startbusy_wait`, `IPROC_SMB_MAX_RETRIES=35`):
> it sleeps *before* checking status, and `msleep(1)` rounds up to ~10 ms/tick, so every transfer
> stalls ~10 ms even though the byte completes in µs. Edgecore had this `msleep` commented out on
> 4.14 (fast); the 4.19 forward-port re-added it and 6.1 still carries it — see the RTC comment in
> `dts/arm-accton-as4610.dts` and `docs/KERNEL_419_PORT.md`.
>
> **Fix staged for the next 4610 build (2026-07-20):** the kernel patch
> `edgecore-4610-54t/nos/kernel/patches/brcm-iproc-6.1.patch` now replaces that `msleep(1)` with a
> **check-first `usleep_range(50,150)` busy-poll** in `iproc_smb_startbusy_wait` — a single-byte SMBus
> op completes in µs, so this makes *all* 4610 i2c (optics, CPLD, sensors) ~50× faster. It is **not yet
> on the live box** (needs a kernel rebuild + reflash); until then, single-port queries are lazy
> (`edgenos interface xe0` reads one module) so the raw-i2c path stays usable. Shipping the `optoe`
> module also helps optics specifically (binds fast eeprom sysfs, no HAL change since the glob is set).

## Caveats / cleanup

- **`platform/accton-as5610-52x/onlp/sfpi.c` is STALE** — its `SFP_PORT_TO_I2C_BUS = 21+port`
  (and GPIO on bus 16/17) do **not** match the live DTS enumeration above (which puts swp1–6 on
  buses 11–16, and the QSFP-reset GPIO elsewhere). It is ONL-heritage C that edged does not use;
  the Python HAL (`core/platform/`) is the source of truth. Remove or reconcile `onlp/`.
- Bus numbers are deterministic for a given DTS (same enumeration every boot) but are **not** a
  stable ABI across kernel/DTS changes — always resolve via the HAL, never hardcode a bus.

## History

`core/platform/base.py:sfps()` originally noted *"bus→front-port labeling is future"*. That gap
is why "what SFP is in swp6?" required manually walking the i2c mux tree. Closed 2026-07-20 by
adding `_sfp_port_for_bus` (base hook + AS5610 map); the AS4610 map is the remaining TODO.
