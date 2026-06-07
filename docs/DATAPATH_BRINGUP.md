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

## 3. Bringing up swp L3 + ping

**Persistent (default, since 2026-06-03):** swp IPs are configured at boot from
`/etc/edged/swp-addrs.conf` by `swp-l3.service` (overlay file, also in
`config/rootfs/overlay/`). Edit the conf to change addresses; both `edged.service`
and `swp-l3.service` are enabled, so swp1→10.101.101.1/29 and swp2→10.101.101.10/29
come up automatically on every boot.

```sh
# change addresses persistently:
vi /etc/edged/swp-addrs.conf      # <iface> <addr/prefix> [mtu]
systemctl restart swp-l3.service  # re-applies the addresses
```

**Manual (one-off):**
```sh
ip addr add 10.101.101.1/29 dev swp1; ip link set swp1 up
ping 10.101.101.2              # the Nexus
```

> **Do NOT restart edged to "program the punt".** The swpN interfaces are TUN
> devices owned by the edged process — when edged stops they are **destroyed**,
> taking their IP addresses with them (verified 2026-06-03 across a real reboot:
> after `systemctl restart edged`, `swp1`/`swp2` cease to exist and their addrs
> are gone). The earlier `swp-l3.service` "add addrs → restart edged" design was
> self-defeating: the restart wiped the addresses it had just set.
>
> The restart was also unnecessary: edged's **live `RTM_NEWADDR`** handler DOES
> program the L3 local-host CPU-punt on each `ip addr add` — it calls the same
> `l3_local_host_add()` as the startup `RTM_GETADDR` dump, and the edged log shows
> `local-host L3_HOST lookup OK` immediately after the add. (Corrects the earlier
> belief that the live handler was "incomplete".)

---

## 3b. L3 datapath — SOLVED (2026-06-04): three bugs behind cold-boot ICMP

After bring-up, ICMP to the Nexus failed (and earlier "0% loss" results were
warm-session artifacts). A long bisection found **three independent bugs**.
With all three fixed, **swp1↔Nexus (10.101.101.1/.2) and swp2↔Nexus
(10.101.101.10/.9) ping bidirectionally at 0% loss for all normal frame sizes,
including full standard 1500-MTU (1514-byte) frames.**

### Bug 1 — L3 route lookup never ran (`PORT_TABm` vs `LPORT_TABm`)
Inbound IPv4 to the switch's own IP was MY_STATION-terminated then **RIPD4
-discarded** (`rdbgc3 +1` per packet) because the per-port `V4L3_ENABLE`/
`V6L3_ENABLE` bits were being written to **`LPORT_TABm`** — a 128-entry *profile*
table indexed by `SOURCE_TRUNK_MAP_TABLE.LPORT_PROFILE_IDX` (=0 for our ports),
not the per-port table. The chip reads the per-port L3 enable from
**`PORT_TABm[logical_port]`**. So L3 was never enabled on the real ingress port →
the `L3_ENTRY`/`L3_DEFIP` search never ran (a match-ALL DEFIP entry's HW HIT bit
stayed 0). Confirmed against the Cumulus dump (`PORT_TAB[1..52].V4L3_ENABLE=1`,
`LPORT_TAB` none) and the BCM SDK (`OpenBCM/sdk-6.5.27`).
**Fix:** `asic/edged/l3.c` — read-modify-write `PORT_TABm[logical_port]` setting
`V4L3_ENABLE/V6L3_ENABLE` (preserving the PVID already there). *Same phys-vs-logical
trap as the PVID.* The old `LPORT_TABm` writes were dead for L3.

### Bug 2 — chip ate the last 4 bytes of every TX frame (CRC-replace mode)
Even with Bug 1 fixed, ICMP worked only for tiny payloads. The egress MAC is in
**CRC-REPLACE mode**: it overwrites the **last 4 bytes** of the frame with the
FCS instead of appending. Every directed-injected frame lost its last 4 data
bytes — harmless when those were L2 pad (≤60B-data frames, which we pad to 64),
but for an exact-length frame it truncated 4 bytes of IP payload, so the peer got
a **clean-FCS frame with a 4-byte-short IP packet** and silently dropped it
(symptom: peer iface counts full bytes, 0 CRC, 0 IP-checksum-error, no reply;
pings worked only when ≥4 pad bytes were present).
**Fix:** `asic/edged/packet_io.c` — append 4 dummy FCS-slack bytes
(`memset(tx_buf+len,0,4); len+=4;`) so the MAC overwrites slack, not data. Safe
under CRC-append mode too (4 harmless trailing L2 pad bytes).

### Bug 3 — chip MTU smaller than the configured interface MTU
Full-size frames (1472 payload / 1514B frame) failed because `EGR_MTU` and
`XMAC_RX_MAX_SIZE` (`asic/edged/datapath.c`) were **1522** while the interfaces
are configured **MTU 1600**. **Fix:** bumped both to **1622** (1600 IP + L2 +
FCS slack). Standard 1500-MTU frames now pass 0% loss. (Only a full *1600*-byte
IP packet — the top of the 1600 MTU — still fails; an extreme edge.)

### Diagnostic kept in source
`datapath_rx_diag()` (`asic/edged/datapath.c`, triggered by
`kill -USR1 $(pidof edged)`): per-stage RDBGC drop counters (**logical-port
indexed**; RIPD4 = `rdbgc3`), `L3_IIF`/VRF, `L3_DEFIP` readback + HW HIT bit, TX
counters, `PORT_TAB`/`VLAN_TAB`. This was the key instrument — it proved the
lookup wasn't running (match-all DEFIP HIT=0) and localised each stage.
Leftover diagnostics to clean up: the catch-all `L3_DEFIP[8000]` probe in
`l3.c` and the `/tmp/no_mystation` toggle.

### Nexus self-test (read-only `ai` account)
```sh
sshpass -p 'Chris123!' ssh -tt -o PreferredAuthentications=keyboard-interactive,password \
  -o PubkeyAuthentication=no -o HostKeyAlgorithms=+ssh-rsa -o UserKnownHostsFile=/dev/null \
  -o StrictHostKeyChecking=no ai@10.1.1.106 "ping 10.101.101.1 vrf routed1 count N"
```
`Ethernet1/33` = swp1 peer (.2), `Ethernet1/34` = swp2 peer (.9); routed ports,
`vrf routed1`, OSPF. (`ethanalyzer` is denied to the read-only role.)

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
