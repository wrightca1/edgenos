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
| Front ports | all 52 — 48 × 10GBASE-T + 4 × 40G QSFP+ |
| Copper rate | 10G full duplex demonstrated on one port against a 10GBASE-T peer |
| Datapath | tap netdev per port, RX and TX verified, transit forwarding counted at the chip |
| L3 | routed ports EOS-style — one VLAN and one L3 interface per port |
| Control plane | FRR 8.4.4 — OSPFv2 **and** OSPFv3, two Full adjacencies each |
| Hardware FIB | IPv4 **and** IPv6: adds, next-hop changes and withdrawals verified against the chip |
| Redundancy | primary/backup uplinks by OSPF cost, failover **measured** — see below |
| Configuration | the datapath is a runtime file, `deploy/datapath.conf` — ports, addresses, policy routes, no rebuild |
| Front panel | chassis, QSFP and copper port LEDs all driven — see [LEDS.md](LEDS.md) |
| Platform | 5 hwmon devices, 2 PSUs over PMBus, 4 fan trays, chassis + tray LEDs |
| Cooling | closed loop on the inlet sensor, clamped `[108, 180]`, refreshed every 60 s |
| PSU fans | bounded PMBus control, 40% floor, refuses a loaded supply without `--force` |
| Watchdog | **hardware** — `sp5100_tco`, 60 s timeout, fed by `init` |

Failover was tested by cutting the primary uplink with a continuous ping
running, not by reading the routing table:

```
primary down   36 routes move to the backup   19 s of loss
primary back   36 routes return               14 s of loss
```

⚠ That tests a **clean link-down**. It does not cover a link that stays up
while forwarding is broken — which happened on this bench, via a 10GBASE-T SFP
whose EEPROM claimed to be a fibre optic. OSPF holds the adjacency and keeps
feeding a black hole. BFD is what catches that; it is not configured here.

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
LEDS.md              the three LED mechanisms, and the MDIO budget rule
IPV6.md              OSPFv3, the v6 hardware FIB, and what still is not proven
kernel/              kernel configuration
initrd/init          the initrd: SCD, resets, sensors, cooling, panel, flash
initrd/bin/          datapath-up.sh — applies deploy/datapath.conf at boot
config/              config.bcm (⚠ EOS-derived — see PROVENANCE.md), phy bus
deploy/              datapath.conf, FRR configuration and daemons
platmon.c            platform monitor: sensors, PSU, fans, LEDs, cooling
kernel-params        the kernel cmdline; belongs at /mnt/flash/kernel-params
tools/               image build, reset release, retimer, Aboot and EOS console
tools/lansniff.c     passive capture — ARP, DHCP, IPv6 ND, OSPFv3 packet types
tools/arpscan.c      ARP sweep for a device whose address nobody knows
```

`lansniff` and `arpscan` exist because the initrd has no `tcpdump` and busybox
has no capture applet. Both were written to find a device on a wire we control
and then kept, because "what is actually arriving on this port" turned out to
be the question that settled several faults that reasoning alone got wrong.

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
