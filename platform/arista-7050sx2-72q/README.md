# Arista DCS-7050SX2-72Q — EdgeNOS platform support

**A 2017 Arista switch running our own network operating system, forwarding
routed traffic in hardware, with no EOS involved.**

72 × 10G (48 SFP+, 6 QSFP+ broken out 4×10G) on a Broadcom **BCM56860
(Trident2+)**. EdgeNOS drives the ASIC directly through the OpenBCM SDK from a
user-space BDE shim — no vendor kernel modules, no vendor agents.

---

## What actually works, and how it was measured

| | status |
|---|---|
| Cold ASIC bring-up from power-on | working |
| All 72 front-panel ports link at 10G | working |
| L2 switching, MAC learning | working |
| IPv4 + IPv6 routing **in the switch chip** | working, measured below |
| OSPFv2 + OSPFv3 (FRR) on multiple ports | Full adjacencies |
| Sensors, PSU, LEDs | working |
| Fan control / thermal loop | **not implemented** |

### Hardware forwarding, measured rather than asserted

A thousand packets were pushed through the box in each address family while
watching the counter of packets that reached the CPU. If software were doing the
forwarding, every packet would appear there.

|                                   | IPv4        | IPv6      |
|-----------------------------------|-------------|-----------|
| delivered end to end              | 1000/1000   | 997/1000  |
| reached the CPU                   | 16          | 14        |
| idle background rate              | 16          | 13        |
| expected if software were routing | 2000        | 1994      |

Two orders of magnitude. The chip is forwarding; the CPU is not in the path.
Topology was one port to a Broadcom-based switch and one to a Cisco Nexus, with
routes learned by OSPF and mirrored into the chip's own tables.

## Layout

```
asic/bcm56860/          the SDK-facing layer (shared, not board-specific)
  bde_shim.c            user-space BDE: PCI mapping, DMA pool, interrupt stubs
  sdkpoc.c              cold init and port bring-up
  tapbridge.c           puts hardware ports on the Linux network stack
  l3sync.c              mirrors the Linux FIB into the chip's route tables

platform/arista-7050sx2-72q/
  scdreset.c            SCD: reset, watchdog, SMBus, sensors   (GPL-2.0, see below)
  leddance.c            front-panel LEDs
  kernel/               kernel config fragment
  deploy/frr/           zebra, ospfd, ospf6d configuration
  tools/                config generators and test scripts
```

## Bringing one up

The ASIC configuration is **not shipped** — see `PROVENANCE.md` for why. Generate
it from your own switch while it is still running the vendor OS:

```sh
POLARITY=1 ./tools/mkconfigbcm.sh <switch-ip> > config.bcm
```

That reads the board's port map and SerDes polarity from the machine in front of
you and writes a `config.bcm` EdgeNOS can boot with. It is read-only — every
command it issues is a show command or a register read.

**The polarity table is not optional.** Without it links come *up* and carry
garbage: inverting a 64b/66b stream turns the sync header `01` into `10`, which
is also legal, so the receiver locks happily onto nonsense and reports no fault.
30 TX and 32 RX lanes are inverted on this board.

## Licensing, in one paragraph

Most of this is ours and permissively licensed. **`scdreset.c` is GPL-2.0** — its
SMBus master was transcribed from Arista's own GPL driver, and transcription
carries the licence with it. The OpenBCM SDK is not redistributed here; you
supply your own. `PROVENANCE.md` records, file by file, what is ours, what is
someone else's open code, and what is deliberately absent.
