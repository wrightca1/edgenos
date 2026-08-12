# Bringing the M1 dataplane up by hand

**2026-08-11.** `edgenos-ourparser.swi` boots into **PROBE MODE** — the boot stops after the ASIC is
enumerated and the tables are loaded, and leaves the Linux side to an operator:

```
[FM6UP] COLD90 PROBE MODE: chip left ALIVE (microcode loaded, no CRM fill) — SSH in and probe.
```

Nothing on flash starts `portd`, and none of this was written down. Reconstructing it after a
reboot cost far more than it should have, so here it is.

## What the boot DOES do

`fullseq` runs to completion — 226,688 ops, `PIN=0x208`, `FULLSEQ DONE` — and the forwarding
tables are loaded. Verify rather than assume:

| register | expected | meaning |
|---|---|---|
| `0x100c01` | `0x94ffffeb` | our parser is resident |
| `0x10000` | `0xffffffff` | L3AR rule 0, the universal default |
| `0x388000` | `0xffffff98` | FFU slice-2 CAM entry 0 |
| `0x1c021` | `0x208` | PIN_STRAP, chip on the bus |
| `0xe3800` | `0xec0` | EPL14 lane 0: RxLinkUp, HeartbeatOk, Transmitting, Receiving, SerXmit |

⚠ `fm6000reg` parses its address as **decimal** unless you write `0x`. `fm6000reg <bdf> 100c01`
silently reads register 100.

⚠ **`fullseq` runs for minutes after SSH comes up, and a chip mid-fill reads all zeros.** SSH is
answering long before the tables exist. Probed at 1 minute of uptime, every register above reads
`0x00000000` while `PIN_STRAP` is a healthy `0x208` — which looks exactly like "the boot never
loaded the tables" and is really "you are early". Check first, then probe:

```sh
ps w | grep "[f]m6000-fullseq"      # still running?
tail -3 /mnt/flash/fullseq.log      # STEP5 counts down "N writes remain"
```

Wait for the process to exit. `dmesg | grep -i fullseq` finds nothing either way — the script logs
to `/mnt/flash/fullseq.log`, not the kernel ring — so an empty dmesg grep is not evidence.

## ⛔ DO NOT HAND-ROLL THIS. RUN THE SCRIPT.

```sh
/usr/lib/edgenos/platform/edgenos-up.sh
```

It ships in the image (source: `build/arista-7150/m1/payload/edgenos-up.sh`) and does the whole
job: modules and device nodes, loopback, portd, **the MAC**, **MTU 1600**, zebra + ospfd, the OSPF
wait, and the hardware FIB sync. It also refuses to run twice, because restarting portd without a
chip reset wedges the DMA rings and RX silently goes to zero — which looks exactly like a
dataplane defect.

### ⚠ Run it DETACHED, or the watchdog will reboot the box

```sh
setsid nohup /usr/lib/edgenos/platform/edgenos-up.sh >/tmp/up.log 2>&1 </dev/null &
```

Running it over a foreground `ssh` with a client-side timeout kills it: when `ssh` dies the script
takes SIGHUP partway through. **The script starts `fm6000_wdog -g 180` near the end**, so a run
that is interrupted after the watchdog starts but before the dataplane is up leaves the box with a
dead dataplane and an armed watchdog — and ~180 s later it reboots.

Observed 2026-08-12: `timeout 280 ssh ... edgenos-up.sh` cut the script off; management went away
a few minutes later and the box came back on **EOS**. Nothing was wrong with the switch. The
watchdog did exactly what it was written to do.

Two things make this harmless instead of alarming:

- **Set `boot-config` back to `EOS-4.16.8M.swi` as soon as EdgeNOS is up.** EdgeNOS lives entirely
  in RAM, so an unexpected reboot then lands on a working switch rather than stranding it in PROBE
  MODE. Do this *first*, before any experiment.
- **`touch /mnt/flash/wdog.off`** while debugging, so a half-finished bring-up cannot reboot the
  box under you. Remember to remove it when you want the watchdog back.

**This document previously contained a hand-reconstructed sequence, and that sequence was wrong.**
It omitted two lines:

```sh
ip link set et1 address 44:4c:a8:31:5d:ab     # <-- omitted
ip link set et1 mtu 1600                       # <-- omitted; peer runs 1600
```

Missing the MAC cost a full day of debugging. The TAP comes up with a random kernel-assigned MAC
(observed: `0a:2f:38:c1:32:6f`, different every boot). The ASIC's punt tables are programmed for
`44:4c:a8:31:5d:ab`, so the peer answers into a black hole: ARP hangs INCOMPLETE→FAILED, no echo
replies, no OSPF adjacency, `routes=2`. Every symptom points at the forwarding plane and none of
it is the forwarding plane.

⚠ `fm6000_portd`'s third argument is a MAC and **it used to be accepted and ignored** — `tap_open`
never called `SIOCSIFHWADDR`. `edgenos-up.sh` worked because it sets the MAC with `ip link` right
afterwards. Passing the MAC to portd now actually applies it (see `tap_set_mac`), but the script
remains the supported path.

## What the boot does NOT do

Nothing on flash runs `edgenos-up.sh`; it must be invoked once per cold boot. That is the real
content of "PROBE MODE" — the chip is fully brought up and the Linux side waits for an operator.

## The hand sequence, for reference only

If you must do it manually, this is what the script does — but prefer the script.


```sh
insmod /mnt/flash/fm6000dma.ko            # portd needs /dev/fm6000dma
mkdir -p /dev/net && mknod /dev/net/tun c 10 200   # and /dev/net/tun for its TAP

PORTD_TXFCS=1 setsid /usr/bin/fm6000_portd et1 03ef 44:4c:a8:31:5d:ab 0 \
    >/tmp/portd.log 2>&1 </dev/null &

ip link set et1 up
ip addr add 10.101.101.26/29 dev et1
ip addr add 10.101.255.26/32 dev lo && ip link set lo up

echo /usr/lib > /etc/ld.so.conf && ldconfig     # see below
mkdir -p /var/run/quagga /var/log/quagga
LD_LIBRARY_PATH=/usr/lib /usr/bin/zebra -d -f /etc/quagga/zebra.conf
LD_LIBRARY_PATH=/usr/lib /usr/bin/ospfd -d -f /etc/quagga/ospfd.conf

/usr/bin/fm6000_fibd -i 5 -v &
/mnt/flash/fm6000_wdog -i 10 -g 180 &          # dataplane watchdog, see E0
```

### Three things that each cost a debugging round

- **`PORTD_TXFCS=1` is mandatory.** Without it portd logs `tx_fcs=0` and transmits frames the peer
  silently drops — you see your own frames at the TAP and the peer's frames arriving, and conclude
  the link is fine. It is not. `env VAR=x nohup ...` did **not** take under busybox; set the
  variable in the shell or prefix the command directly.
- **Pass the real MAC, `44:4c:a8:31:5d:ab`.** With the placeholder `x` portd invents a random MAC
  (observed: `82:2f:c8:ff:09:32`). The ASIC's punt tables are programmed for the real one, so
  replies are never punted to the CPU — ARP hangs INCOMPLETE. Worse, the peer caches the bogus
  binding and keeps answering into a black hole after you fix it.
- **`/usr/lib` is not on the loader path.** There is no `/etc/ld.so.conf` and no cache, so `zebra`
  and `ospfd` fail with `libzebra.so.1: cannot open shared object file` even though the libraries
  are sitting in `/usr/lib`.

### And one trap that kills your own session

`kill $(pgrep -f "fm6000_portd")` matches **the ssh command line running it**, so it kills the
session instead of the daemon. Same shape as the `pkill -f "cat /dev/ttyUSB1"` trap. Use
`ps w | grep "[f]m6000_portd" | awk '{print $1}'`.

## Status as left

Everything above is done and verified, and the dataplane still does **not** forward:

- link healthy, tables programmed, `portd` running with `tx_fcs=1` and the correct MAC
- our frames leave the TAP correctly formed — ICMP echo requests with the right MACs and IPs
- the peer's frames arrive normally (OSPF hellos, IPv6 ND)
- **nothing we transmit is ever answered**: ARP goes INCOMPLETE→FAILED, no echo replies, no OSPF
  adjacency, `routes=2`

ARP resolved exactly once, immediately after the MAC was corrected, then lapsed — which is what
put suspicion on the peer's cache holding the random MAC. Waiting it out and flushing did not
recover it.

So the remaining fault is in **CPU→ASIC→wire egress**, not in the tables and not in the link.

## What the egress diagnosis established

Frames are not being dropped on the way out, and the tables are almost entirely correct:

- `CM_PORT_TX_DROP_COUNT` for port 40 (Et1) stays **0** across a ping — congestion management is
  not discarding them. portd has handed the ASIC 400+ frames.
- `GLORT_CAM` at `0x0e000` is programmed, EPL FIFO error status is clean, TX FIFO pointers cycle.
- A 150-address audit of the replay against the live chip: **parser, L3AR, L2AR and FFU are 25/25
  correct.** The replay applies cleanly to every forwarding table we care about.

★ **But two memory classes do not accept plain MMIO writes, and the replay is plain MMIO.**

| region | symptom |
|---|---|
| CM `0x113800`–`0x114500` | replay writes real values (`0x3fff`, `0x15f5`, `0xd6`); chip reads `0x00000000` |
| MOD CAM | `want=0xffffffff got=0x0000ffff` — the upper 16 bits never land |

The MOD pattern is a write-width problem, which is what `fm6000_wr128` exists for. The CM one is
harder: re-running the **full** replay (373,345 ops, `PIN=ok`) left `0x114000` at zero, and
`fm6000_cmfill` — written specifically to fill these "instead of via the (stuck) CRM" — reports its
own readback failing:

```
CM_0x115000  @0x115000 x912  <- 0xffffffff ... done (readback[0]=0xffffffff)   <- takes
CM_0x114000  @0x114000 x1280 <- 0x00003fff ... done (readback[0]=0x00000000)   <- REJECTS
```

Three of the four CM fills stick; `0x114000` refuses writes in the chip's current state. So that
memory needs either the CRM Memory Set walk (`fm6000_crm`) or some enabling state that PROBE MODE
never established. **Whether this is the cause of the forwarding failure is not established** — it
is a real defect found while looking, not a confirmed root cause.

## Getting the lab working again

`boot-config` is already `SWI=flash:/EOS-4.16.8M.swi`, so **a plain reboot returns the box to EOS
and to working forwarding.** Nothing here is destructive; the EdgeNOS state is entirely in RAM.
