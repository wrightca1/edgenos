# Arista DCS-7050TX-64 (Yreka64) — BCM56855 / Trident2

Platform support for the Arista 7050TX-64: 48× 10GBASE-T + 4× QSFP+, AMD Kabini
CPU, Broadcom Trident2. **This is a bring-up in progress, not a finished
platform** — read [PROVENANCE.md](PROVENANCE.md) before publishing anything from
it, and the state of play below before trusting it.

The detailed reverse-engineering record — 41 documents, traces, artifacts — lives
in the private research repo `td2-7050tx64-reverse-engineering`; start at its
`docs/STATE-OF-PLAY.md`. This branch carries only the platform support.

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
| Cooling | EOS's interpolated curve, clamped `[127, 180]` |

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

## Layout

```
board.yml            manifest
PROVENANCE.md        what is ours, what is not, what blocks publication
kernel/              kernel configuration
initrd/init          the initrd: SCD, resets, sensors, cooling, panel, flash
config/              config.bcm (⚠ EOS-derived — see PROVENANCE.md), phy bus
deploy/              FRR configuration
platmon.c            platform monitor: sensors, PSU, fans, LEDs, cooling
tools/               image build, reset release, retimer, Aboot console
```
