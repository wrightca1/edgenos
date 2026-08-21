# 7150 hardware harnesses (2026-08-15/16)

Scripts that drive the 7150 over the network. They assume `sw.sh` (EOS CLI),
`eg.sh` (EdgeNOS shell) and `p5.sh` (the AS5610 peer) exist alongside them —
those hold credentials and are not in the repo.

## The rig

```
7150 Et1  10.101.101.26/29  <-->  AS5610 swp6  10.101.101.25/29
7150 Et2  10.101.101.34/29  <-->  AS5610 swp7  10.101.101.33/29
```

Two different /29s on one peer, so a `/32` route on the peer forces a hairpin —
in Et2, routed, out Et1 — and `tcpdump` on swp6 captures **what the switch
emitted**. That is the observation A4 and B1 were blocked on. The peer is
`<peer>` and has `tcpdump 4.99.4`.

## What each script is for

| script | question it answers |
|---|---|
| `et2-baserate.sh` | how often does Et2 link? (measured: 5 of 10 identical boots) |
| `hunt-et2.sh` | reboot until Et2 is up, archiving every boot's settle trace |
| `cmp-traces.sh` | diff a good boot's trace against a dark one, bit 10 masked |
| `rxdump-test.sh` | are punted frames F64-tagged? (needs portd stopped) |
| `et2-demux-test.sh` | does per-port RX demux work? guards against a false negative |
| `transit-test.sh` | does traffic transit, and what does the switch emit? |
| `a4-leaveout.sh` | clear ONE MOD step's Valid bit and read the wire |
| `a4-slice-sweep.sh` | which MOD slice performs a given edit (disable a whole slice) |
| `transit-probe-hex.sh` | what bytes did the switch actually emit? (raw hex, not `ttl NN`) |
| `mod-perturb.sh` | one guarded MOD perturbation: save-verify, restore, health-check |
| `mod-slotsweep.sh` | which *entry* in a slice fires (only one does; slot 9 in slices 14–16) |
| `mod-bisect.sh` | same, by bisection — 5 probes instead of 29 for a full bank |
| `fm6000-status.sh` | live port/lane state off the chip, no reboot |
| `et2-goodboot-measure.sh` | reboot until Et2 links, then measure whether the link holds |
| `resolve-teardown.py` | turn an in-replay trace into the writes in a suspect window |

## Rules these scripts encode, each learned the hard way

- **A single boot measures nothing.** Et2 links on ~half of identical boots, so
  any "X makes it work" claim needs n per arm and a reported count. At that rate
  separating two arms needs ~32 boots each, so **design experiments to be
  falsified** — a hypothesis predicting a *zero* costs 5 boots, one predicting a
  shift costs a day.
- **A TAP's `carrier` is not a link signal.** portd's TAP reports carrier=1 as
  soon as it is up, whatever the lane is doing. Read `PORT_STATUS` at `0xe4000`.
- **`PORTD_DEBUG=N` dumps N frames.** With N=1 the one frame you see may be a
  runt, and generalising from it is how the punt-tag diagnosis went wrong.
- **Verify a save before perturbing, and health-check between tests.** The first
  `a4-slice-sweep.sh` guarded its 32-value capture with "non-empty", wrote zeros
  over a live table, and then reported five more slices as broken when it was
  the same wedge.
- **Restore is not recovery.** A restored table does not un-wedge a dataplane in
  flight. Budget a reboot.
- **`MOD_COMMAND_RAM` takes writes after boot** (unlike `PARSER_INIT_FIELDS`), so
  these perturbations are real until the next boot — which is also why a reboot
  always fixes them, since FULLSEQ reprograms the table.

## Reading link state: `fm6000-status.sh`

Runs **on the switch**. Reads `PORT_STATUS`, `LANE_STATUS`, `pcsRx`, `TX_CFG`, `RX_CFG`, `LINK_IM`
and `LINK_IP` straight off the chip via `devmem`, any time, without a reboot.

```
sh /tmp/fm6000-status.sh 1 2 3
```

Register space is indexed in 32-bit **words** — `fm6000_hw.c`'s `rd()`/`wr()` do `M[w]` on a
`uint32_t *` — so the byte address `devmem` wants is `BAR0 + word*4`. BAR0 is read from sysfs, not
hardcoded; it has moved between boots.

- **A lane is up when `LANE_STATUS == 0x940`.** `PORT_STATUS` alone is not enough: its low bits are
  LinkFault fields that flicker on a perfectly healthy port (Et1 alternates `0x8c0`/`0xcc0`/`0x8ea`).
- Bit 8 of `PORT_STATUS` is `HiBer`. A lock with `HiBer` set is a bad lock, not a link.
- There is no `scp` on the box. Install with `eg.sh 'cat > /tmp/x.sh' < x.sh`.

Before this existed, every link reading came from `/mnt/flash/fullseq.log`, which is written once per
boot and truncated by the next. That is how two dark traces were lost.

## Driving a lane up: `fm6000_lanelink <front-panel-port>`

Drives one lane post-boot. Port 2 is Et2. Lock appears several seconds *after* the tool exits —
training is asynchronous, so read status on a delay, never immediately.

⚠ **On Et2 it does not produce a working link, and it does not reproduce a good boot.** The lane
reaches `LANE_STATUS=0x940` and then oscillates in and out of lock indefinitely (13 of 22 samples,
against a simultaneous Et1 control at 22/22). Do not score it with a single read: that samples the
flap and reads as a success rate. Any number you take here must be a **duty cycle over a stated
window with a live Et1 control beside it**.

Measured on fresh boots, there are three distinct states, and the third one is ours:

| state | `PORT_STATUS` | duty | `HiBer` |
|---|---|---|---|
| good boot | `0x08C0` | **60/60** over 5 min | clear |
| dark boot | `0x0815` | **0/20** | — |
| `lanelink`-driven | `0x09D5` | 13/22 | **set** |

A fresh boot is unambiguous in either direction — the flapping is an artifact of driving the lane, not
something a boot does. So treat `lanelink` as a recovery hack, never as a model of how bring-up works.

- ⚠ **Never run it on a lane that is already locked — it tears the link down.** Check
  `fm6000-status.sh` first. A retry loop must test before every attempt, not just at the end.
- ⚠ Its locks are marginal (`HiBer` set) where a good boot's are clean. Do not treat a `lanelink`
  lock as equivalent to a boot lock when it is the thing under measurement.
- `-n` is a dry run and prints the port's EPL/lane/SBus mapping without touching the chip. Use it to
  confirm a mapping is *observed* rather than extrapolated before driving anything.

## Resolving a trace to writes: `resolve-teardown.py`

```
python3 resolve-teardown.py <fine-DARK.log> <fwd-executed.txt>
```

Takes an in-replay sample log and the replay that **actually executed**, finds the window between the
last locked sample and the first unlocked one, and classifies the writes in it by MMIO region and
SBus device.

⚠ **Op numbers index the generated replay, not `/mnt/flash/fwd4.txt`.** FULLSEQ regenerates 18 blocks
and executes the result; indexing the reference capture instead silently misattributes every write.
`fm6000-fullseq.sh` preserves the executed file as `/mnt/flash/fwd-executed.txt` for exactly this.

⚠ **Always resolve the same window against a good trace too.** The first run of this pointed at 1,024
writes that looked decisive until the good boot was shown to execute the identical writes with the
link up.

## ⛔ Rebooting the 7150 into EdgeNOS — three traps, all silent

Any harness that reboots between trials must do all three of these. Missing any one produces data
that looks fine and means nothing.

**1. `reboot` does nothing.** PID 1 under EdgeNOS is `/bin/busybox sh` — a plain shell, with no
reboot handler. BusyBox `reboot` signals it, returns **rc=0**, and the box keeps running. Six
consecutive "boot attempts" once ran against a single box that had been up 8 hours; the only clue was
`/proc/uptime`. Use `reboot -f`, which calls the syscall directly:

```sh
sync; mount -o remount,ro /mnt/flash; sync; (sleep 2; reboot -f) &
```

`/mnt/flash` is unjournaled **vfat** and there is already an `FSCK0000.REC` there from a past
corruption, so remount it read-only before forcing. Write anything you need *before* the remount.

**2. `boot-config` resets to EOS on every EdgeNOS boot.** It is a one-shot safety: a failed boot
cannot strand the box. So an unplanned reboot lands on **EOS 4.16.8M**, not EdgeNOS, and the symptom
is dropbear gone with the box still pinging. Rewrite it before *every* reboot, while flash is rw:

```sh
printf 'SWI=flash:/edgenos-7150-0.3.0-alpha16.swi\n' > /mnt/flash/boot-config
```

To get back from EOS, do the same via `sw.sh 'bash sudo sh -c ...'` and reboot.

**3. `/mnt/flash/fullseq.log` survives the reboot.** The new boot does not truncate it until FULLSEQ
actually starts writing, so grepping for `FULLSEQ DONE` right after a reboot matches the **previous**
boot's marker and its stale `final et1=` line. Sampling then runs mid-replay with both ports still
dark. Delete the log before rebooting and treat a `DONE` as fresh only then.

**And check the control.** Et1 reads 20/20 and 60/60 on every valid sweep. If a sweep reports Et1
below full, the sweep is invalid — it ran mid-replay or the box is unhealthy. Discard the data point;
do not record it as a dark boot. That single guard is what caught trap 3.

## MOD command identification: swap, don't disable

`transit-probe-hex.sh` emits one transit frame and prints the switch's **egress bytes** as one
contiguous hex string, so byte N is chars 2N..2N+1. TTL is byte 22. Reading raw bytes rather than
`ttl NN` is essential: TTL alone cannot separate "the decrement was removed" from "the decrement
moved six bytes", which is the distinction the whole A4 question turns on.

`mod-slotsweep.sh <bank> <slot>...` disables one entry at a time; `mod-perturb.sh bank|slot ...`
does one perturbation with save-verify, bit-identical restore, and a health check.

**Find the firing entry first.** Only one entry per slice fires, and for the ICMP transit flow it is
**slot 9 in slices 14, 15 and 16** — 14 of 15 live entries in bank 15 are inert. Three hand-picked
leave-one-out tests once "refuted" `{0x20, 0xe0}` by disabling copies that never fire.

⚠ Slot 9 is a starting guess for other slices, not a rule: **bank 1's slot 9 is empty**, so the index
is not uniform. Sweep the slice rather than assuming.

⚠ **Disabling does not work in slices 15 and 16 — it drops the frame**, whether you clear one entry
or the whole bank. Those slices require a CAM match. Only slice 14 shows the "edit moves, frame
survives" signature. **Swap the command byte instead, keeping the Valid bit set**: the entry still
matches, and only the operation changes. That is how `0xe0` was identified as `DECREMENT`.

⚠ **Do not read meaning into these operands.** `0xe0`/`0xe1`/`0xe2` behave identically, as do
`0x20`/`0x22`/`0x24`. The cursor carries the position; the operand is unused for these opcodes.

Restoring MOD entries is safe (`MOD_COMMAND_RAM` takes writes after boot) but a wedge costs the boot,
and Et2 only comes up on ~1 boot in 2 — budget accordingly.

### The transit rig needs a static ARP entry now

Et2's **CPU punt path is broken** on current images: `et2 rx=0` while `tx` climbs, so the peer cannot
ARP the switch's Et2 address and never sends the transit traffic at all — the capture comes back
empty and looks like a forwarding failure. Hardware forwarding is fine. Pin the next hop on the peer:

```sh
ip neigh replace 10.101.101.34 lladdr 44:4c:a8:31:5d:ab dev swp7 nud permanent
```

Also note `edgenos-up.sh` is **not** started by `init-m1` — run it by hand after boot, and check
`ip route` shows `<admin-net-host>/24 via <mgmt-net-host> dev eth0 metric 5` first or ospfd will take mgmt away.

## ⛔ Do NOT write `MAPPER_DMAC_CAM` (`0x123xxx`) — it breaks my-MAC chip-wide and only a reboot fixes it

`MOD_COMMAND_RAM` and `MOD_VALUE_RAM` take direct `devmem` writes and restore cleanly. **`MAPPER`
does not.** A single word written to `0x123106` (`MAPPER_DMAC_CAM1` entry 1, word 2):

- **read back as the ORIGINAL value** — the write appeared not to land at all, and
- **broke my-MAC recognition chip-wide anyway**: transit stopped, and the switch also stopped
  answering pings on its own `Et1` address. Both links stayed `LOCKED`; the dataplane did not.

Recovery attempts that did **not** work: rewriting all four words of the entry in order;
`fm6000_mapperpre`; `fm6000_mapperinit` (361 writes). **Only a reboot restored it**, because FULLSEQ
reprograms the block from scratch.

The table is 3 words wide with stride 4 (`MAPPER_DMAC_CAM1_WIDTH 3`), and like `NEXTHOP` it is
evidently a staged/commit structure where the readable words are not the live array. A partial write
commits garbage into the CAM while the staging copy still reads correct — the worst possible
combination, because the readback check that catches every other silent-write bug passes here.

**Rules:**
1. Do not write any `MAPPER` address without establishing its commit semantics first.
2. `NEXTHOP` (`0x160000`) needs **both** words of a 64-bit entry written, low then high, or the write
   is silently discarded — that one *does* read back as unchanged, honestly.
3. A readback that matches the value you wrote proves the write landed. A readback that matches the
   **old** value proves nothing about whether the hardware saw it.
4. Budget a reboot before touching an uncharacterised table, and check the *whole* dataplane
   afterwards — not just the flow under test. The `Et1` ping is the cheapest my-MAC canary.

## ⛔ `LANE_STATUS == 0x940` IS NOT A LINK — check `HiBer` too

This document already said *"a TAP's `carrier` is not a link signal — read `PORT_STATUS`"*. That was
right and incomplete: **`LANE_STATUS` is not a link signal either.**

Measured 2026-08-18, Et2 with a solid `LANE_STATUS=0x940` on 16 of 16 samples, forwarding nothing:

```
Et1  PORT_STATUS=0x0EC0  pcsRx=0x1   rx=354   working
Et2  PORT_STATUS=0x09D5  pcsRx=0x67  rx=0     HiBer -- locked, garbage
```

`PORT_STATUS` bit 8 is `HiBer`. Set, it means the PCS achieved block lock over a channel with a bit
error rate too high to carry frames. `pcsRx` reads `0x67` instead of `0x1`.

**Score a link as up only when all three hold:**

```
LANE_STATUS == 0x00000940      block lock
PORT_STATUS bit 8 == 0         not HiBer
pcsRx == 0x00000001            clean receive
```

…or better, score it on **traffic** — `transit-probe-hex.sh`, or `rx_packets` moving. A duty cycle
built on `LANE_STATUS` alone counted `HiBer` locks as links and reported 16/16 for a dead port.

⚠ `fm6000_lanelink` is a known producer of this state (see its section above: `0x09D5`, flapping).
Anything that calls it — including FULLSEQ's `STEP5b` retrain — can leave the lane locked and unusable,
and a `LANE_STATUS` check will not notice.

`fm6000-status.sh` now prints `LOCKED-HiBer(no traffic)` for this case.
