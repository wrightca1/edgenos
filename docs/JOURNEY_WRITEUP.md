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

## Part 3 — 40-gig QSFP: where we are honest about the wall

The 40G QSFP ports are the unfinished frontier. Real progress, real wall:

**What works:**
- All four QSFP optics (40GBASE-SR-BiDi, an unusual duplex-LC/WDM module — not the
  typical 4-fiber MPO) are **detected and their EEPROMs read** over the muxed I²C
  buses. Getting there meant decoding the platform's GPIO/reset/mod-select scheme
  (which turned out to be *reverse-ordered* from what the schematic implied) and the
  device-tree mux configuration.
- 40G **persists across reflashes**, and we built per-lane equalization diagnostics.

**What doesn't (yet):**
- A 40G link needs all **four PCS lanes** to align and de-skew. We reliably get
  **2 of 4 lanes** to lock; the other two won't alignment-marker-lock.
- We chased and *ruled out* the usual suspects: lane swap/remap, polarity, the
  KR4-vs-X4 forced modes (X4 actually made it *worse* — it disabled the RX
  auto-adaptation we needed).
- **Root cause (pinned, not yet fixed):** the open toolkit (OpenMDK) is a *partial*
  reimplementation of Broadcom's full Warpcore bring-up. It lacks the **per-lane RX
  calibration layer** that the full SDK / Cumulus runs (`independent_lane_init`). The
  two stubborn lanes need RX equalization that our code never performs. We have the
  two closure paths identified: port the missing calibration sub-routines, or capture
  and replay a cold-init sequence from the working reference. That's the next mountain.

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
- 🟡 40G QSFP: optics detected/persisted, **2 of 4 lanes lock**; missing per-lane RX
  calibration is the identified blocker.
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
> The 40-gig optical ports are the honest unfinished part — all four optics detect, but
> only 2 of 4 lanes lock. We've pinned the root cause (the open toolkit is missing a
> per-lane receiver-calibration step the full vendor stack runs) and have the path
> forward.
>
> Bringing up silicon from scratch is 10% heroics and 90% building the instruments
> that let you see. More soon.
