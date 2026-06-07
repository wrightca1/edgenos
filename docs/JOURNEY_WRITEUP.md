# Building a switch OS from the silicon up — field notes

A running write-up of what it actually takes to bring a bare-metal data-center
switch to life with a from-scratch network OS — the wins, the dead ends, and the
bugs that hid behind other bugs. Source material for a LinkedIn update; redact
internal IPs/hostnames before posting if needed.

---

## The project in one paragraph

We're building **EdgeNOS**, a network operating system written from scratch, and
bringing it up on an **Edgecore AS5610-52X** — a 52-port data-center switch built
around a **Broadcom BCM56846 (Trident+)** switching ASIC. No vendor SDK black box:
we drive the chip through **OpenMDK** (Broadcom's *partial* open toolkit) plus a lot
of reverse-engineering, using a known-good **Cumulus Linux** install on the same
hardware as a "what should the chip look like" reference. The goal: take a switch
that only ever ran a commercial NOS and make our own software forward real packets.

---

## Part 1 — First light on the 10-gig ports

The first milestone was getting a **10G SFP+ port** to come up and pass traffic to a
real neighbor (a Cisco Nexus). That sounds simple. It is not, when you own every
register.

What it took:
- **Link/L1:** the SFP+ path runs through **DS100DF410 retimers**. Getting the
  Warpcore SerDes to actually lock (PCS `block_lock`) meant tuning the retimers and
  finding non-obvious settings — e.g., the optics stayed *muted* until specific
  equalization/PRBS settings were applied. We learned to **capture what Cumulus does
  via GDB and register dumps** and replicate it, rather than guess.
- **DMA:** moving packets between the CPU and the chip is its own project. We
  reverse-engineered Cumulus's user-mode DMA model (no kernel KNET, interrupt-driven,
  a 64-descriptor ring) and rebuilt it. There were multiple walls here — the RX path
  alone went through several rewrites before frames reliably reached the CPU.
- **The forwarding plumbing:** port maps, VLANs, L2/L3 tables, CPU "punt" rules — all
  programmed by hand against the chip and cross-checked against the Cumulus baseline.

Payoff: the box **pinged a live Cisco Nexus, 0% loss**, on its own software. Then a
reflash (for the 40G work) and a clean reboot knocked it back over — which kicked off
the saga in Part 2.

---

## Part 2 — "Why won't it ping?" — three bugs wearing a trench coat

After a cold reboot, ICMP to the Nexus was dead again. ARP worked. The links were up.
Frames were moving. But ping was 100% loss. This turned into a long, layered hunt —
the kind where every answer reveals the *next* wrong assumption.

The honest arc:
1. **First wrong theory:** "our address-config service is buggy." Found a real bug
   (a service that restarted the switch daemon *after* setting IPs, which destroyed
   the very interfaces it had just configured) — fixed it. Ping still dead.
2. **Second wrong theory:** "the chip isn't punting received packets to the CPU."
   Built an **on-chip diagnostic** (trigger it with a signal, it dumps per-stage
   hardware drop counters, table contents, even hardware "hit" bits) and proved the
   inbound packet was being **dropped at the L3 stage** — every IPv4 frame to our own
   IP discarded as `RIPD4`. Spent a long time ruling things out: VRF, VLAN profiles,
   TCAM enables, even installing a *match-everything* route whose hardware hit-bit
   **never set** — proving the L3 route lookup wasn't running at all.
3. **The real bug #1 — wrong table.** Reading Broadcom's full SDK source, the per-port
   "do IPv4 routing" enable lives in one table (`PORT_TAB`); our code had been writing
   it into a *different* look-alike table (`LPORT_TAB`, a profile table the chip wasn't
   even consulting for these ports). One letter of difference, total silence from the
   hardware. Fixed it → the chip finally **routed the packet and the CPU replied**.
   Confirmed at the wire with packet captures.
4. **Ping still failed... but now for a *different* reason.** New symptom: tiny pings
   worked, normal pings didn't. A frame-size bisection (pinging from the Nexus at
   different payload sizes) showed the boundary was exactly at the **minimum Ethernet
   frame**. The real bug #2: the chip's MAC was in **"CRC-replace" mode** — it
   overwrites the **last 4 bytes** of every outgoing frame with the checksum instead
   of *appending* it. Small frames have padding to spare; full frames lost 4 bytes of
   real data, so the neighbor received a perfectly-valid-looking frame whose IP packet
   was 4 bytes short — and silently dropped it. Fix: pad 4 bytes of slack so the MAC
   eats padding, not data.
5. **Real bug #3 — MTU.** Full 1500-byte frames still failed: the chip's frame-size
   limit was set to 1522 while the interfaces were configured for a 1600 MTU. Bumped
   the chip limit to match.

**Result: bidirectional ping, 0% loss, on both uplink ports, for all normal frame
sizes including full standard-MTU frames** — and verified rigorously that it's
genuinely going through the switch silicon (routing table, wire captures with the
neighbor's real MAC, neighbor-initiated pings that can *only* reach us through that
port, and zero leakage onto the management interface).

The theme of Part 2: **"it works" is a claim you have to earn.** Every apparent fix
was real and necessary, and *still* not the whole story. The on-chip diagnostic — and
disciplined before/after counter deltas — is what turned guesswork into a bisection.

---

## Part 3 — 40-gig QSFP: the bug that hid behind another bug

The 40G QSFP ports were the last frontier — and the most instructive, because the
"wall" turned out to be two bugs stacked so they masked each other. **Solved
2026-06-07: all four lanes lock and the port forwards 40G in both directions.**

**What works:**
- All four QSFP optics (40GBASE-SR-BiDi, an unusual duplex-LC/WDM module — not the
  typical 4-fiber MPO) are **detected and their EEPROMs read** over the muxed I²C
  buses. Getting there meant decoding the platform's GPIO/reset/mod-select scheme
  (which turned out to be *reverse-ordered* from what the schematic implied) and the
  device-tree mux configuration.
- 40G **persists across reflashes**, and we built per-lane equalization diagnostics.
- **All four PCS lanes align and de-skew; the link forwards traffic** — verified by
  injecting raw frames on swp49 and capturing them byte-for-byte on swp50 across the
  loopback (and the reverse).

**The two bugs that masked each other:**
- For weeks it looked like only **2 of 4 lanes** would lock, and we built an entire
  theory around it: OpenMDK is a *partial* reimplementation of Broadcom's Warpcore
  bring-up, so it must lack the per-lane RX calibration the full SDK runs
  (`independent_lane_init`). We were ready to port cal sub-routines or replay a
  Cumulus cold-init. **That theory was wrong.**
- **Bug 1 — frozen adaptation.** We were setting the Warpcore to SR4 firmware mode
  (`fw_mode=0x1111`), which *freezes* the SerDes RX auto-adaptation. The fix was the
  opposite of what we'd been attempting: `fw_mode=0` — let the firmware adapt freely.
  Suddenly all four lanes were converging.
- **Bug 2 — we were misreading "locked."** Our link check required the alignment-lock
  field to read `0xf`. But that field is a *state-machine value*, not a per-lane
  bitmap — and **`0x6` is the locked state**. The chip had been reporting `0x6`
  (exactly matching Cumulus's working 4/4) while our code called it "not locked." A
  one-constant decode bug had us staring at a healthy link and seeing failure.
- The lesson: when a result is stuck at a suspiciously clean fraction (2 of 4), suspect
  your *measurement* before you build a theory around the silicon. The fix was a frozen
  mode flag and a misread status field — no missing calibration layer at all.

---

## The methods that actually moved the needle

- **A working reference is gold.** Having Cumulus on the same silicon meant every
  "what should this register be?" had a ground-truth answer. We captured 600+ MB of
  its live chip state — register dumps, table contents, init traces — and mined it
  offline.
- **Build your own visibility.** The single most valuable tool was a custom in-software
  chip diagnostic that dumps per-stage hardware drop counters and table/hit-bit state
  on demand. Black-box "it doesn't ping" became "the L3 lookup stage isn't executing,
  here's the counter that proves it."
- **Read the source.** When register-level guessing hit its limit, the answer
  (PORT_TAB vs LPORT_TAB) came from reading Broadcom's full SDK and diffing intent
  against our code.
- **Earn your conclusions.** Counter deltas before/after, wire captures on both ends,
  and tests the topology can't fake (a neighbor pinging an address only reachable
  through the port under test). "Ping works" only counts when you've ruled out every
  way it could be lying to you.

---

## Scoreboard

- ✅ 10G SFP+: links up, L2/L3 forwarding, **bidirectional ping to a Cisco Nexus,
  0% loss, full-MTU** — on a from-scratch NOS driving the chip directly.
- ✅ L3 datapath cold-boot bug: **3 root causes found and fixed**, verified end-to-end.
- ✅ 40G QSFP: optics detected/persisted, **all 4 lanes lock, forwards both directions**.
  The long "2 of 4" was a frozen-adaptation flag (`fw_mode=0x1111`) + an `am_lock==0xf`
  decode bug (`0x6` is locked) — *not* the missing-calibration wall we'd theorized.
- 🧰 Built a reusable on-chip diagnostic and a reproducible cross-compile/build flow
  along the way.

The big lesson: bringing up silicon from scratch is less about heroics and more about
**building the instruments that let you see**, keeping a ground-truth reference, and
refusing to believe "it works" until the evidence is overwhelming.

---

## A LinkedIn-ready condensed version (edit to taste)

> Spent the last stretch bringing a data-center switch to life on a network OS we
> wrote from scratch — driving the Broadcom switching chip directly, no vendor black
> box.
>
> The 10-gig ports now forward real traffic: bidirectional ping to a Cisco Nexus, 0%
> loss, full MTU. Getting there meant a brutal-but-fun bug hunt where "fixed it" kept
> being wrong three times in a row — the packet was being dropped inside the chip's
> L3 pipeline because an enable bit was written to a look-alike table the hardware
> wasn't even reading; then the MAC was overwriting the last 4 bytes of every frame
> with its checksum (small pings worked, big ones didn't — the tell); then an MTU
> mismatch. Each one looked like the answer. None of them was the whole answer.
>
> What made it tractable: building our own on-chip diagnostics to actually *see* where
> packets die, keeping a known-good reference on the same hardware to diff against, and
> refusing to trust "it works" until packet captures and the neighbor's own counters
> agreed.
>
> And the 40-gig optical ports now forward too — all four lanes lock, traffic both
> ways. That one was the best lesson of the whole project: for weeks it looked like
> only 2 of 4 lanes would ever lock, and we built an elaborate theory that the open
> toolkit was missing a vendor calibration step. Wrong. It was a mode flag freezing the
> receiver's auto-tuning, plus our own code misreading the "locked" status register —
> we'd been staring at a healthy link and calling it broken. When a number sticks at a
> suspiciously clean fraction, doubt your measurement before you blame the silicon.
>
> Bringing up silicon from scratch is 10% heroics and 90% building the instruments
> that let you see. More soon.
