# EdgeNOS Datapath — Working Bring-up (2026-06-02)

EdgeNOS now has a **working end-to-end datapath** on the Edgecore AS5610-52X
(BCM56846 / Trident+): links come up, L2 forwards, CPU-injected frames egress
the wire, received frames punt to the CPU, and the box **pings a live Cisco
Nexus neighbor** (`5/5, 0% loss, ttl=254`).

This document is the edgenos-side companion to the reverse-engineering writeup
in `edgecore-5610-reverse-engineering/`:
- `TX_DATAPATH_PORTMAP_AND_INJECTION_2026_06_02.md` — the full diagnostic chain.
- `DATAPATH_DEBUG_INSTRUMENTATION_2026_06_02.md` — the debug probes (what/why/how).

---

## 1. The datapath, end to end

```
  swpN netdev (TUN)                          chip                       wire
  ───────────────                            ────                       ────
  TX:  kernel → swpN TUN → edged handle_tun_tx
                          → bmd_tx (directed: SOBMH → physical lane)  → egress swpN → peer
  RX:  peer → ingress swpN → L2/L3 lookup → CPU punt
                          → XGS RX DMA → bmd_rx_poll → edged → write swpN TUN → kernel
```

- **TX** uses *directed injection*: for a link-up port, edged sets
  `pkt.port = physical_lane` and `bmd_tx` builds a SOBMH module header that
  sends the frame straight out that port (no L2 lookup). Down ports fall back
  to the Cumulus service-VID flood path.
- **RX→CPU** uses the **XGS DMA** engine (packed `CMIC_DMA` regs @ `0x100`),
  *not* the CMICm/xgsd per-channel regs @ `0x31xxx` (those don't accept writes
  on this chip — the multi-session RX wall). Frames punt to the CPU via:
  - per-port `{swpN MAC, service VID} → CPU` L2 entries (ARP replies, etc.), and
  - `ING_L3_NEXT_HOP COPY_TO_CPU=1` for the switch's own IP (ICMP replies).

---

## 2. The fixes that make it work (this is the build)

| # | Where | Fix |
|---|---|---|
| 1 | `patches/openmdk/bcm56840_a0_bmd_attach.c` | `p2l()` → captured **Cumulus physical→logical port map** (was a contiguous fallback: swp2→logical 58 instead of 2, corrupting every per-port table). |
| 2 | `patches/openmdk/bcm56840_a0_bmd_switching_init.c` | Skip unmapped (`0x7f`) lanes so init doesn't abort on the Cumulus map. |
| 3 | `asic/edged/datapath.c` | `ING_CONFIG_64`: keep only L2/L3 hit-enables; **drop** `ARP_RARP_TO_FP`/`APPLY_EGR_MASK` (they need an initialised FP/egress-masks we don't have — they were dropping ARP/floods). |
| 4 | `asic/edged/packet_io.c` | **Directed TX injection** for link-up ports (untagged out the physical lane). |
| 5 | `asic/edged/l2.c` | Per-port `{swpN MAC, service VID} → CPU` L2 punt entries. |
| 6 | `patches/openmdk/bcm56840_a0_bmd_rx.c` | **RX moved to the XGS DMA path** (the one working TX uses) — the CMICm/xgsd channel regs never armed. This is what made RX→CPU punt fire. |
| 7 | `asic/edged/netlink.c` | Startup `RTM_GETADDR` dump so edged programs swpN IPs already present at boot → L3 local-host CPU-punt (ICMP echo replies). |

Plus L1: swp1/swp2 link to the Nexus stably (Warpcore PCS `block_lock`,
`LSM=0xc262`). The retimer init (`retimer-init.sh`) tunes the DS100DF410s.

---

## 3. How to bring a port up and ping (manual, today)

```sh
# on the switch (edged running)
ip addr add 10.101.101.1/29 dev swp1
ip link set swp1 up
systemctl restart edged        # so the RTM_GETADDR dump programs swp1's L3 local host
ping 10.101.101.2              # the Nexus
```

> **Why the restart:** the kernel only emits `RTM_NEWADDR` on an address
> *change*. edged's startup `RTM_GETADDR` dump programs the L3 local-host
> CPU-punt for addresses already present — so after configuring the IP, a
> restart (or configuring the IP *before* edged starts) is what wires up the
> ICMP-reply punt. **TODO:** load swpN IPs from a config file at startup so
> this is automatic and reboot-persistent.

---

## 4. Verifying the datapath (no debug build needed)

The verbose chip/DMA debug dumps were removed after bring-up (see the RE
`DATAPATH_DEBUG_INSTRUMENTATION` doc for what they were). To re-check the path:

- **TX egress:** the peer's RX counters (`show interface … counters` on the
  Nexus) — `InUcastPkts` rising = our frames reach the wire.
- **RX→CPU:** `tcpdump -i swpN` shows inbound frames once they punt; `ip neigh`
  shows the peer `REACHABLE`.
- **End-to-end:** `ping`.

---

## 5. Verified after a clean reboot (2026-06-02)

A cold boot brings the whole datapath up reproducibly: `platform-init.service`
programs the retimers, then (after `systemctl start edged`) **all four links come
up** — swp1/swp2 (Nexus) and swp47/swp48 (loopback). The **swp47↔swp48 loopback
passes traffic** (frames sent out swp47 punt to the CPU on swp48), and the
**Nexus ping is 4/4, 0% loss**.

Note: swp48 had appeared "stuck down" mid-session — that was **accumulated state
from dozens of edged restarts** repeatedly re-arming the Warpcore lanes, *not* an
RX-EQ wall. A reboot clears it. Rule of thumb: if a port won't link after many
edged restarts, reboot before chasing SerDes.

## 6. Known gaps / next

- **edged does not auto-start at boot** (`edged.service` is `disabled`;
  `platform-init.sh` doesn't launch it). `systemctl enable edged` or start it
  manually. (mgmt `end0`/`10.1.1.212` is independent and always up.)
- swpN IP config is manual + needs an edged restart (item in §3) so the
  `RTM_GETADDR` dump programs the L3 local host — make it config-driven and
  reboot-persistent.
- Repeated edged restarts can degrade Warpcore lane state (see §5) — a reboot
  is the reset.
- Broadcast/multicast-to-CPU (OSPF, etc.) traps rely on the FP, which isn't
  fully initialised (`project_init_all_insight` / `soc_init` foundation gap) —
  unicast control traffic (ARP/ICMP) works via L2/L3 punt today.
- The drop-localization stat infra (`RDBGC*` readers) is kept but dormant; the
  per-poll probe that printed it was removed.
