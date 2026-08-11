# Bringing up front-panel port 3 on EdgeNOS

**2026-08-11.** Front-panel port 3 is now up under EdgeNOS. This is the second connected port, and
it is what unblocks the transit traffic that A4 (MOD command split) and B1 (FFU ByteMux) need —
neither can be settled with CPU-terminated traffic alone.

## The port

From `asic/fm6000/fm6000_serdes_ports.h`:

```
{ 1, 40, 14, 0, ... }   front-panel 1 = alta 40, EPL14 lane 0   (Et1)
{ 3, 41, 14, 1, ... }   front-panel 3 = alta 41, EPL14 lane 1
```

Port 3 is **the same EPL as Et1, one lane over**, and both share `rxpol=0, txpol=1`. EPL14 is
already up and clocked for Et1, which is why this is lane-level work and not a full EPL bring-up.

## The recipe

Not derived from first principles — **read off a working EOS boot**, which has the port up as
`Et3 connected 10GBASE-SR` because the test host is plugged in. See `EOS-VS-EDGENOS-DIFF.md` for
how that reference was captured.

```sh
# 1. enable EPL port 1 and select its PCS  (EPL14 shared config)
fm6000reg 0000:02:00.0 0xe3b01 0x7e1d7899     # EPL_CFG_A: bit 20 = Active_1
fm6000reg 0000:02:00.0 0xe3b02 0x00090033     # EPL_CFG_B: bits 4-7 = Port1PcsSel = 3

# 2. apply EOS's lane-1 block, 0xe3880-0xe38ff (30 non-zero words)
fmload lane1.txt
```

`EPL_CFG_A` bit 20 and `EPL_CFG_B` bits 4–7 were identified from the header
(`FM6000_EPL_CFG_A_b_Active_1`, `FM6000_EPL_CFG_B_l_Port1PcsSel`) — the two bits are the *only*
genuine configuration differences between EOS and EdgeNOS in the whole EPL region.

⚠ Step 1 alone is **not** sufficient: with only `Active_1`/`Port1PcsSel` set the lane stayed at
`0x00000015`. The lane-1 block carries the rest.

## Result

```
PORT_STATUS(EPL14, lane1) = 0xe3880
   before: 0x00000015   LinkFaultDebounced + LinkFaultMac + LinkFaultRx, RxLinkUp clear
   after:  0x000008c0   RxLinkUp, HeartbeatOk, SerXmit      <- stable over 30s
```

Et1 is unaffected throughout (`lane0` alternating `0x8c0`/`0xec0`, which is the Receiving bit
toggling on live traffic), `routes=34`, `PIN_STRAP=0x208`.

## Ingress is proven working

Controlled measurement — the test host given `10.99.99.2/24` on eth1 and made to transmit, while
`PORT_STATUS(EPL14, lane1)` is sampled once a second:

```
host idle:          0x8c0 0x8c0 0x8c0 0x8c0 0x8c0 0x8c0
host transmitting:  0xcc0 0xcc0 0xcc0 0xcc0 0xcc0 0xcc0     <- bit 10, Receiving
```

Six consecutive samples each way. The ASIC receives the test host's frames on port 3. The RX half
of the path is done.

**Nothing egresses yet**: `tcpdump -i eth1` on the test host captured 0 packets over 12s, and its
`rx_packets` counter did not move. Port 3 has link and receives, but is in no forwarding domain.

## What is still needed for transit traffic

Link is up; forwarding through it is not configured.

- **GLORT.** `PARSER_INIT_FIELDS[41]` reads `0x00010103` — logical id `0x103`, default SGLORT
  `0x0001`. **EOS leaves it at the default too**, because Et3 is a switched VLAN-1 port rather than
  a routed one; `GLORT-MAPPING.md` records that only configured-up routed ports (Et1 `0x03ef`,
  Et2 `0x03ee`) get real GLORTs. So a unique SGLORT plus a `GLORT_CAM` entry is needed only if we
  want to *inject* on port 3 from the CPU.
- **For transit we may not need that at all.** Wire→wire forwarding does not involve the CPU: a
  frame ingressing port 3 and egressing Et1 exercises the full pipeline including MOD, with no
  portd on port 3.
- **The capture direction that matters** is `peer → Et1 → switch → port 3 → test host`, because
  the test host is ours and can run tcpdump. That is the egress observation A4 has been blocked on
  since the beginning — MOD's edits become directly visible.

## Note on signals

`routes=34` is the reliable health metric here, not ping. Ping collapses to 100% loss on this box
for unrelated reasons (checklist D5) — it did so during this very session while OSPF was converged
with 34 routes and the FIB programmed. Do not read a ping failure as a forwarding failure.


---

## 2026-08-11, pulling the egress thread: three findings

### 1. The DGLORT→DestMask path is not populated at all

`GLORT_RAM.DMaskBaseIdx` indexes `FM6000_MCAST_DEST_TABLE` (`0x240000`, 4096 entries × 4 words,
`DestMask[75:0]` — one bit per port, so bit 40 = Et1, bit 41 = port 3).

Dumped all 4096 entries on a live chip with Et1 forwarding: **every word is zero.** No entry
selects port 40, or any port. So the chain GLORT_CAM → GLORT_RAM.DMaskBaseIdx → MCAST_DEST_TABLE
is not how a CPU-injected frame reaches Et1 — it cannot be, because the table is empty.

Combined with the earlier result that no `GLORT_CAM` entry matches `0x03ef` at all, the conclusion
is firm: **the ISL/F64 DGLORT does not resolve through the GLORT/DMask machinery.** Whatever
carries a CPU-injected frame to its egress port is something else, and both of the obvious
candidates are now eliminated by measurement rather than argument.

### 2. portd instances share DMA rings — you cannot run two

Running a second portd for `et3` alongside the `et1` one broke the dataplane: `routes` fell
34 → 3 while the et3 instance consumed **1,707 frames**. Killing it restored `routes=35`
immediately.

`edgenos-up.sh` warns about this for a *second run of the script*; the same hazard applies to a
second portd on a **different port**, because the rings are shared and **not demultiplexed by
port**. The second instance simply steals frames the first one needed.

So port 3 cannot be given its own portd as things stand. Supporting two CPU-attached ports needs
portd extended to demultiplex one ring set across ports — presumably on the ISL tag's source
GLORT, which is exactly what `PARSER_INIT_FIELDS[port]` stamps on ingress.

### 3. ⚠ RETRACTED: port 3's ingress does NOT reach the CPU

I originally read the 1,707 frames the et3 portd consumed as proof that port 3's receive path
worked end to end. **That was wrong, and it followed from finding (2).** The DMA rings are shared
and *not demultiplexed by port*, so an et3 portd reads whatever is in the ring — which was Et1's
traffic. The count proved the rings were being drained, not where the frames came from.

Direct check with `fm6000_rxdump` and no portd running, while the test host transmitted: **every
punted frame carries tag word 1 = `0x03ef`**, Et1's SGLORT. Nothing from port 3 is punted at all.

What is established for port 3 is the MAC-level measurement only: `PORT_STATUS` bit 10 `Receiving`
tracks the test host's transmission exactly (`0x8c0` idle, `0xcc0` transmitting, six samples each
way). The MAC receives; the frames go no further, because port 41 is in no forwarding domain.

## The punt frame format, decoded

`fm6000_rxdump` with no portd competing shows the real layout — and it is simpler than portd
assumes:

```
33 33 00 00 00 05 | 80 a2 35 81 ca b4 | 07 01  03 ef  00 01  ff ff | 86 dd ...
DMAC (0..5)         SMAC (6..11)        F64 tag: 4 x 16-bit at 12    ethertype (20)
```

- **There is no receive prefix.** The frame starts at offset 0 every time. portd scans offsets
  0..39 for a plausible ethertype because the framing was never characterised; it always finds 0.
- The **F64 tag is inline at offset 12**, four 16-bit words, exactly where portd puts it on inject.
- **Tag word 1 is the GLORT: source on RX, destination on TX.** Every punted frame from Et1 carries
  `0x03ef`, which is precisely `PARSER_INIT_FIELDS[40] >> 16`. portd's TX tag is
  `{0x0100, 0x03ef, 0xff00, 0x0000}` — same slot, opposite direction.
- Word 0 varies with frame type (`0x0701` on IPv6/OSPF multicast, `0x0301` on MLD) — a flags or
  priority field, not yet decoded.
- Word 3 is `0xffff` on RX, `0x0000` on TX.

**This is the demux key multi-port portd needs**: tag word 1 identifies the ingress port, provided
each port has a distinct SGLORT.

## ⚠ PARSER_INIT_FIELDS cannot be written after boot

Giving port 41 a unique SGLORT is a single register write —
`PARSER_INIT_FIELDS[41] = (0x03ed << 16) | 0x103` at `0x1082a4` — and **it does not stick**. The
register still reads `0x00010103` immediately afterwards. `fm6000reg` writes work fine elsewhere
(EPL_CFG_A/CFG_B took in this same session), so this is the parser tables specifically.

That is phase 76/78's *writability is a boot state* again: the replay's `PARSER_INIT_FIELDS` writes
land during fullseq, when the memory subsystem is writable, and post-boot writes do not. So port
41's SGLORT has to be set **in the boot path** — in a generator or spliced into the replay — not
interactively. That is a small, well-defined change and it is the next concrete step.
