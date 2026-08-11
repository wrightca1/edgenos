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

## What you must do by hand

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
