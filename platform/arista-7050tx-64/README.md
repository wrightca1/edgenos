# Arista DCS-7050TX-64 (Yreka64) — BCM56855 / Trident2

Platform support for the Arista 7050TX-64: 48× 10GBASE-T + 4× QSFP+, AMD Kabini
CPU, Broadcom Trident2. **This is a bring-up in progress, not a finished
platform** — read [PROVENANCE.md](PROVENANCE.md) before publishing anything from
it, and the state of play below before trusting it.

The detailed reverse-engineering record behind this — how the chip was brought
up, what was measured, and every wrong turn — is kept in a private repository.
This branch carries only the platform support that came out of it, so some
commit messages here refer to findings you cannot see. Where that matters, the
reasoning has been restated in these documents rather than left as a dangling
reference.

## What works

| | |
|---|---|
| Boot | Aboot → `kexec` → our 6.12 kernel + initrd, from `flash:/edgenos.swi` |
| ASIC | BCM56855_A2 via the BCM56850_A0 driver, OpenBCM SDK 6.5.24 |
| BDE | **user space** — no `linux-kernel-bde`, no KNET, stock kernel |
| Datapath | 40G uplink, tap netdev per port, RX and TX verified |
| L3 | routed ports EOS-style — one VLAN and one L3 interface per port |
| Control plane | FRR 8.4.4, OSPF Full, 35 routes |
| Hardware FIB | adds, next-hop changes and withdrawals all verified against the chip |
| Platform | 5 hwmon devices, 2 PSUs over PMBus, 4 fan trays, chassis + tray LEDs |
| Cooling | closed loop on the inlet sensor, clamped `[127, 180]`, refreshed every 60 s |
| Watchdog | **hardware** — `sp5100_tco`, 60 s timeout, fed by `init`; resets the box if it wedges |

## What does not

* **Copper ports are unreliable to bring up.** `bcm_init` with the PHY bus
  completed on two of five attempts and stalled on three, spinning on two cores
  with the log frozen. Without the PHY bus the SDK binds only the internal TSC
  SerDes and all 48 copper ports stay down. Unexplained.
* **The SDK agent and control plane are started by hand.** Not in `init` yet.
* **No cold-boot test.** Every boot so far has been warm from EOS.

## How this differs from the finished platforms

`accton-as5610-52x` targets `edged` — the shared datapath in `core/datapath`.
This board targets **`sdkpoc`**, which is the bring-up agent: it does cold init,
port bring-up, the tap datapath and FIB sync in one binary. Converging it onto
`edged` is the obvious next structural step, and is deliberately not done yet —
the chip-level behaviour was still being learned while it was written.

## Build

```
tools/mkkernel.sh                                  # 6.12 kernel
tools/build-frr.sh                                 # stock FRR + its glibc closure
make -C ../../asic/bcm56855 STATIC=1 OPENBCM_SDK=/path/to/sdk-6.5.24
tools/mkswi.sh                                     # the SWI
```

`STATIC=1` is required for anything that runs before the initrd has a loader.
The SDK is **not** vendored; supply your own tree.

⚠ `build-frr.sh` is **not optional**. `mkswi` warns if the FRR tree is missing
and then builds the image anyway — you get a bootable switch with no routing
daemons at all.

## First boot on your own switch — three files you must generate

This platform deliberately ships **none of the board vendor's data**. Three files
are therefore not in this repository, and the switch needs them. All three are
generated on the switch itself, from what is already on it:

```
tools/fdl-extract.sh all                      # -> /etc/edgenos/{cooling,retimer}.conf
platform trident diag config > asic-config.txt   # under the vendor NOS
tools/mkconfigbcm.py asic-config.txt > config.bcm
```

| file | without it |
|---|---|
| `/etc/edgenos/cooling.conf` | fans run at **100%** |
| `/etc/edgenos/retimer.conf` | the retimer **refuses to program** — 40G stays dark |
| `config.bcm` | the SDK **will not attach** — `Port config error !!` |

Those defaults are chosen, not accidental. A box whose cooling policy is unknown
should be loud rather than warm, and an unprogrammed retimer is an obvious dead
port rather than a marginal link that works until it does not.

[PROVENANCE.md](PROVENANCE.md) explains why these are absent: the mechanism is
ours and is published, the vendor's numbers are not ours to publish.

## Layout

```
board.yml            manifest
PROVENANCE.md        what is ours, what is not, what blocks publication
kernel/              kernel configuration
initrd/init          the initrd: SCD, resets, sensors, cooling, panel, flash
config/              config.bcm (⚠ EOS-derived — see PROVENANCE.md), phy bus
deploy/              FRR configuration
platmon.c            platform monitor: sensors, PSU, fans, LEDs, cooling
kernel-params        the kernel cmdline; belongs at /mnt/flash/kernel-params
tools/               image build, reset release, retimer, Aboot and EOS console
```

## Safety net

There is **no timer that reboots the box on a schedule**. There was, and it
rebooted healthy sessions mid-run. It is replaced by a hardware watchdog:
`sp5100_tco` with a 60 s timeout, fed every 15 s by `init`. If the box wedges —
a hung kernel, a stuck register access — the hardware resets it and Aboot boots
whatever `boot-config` points at, which is normally the vendor NOS.

`CONFIG_WATCHDOG_NOWAYOUT=y` is deliberate: without it, closing `/dev/watchdog`
disarms the watchdog, so anything that stops the feeder silently removes the
safety net. `edgenos_watchdog=off` leaves it unarmed; `EDGENOS_BACKSTOP=<seconds>`
in `kernel-params` re-enables the old timer alongside it if a session wants a
hard ceiling.

⚠ Verified by letting it fire: the feeder was killed and the board reset 62 s
later. Worth knowing before you test it yourself — the reset is unclean, and the
vendor NOS did not come up cleanly afterwards on this board. Do not test this
remotely on a box you cannot power-cycle.
