# Front-panel LEDs

Three independent mechanisms drive this panel, and every one of them needs
different code. Getting them confused is most of what makes port LEDs hard on
this board.

| LEDs | driven by | written by |
|---|---|---|
| status, fan, PSU1/2 | SCD registers `0x6050`–`0x6080` | `platmon led auto` |
| QSFP Ethernet49–52 | SCD `0xA000` + `n`×`0x10`, **one block per lane** | `platmon led auto` |
| copper Ethernet1–48 | BCM84848 PHY register `1.0xa83b` | `sdkpoc`, from link state |

## The chassis and QSFP LEDs

Both are SCD register blocks, one 32-bit word each, written with the legacy
constants (`0x0006ff00` off, `0x1006ff00` green, `0x0806ff00` red). The board
description defines LED blocks for the status LED (with a blue beacon
capability), one fan LED, the two power supplies, and then sixteen more for the
QSFP ports — **four per port, one per lane**, running from `0xA000` to `0xA0F0`.

There is no SCD LED block for Ethernet1–48. That is not an omission in our
reading of it; the board simply does not wire the copper LEDs to the SCD.

⚠ `0xA100` is transceiver control, immediately above the LED blocks.
`qsfp_led_set()` refuses any offset past `0xA0F0` for that reason — an
off-by-one here writes transceiver control registers.

⚠ These registers **read back `0` while the LEDs are physically lit**, so a
read is not a check. Confirm by looking at the panel.

The QSFP LEDs were dark for a long time for an unglamorous reason: nothing had
ever written them. `led auto` drove the chassis LEDs and the QSFP blocks were
simply never in the list.

## The copper LEDs

Not the SCD, as above. Also **not the switch chip's LED processors** — Trident2
has two, and many 48-port designs use them, but on this board both read
`LEDUP_EN=0` and `LEDUP_RUNNING=0`, with `PORT_ORDER_REMAP` and `CLK_PARAMS` at
values that are byte-identical to what the vendor NOS leaves behind. They are
reset defaults that nothing enables, on either NOS.

They are the BCM84848's own LED outputs, and `1.0xa83b` is the drive: **five
3-bit mode fields**, one per LED.

| mode | meaning |
|---|---|
| 0 | dark — the SDK's default in most fields |
| 2 | **lit** |
| 3 | the parked state `_phy_8481_halt` writes to all five (`0xb6db`) |
| 4 | "the PHY firmware drives this" |

The vendor NOS holds all five fields at 4 and **its firmware rewrites field 0 to
2 when the port gains link** — `0x4924` with no link, `0x4922` with link. Three
further registers differ from the SDK's defaults and are pure configuration,
identical on linked and unlinked ports: `0xa82c`, `0xa82f` and `0xa835`, all
`0x0020`.

Our PHY firmware never writes `0xa83b` at all. Mode 4 means "let the firmware
drive it", and ours does not — so **matching the configuration was necessary and
lit nothing by itself.** The fault was never the configuration; it was that no
code on the box was ever going to write that register.

So EdgeNOS writes it, using the vendor's own two values:

* the three configuration registers applied once per PHY at startup
* `0xa83b` set to `0x4922` / `0x4924` per port as link comes and goes
* a linkscan callback for immediacy, **plus** a 2-second reconcile thread

The thread is not redundant. Linkscan reports only *changes*, so a port that is
already up when the handler registers never generates one and would stay dark
until it flapped. Polling the cached link state costs nothing — hardware
linkscan keeps it in software, no MDIO — and the PHY is written only when our
own view changes, so the steady state is silent. It is also self-healing, which
a callback is not: one missed event cannot leave an LED lying indefinitely.

The other four mode fields stay at 4. They are the activity and speed LEDs the
vendor firmware drives, and we have no honest source for them; leaving them at 4
keeps them dark rather than lighting them with something invented.

### ⚠⚠ An LED never justifies a bus transaction

Driving these LEDs once broke **receive on all 48 copper ports**. Transmit was
fine, the 40G SerDes ports were fine, and everything behind a BCM84848 went
deaf — through a reboot, a full port bring-up sequence, and reverting the LED
registers. Rolling the agent binary back restored it instantly, which is what
identified the LED code as the cause at all.

Bisected by making it runtime-switchable rather than guessing — `SDKPOC_PHYLED`
selects off / config-only / full, and `phyled sync on|off` toggles link driving
live, so both halves are testable on one boot:

| | copper receive |
|---|---|
| LED code disabled | works |
| config registers written to all 48 PHYs | works |
| link driving from cached state | works |
| link driving calling `bcm_port_speed_get` per PHY every 2 s | **dead** |

It was the **MDIO budget**, not the registers. Twenty-four bus transactions a
second, on a bus already shared with linkscan and the SDK's own PHY driver,
starves the path copper receive depends on. The 40G ports were unaffected
because they are direct SerDes and touch no MDIO.

So: `bcm_port_info_t` already carries the negotiated speed in the linkscan
callback, and `bcm_port_link_status_get()` reads software state that hardware
linkscan maintains. Both are free. The only MDIO left is **one speed read per
port at startup**, plus a bounded re-read for a port linkscan reports up but
for which no speed was ever learned — which cannot repeat, because the next
pass has one.

⚠ That startup seed is **not optional**. Without it the cache is empty, and
linkscan reports only *changes* — a port already up when the agent starts never
populates it, so the whole panel stays dark while the code reads as correct.

`leds=yes` in `datapath.conf` gates all of it, defaulting to **off**. The
datapath is what matters; the LEDs are decoration.

### ⚠ A link with no speed is not a link

`bcm_port_link_status_get` returns **UP for every copper port on this board**,
cable or no cable — "Link Up with Speed 0M!" with the BCM84848 driver loaded.
Gating the LEDs on link status alone lights all 48 with nothing plugged in.

The correct test is `link status UP && bcm_port_speed_get() > 0`, which is the
same test the routed-port setup in `sdkpoc.c` already uses for exactly this
reason — and which still got missed twice when the LED code went in.

Startup deliberately seeds every port **dark** rather than reading link, because
that read happens before linkscan settles and reports UP on empty ports. An LED
that is wrong is worse than one that is two seconds late.

### ⚠ `phy raw c45` cannot reach these PHYs

It returns `0xffff` for everything, including the PMA/PMD ID that `phy info`
reads correctly as `600d`/`84f9`. The raw path uses the switch chip's internal
MIIM controller, and this board hangs the copper PHYs off the SCD's three MDIO
accelerators instead. Only the driver path reaches them — `pc->read`/`pc->write`,
which is what `sdkpoc`'s `phyreg` command uses.

An all-ones read is not data. It is worth saying because it looks exactly like a
register full of ones.

## Commands

```
platmon led <status|fan|psu1|psu2> <off|green|red>
platmon led qsfp<49-52> <off|green|red>
platmon led auto                     # chassis + QSFP from measured state

phyreg <port> <devad> <reg> [value]  # clause-45 via the driver path
phyreg dump <port>                   # the LED control block
phyled fix [port]                    # re-apply the copper LED configuration
```

`phyreg`/`phyled` go to `sdkpoc`'s command FIFO (`SDKPOC_DAEMON`).

## What is not implemented

Activity and speed indication on the copper ports, and per-lane LEDs when a QSFP
port is broken out into 4×10G — `qsfp_led_set()` drives all four lane blocks
together, which is correct for a 40G port and wrong for a broken-out one.
