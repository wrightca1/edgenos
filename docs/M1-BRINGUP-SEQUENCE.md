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

---

## ⚠ `edgenos-up.sh` kills mgmt SSH unless the admin network is pinned

**2026-08-13.** The bring-up completes (`=== UP ===`, adjacency in ~16 s, 35 routes, FIB sync) and
the box goes unreachable *at step 4*, mid-run. It is not a crash and not a dataplane fault: the
console shows a healthy shell throughout.

`ospfd` installs ~35 routes via `et1`, and one of them covers the admin subnet:

```
10.22.1.0/24 via 10.101.101.25 dev et1  metric 20
```

That is **more specific than the default route**, so replies to the build host leave by the front
panel instead of `ma1` and never return. Requests still arrive, which is what makes it look like
the box died rather than like a routing change.

Fix — a static route for the admin network, pinned to `eth0` with a better metric:

```sh
ip route add 10.22.1.0/24 via 10.1.1.1 dev eth0 metric 5
```

`init-m1` now does this at boot (`MGMT_PEER`, `MGMT_GW`), before ospfd exists, and the lower metric
keeps it winning afterwards. A plain default route is **not** sufficient — that was tried first and
OSPF's more-specific route beat it.

Verify with `ip route get <admin-host>`; it must name `dev eth0`.

## 2026-08-15: two-port bring-up, and two things alpha9 gets wrong

Transit traffic — the prerequisite A4 and B1 have been blocked on for days — needs **both** front
ports as netdevs. The topology for it already exists and had not been noticed:

```
7150 Et1  10.101.101.26/29  <-->  AS5610 swp6  10.101.101.25/29
7150 Et2  10.101.101.34/29  <-->  AS5610 swp7  10.101.101.33/29
```

Two *different* subnets on the same peer, so a frame in one port and out the other genuinely
transits the switch. And the AS5610 (`10.1.1.238`, root/`as5610`) has **`tcpdump 4.99.4`**, which is
the egress capture point `FEATURE-COMPLETE-CHECKLIST.md` A4 says is missing. It is not missing.

### `edgenos-up.sh` cannot do this on its own

It configures `et1` only — `PORTD_PORTS` selects the ports portd creates, but the `ip link`/`ip addr`
lines for anything else are not there, as its own comment admits. `/mnt/flash/up2.sh` wraps it:
sets `PORTD_PORTS` for both ports, then configures `et2`.

⚠ Passing `PORTD_PORTS` through a one-shot `ssh 'VAR=... sh script'` did **not** take — portd came
up with the default `et1` alone and the only evidence was one line in `/tmp/portd.log`. Check
`/sys/class/net/et2` exists before believing a two-port bring-up happened, and remember portd cannot
be restarted without a chip reset, so a failed attempt costs a reboot.

### ⚠ alpha9 predates the `MGMT_PEER` pin — bring-up still black-holes management

`init-m1` was fixed on 2026-08-13 to pin the admin subnet to `eth0`, but **alpha9 was built before
that fix and does not carry it.** Observed live: `edgenos-up.sh` completes, ospfd forms its
adjacency, and the route table shows

```
10.22.1.0/24 via 10.101.101.25 dev et1  metric 20
```

with no `eth0` route at all — management dies mid-command. Recovery is over serial:

```
ip route add 10.22.1.0/24 via 10.1.1.1 dev eth0 metric 5
```

`up2.sh` now does this immediately after `edgenos-up.sh` returns. **The real fix is a rebuild**; any
image older than 2026-08-13 has this hole, and the initrds already on flash all predate it.

### ⚠ Repeated mid-FULLSEQ reboots

Three consecutive boots reset partway through the sequence — uptime back to 1 min, both ports still
at `0x0015`, the log restarting from `STEP1`. That is worse than the documented "fails 1 boot in 6"
and it is not the dataplane watchdog: `fm6000_wdog` is not started at boot (checklist E0a) and
`/mnt/flash/wdog.off` is present anyway.

Do not diagnose this by boot-cycling. The console is the instrument — the kernel carries
`nmi_watchdog=panic` and `reboot=p`, so a wedge reboots the box and the reason is printed on
`ttyUSB2` and nowhere else. Watch it while the sequence runs.

## 2026-08-15: the punt ring delivers entries that are not frames — SOP/EOP is unhandled

The `fm6000_rxdump` capture taken to settle the F64 tag question showed something else worth having:
**3 of 7 ring entries are not frames.**

```
[0] len=90  33 33 00 00 00 05|80 a2 35 81 ca b4|07 01|03 ef 00 01 ff ff|86 dd   proper
[1] len=12  0a 65 65 f1 00 00 00 00 48 5d 93 f6                                 12 bytes
[2] len=90  01 00 5e 00 00 05|80 a2 35 81 ca b4|07 01|03 ef 00 01 ff ff|08 00   proper
[3] len=82  ... proper
[4] len=20  01 00 00 13 00 0a 00 28 0a 65 65 f1 ...                             20 bytes
[5] len=71  ... proper
[6] len=19  0a 02 01 00 00 00 28 0a 65 65 19 00 ...                             19 bytes
```

The short entries carry recognisable data from this network — `0a 65 65 f1` is `10.101.101.241`,
`0a 65 65 19` is `10.101.101.25`, the peer's swp6 address — so they are real bytes, not noise, but
they are not Ethernet frames: no plausible DMAC/SMAC/ethertype structure.

**The receive path never looks at SOP/EOP, and says so.** `fpdma.c`:

```c
/* HW sets status bit2 (FM6000_DESC_DONE) when it fills a descriptor
 * (same done-bit as TX, per fpr_reclaim). TODO(live-trace): SOP/EOP/error */
if (!(status & FM6000_DESC_DONE))
```

`fm6000_rxdump.c` is the same: `if(!(d[0]&0x04)) continue;`. Yet the descriptor format documents the
bit — `FM6000_DESC_HANDOFF 0x09` is commented "READY(0)+EOP(3)" — so **bit 3 is EOP and a frame can
span descriptors**. Every completed descriptor is currently treated as a whole frame.

⚠ **Stated as a hypothesis, not a finding:** if frames can span descriptors, the short entries are
continuation buffers and the receive path is delivering fragments as frames while dropping the rest.
portd counts them as `n_rx_drop` and discards them silently, so nothing has ever surfaced it.

**This is worth testing against D5.** The checklist's oldest unexplained defect is *"ping collapses
to 100% loss within ~3 minutes, on EOS's own parser and the stock replay too, still unroot-caused;
suspicion is the portd DMA ring."* A receive path that mis-assembles multi-descriptor frames would
produce exactly that, and the suspicion was already pointing here.

⚠ Do not treat that connection as established — today has produced several confident chains that
did not survive their own tests. The check is cheap and specific: log the full status byte per
descriptor, not just bit 2, and see whether the short entries have EOP clear while the proper frames
have it set. One capture, no reboot beyond the one it rides on.
