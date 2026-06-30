# EdgeNOS Roadmap

## Where IPv4 L3 stands today

Working end-to-end on both switches: kernel FIB (static / OSPF) → chip L3, with
ECMP hardware-forwarded. The chip is **already configured for IPv6 L3 too** —
`MY_STATION_TCAM`, `PORT_TAB`, and `CPU_CONTROL_1` all enable v6 termination/traps
(9 v6 bits set in `core/datapath/l3.c`). What's missing for v6 is only the route/
host *programming* (v6 lookup tables), not the chip bring-up.

Two L3 implementations exist, and IPv6 effort is asymmetric:
- **AS4610 `bcmd`** — OpenBCM SDK (`bcm_l3_route_add` / `bcm_l3_host_add`). IPv6 is
  nearly free: pass v6 addresses + handle v6 in the netlink path.
- **AS5610 `edged`** — hand-rolled `L3_DEFIP` writes. IPv6 needs `L3_DEFIP_128` /
  `L3_ENTRY_IPV6` programming written from scratch — the real work.

---

## Do BEFORE IPv6 (IPv4 foundation — v6 will duplicate this code)

1. **L3 table lifecycle / reclamation** — ✅ **DONE** (edged 72c0d4e).
   Added a route bookkeeping layer (`route_tab`: prefix → {slot, ecmp_group,
   path-signature}) + DEFIP slot free-list + ECMP group dedup/refcount with member-
   slot and group-id free-lists. Slots/groups are now freed on delete and reused, so
   the tables stay flat under churn. Verified on the AS5610 (slot reuse, ECMP group
   freed at refcount 0). Same layer to be reused by IPv6.

2. **Host-route / neighbor deletion** — ✅ **DONE** (edged 8a7f617).
   `next_hop_alloc/release` free-list; `l3_host_add` reuses a gateway's existing
   next-hop on ARP refresh (no re-resolution leak); `l3_host_del` implemented
   (deletes the L3_ENTRY via a new SCHAN delete, clears ING/EGR next-hop, frees the
   index). Verified on the AS5610 (re-resolve keeps nh 9, delete frees it, new
   gateway reuses 9). All four L3 allocators now alloc/free/reuse cleanly.

3. **ECMP member-change detection** — ✅ **DONE** with #1. `route_add_paths` now sorts
   the next-hop set and signatures it, so a re-dump of the same paths is a no-op and a
   change of ECMP *members* at the same path count is detected and reprogrammed.

4. **Functional forwarding validation** — we've verified routes are *programmed*
   into `L3_DEFIP`; confirm transit traffic actually **hardware-forwards and
   load-balances** across the ECMP members with real traffic (not just table reads),
   so v6 is built on a proven v4 datapath.

## Control-plane gaps (IPv4 feature parity, can run in parallel)

- **BGP** is disabled in the Quagga build (`--disable-bgpd` in
  `core/control-plane/build-quagga.sh`). The FIB→chip sync is protocol-agnostic, so
  enabling `bgpd` would give BGP (v4 now, v6 later) "for free" on the datapath side
  — plus a webui BGP module (the UI is already capability-driven).
- **OSPFv3** (`ospf6d`, also `--disable-ospf6d`) is the v6 control-plane counterpart
  to enable alongside the IPv6 datapath work.

---

## L2 / VLAN (parallel to the L3 work)

### Dumb L2-switch mode for selected ports
Bridge a chosen set of ports into one L2 broadcast domain (MAC-learning + flooding
among them, like an unmanaged switch), instead of the default model where every port
is its own L3-routed interface.

- **AS5610 `edged` — already works today.** Write `/etc/edged/l2-groups.conf`:
  ```
  # <vid> <swpA> <swpB> [swpC ...]      (ports as "swp10" or "10")
  100 swp1 swp2 swp3
  ```
  At startup `vlan_load_l2_groups()` → `vlan_l2_group_apply()` pulls those ports off
  their per-port service VLANs onto the shared VID (untagged, PVID set, STG-1), so
  the chip L2-bridges them directly and isolates them from the L3-routed ports.
  Persists via the config overlay. **To do:** document it (currently undiscoverable),
  and add a remove/toggle back to L3.
- **AS4610 `bcmd` — parity to add.** Ports default to VLAN 1 (chip already L2-bridges
  same-VLAN ports); `bcmd_port_own_vlan` does the inverse (isolate). Add the same
  config-driven `l2-groups.conf` + apply using the SDK `bcm_vlan_port_add` /
  `bcm_port_untagged_vlan_set` primitives (already used in bcmd).
- **Web UI** — a VLAN/L2 view to pick ports into a group (writes `l2-groups.conf` +
  applies live), since the UI is already capability-driven.

### VLAN testing on ports
No validation harness exists yet. Build one covering:
- Access (untagged / PVID classification) vs trunk (tagged) ingress/egress.
- VLAN isolation (no leak between groups / L3 ports) and L2-group bridging.
- Per-VLAN MAC learning + flooding (DLF) behavior.
- The CPU service-VLAN scheme (tagged CPU member, untagged egress strip).
- Real-traffic tests: inject tagged/untagged frames, capture on member/non-member
  ports, assert tag in/out and isolation — on both `edged` (5610) and `bcmd` (4610).

## IPv6 (the todo)

Depends on the IPv4 lifecycle layer above. Then:

- **Datapath — AS4610 `bcmd`**: ✅ **IMPLEMENTED** (514f77d) — `AF_INET6` in the
  netlink addr/neigh/route path + SDK `bcm_l3_*` with `BCM_L3_IP6` and 16-byte
  addresses (host/neighbor/route/ECMP, with a v6 next-hop cache); dumps both families.
  Builds clean against the full OpenBCM SDK. **Pending HW validation** — the AS4610 is
  offline. OSPFv3 (`ospf6d-arm`) shipped too (261a57d). bcmd was already EBUSY-safe and
  accepts `rtm_table==0`, so it needed no netlink fixes (unlike edged).
- **Datapath — AS5610 `edged`**: ✅ **DONE** — v6 host/neighbor `L3_ENTRY_IPV6`
  (5054f0f, encoding validated by readback) + v6 transit `L3_DEFIP_128` (3356991,
  single + ECMP, own lifecycle). `L3_DEFIP_128` is a dedicated 256-entry table (no v4
  TCAM overlap). netlink parses v6 dst/gateways. Neighbor + transit + termination all
  program the chip; verified on hardware (writes wr=0, v4 unaffected). Actual v6
  *forwarding* awaits v6 on the peer (Nexus) — chip programming is verified.
- **Control plane**: ✅ **OSPFv3 (`ospf6d`) DONE + VERIFIED AGAINST A LIVE PEER**
  (8676612, 50117f3). With the Nexus running OSPFv3, the AS5610 forms Full adjacencies
  on swp1/swp2/swp49, learns 43 v6 routes, and edged programs them into `L3_DEFIP_128`
  incl 3-way ECMP across the uplinks. Key fix: zebra installs v6 routes with
  `rtm_table=UNSPEC` + `RTA_TABLE`, so handle_route now honors `RTA_TABLE` (was
  dropping every OSPFv3 route). v6 loopback `2001:470:882d:1111::241/128` (in the
  Nexus's OSPF loopback /64) + explicit interface `::2` addresses persist via
  `swp-addrs.conf`. v6 ping to the Nexus link addresses + loopback all reply.
  Note: the box has no `ping6`/`ping -6` (BusyBox) — use a v6-capable tool to test.
- **Web UI**: ✅ **OSPFv3 module DONE** (d6065f2) — `core/webui/modules/ospf6.py`,
  neighbors/learned-routes/add-interface-to-area via the ospf6d vty (2606), verified
  on the AS5610. Still todo: v6 addresses/routes on the Interfaces/ECMP pages
  (currently `ip -4` only).
- **Validation**: v6 transit + ECMP forwarding test, mirroring the v4 validation.

## Suggested sequencing

1. IPv4 lifecycle/bookkeeping layer (#1–#3) — one focused change in `edged`.
2. IPv4 functional forwarding/ECMP validation (#4).
3. IPv6 on `bcmd` (4610) — quick win, validates the v6 netlink path.
4. IPv6 on `edged` (5610) — the `L3_DEFIP_128` work, on top of the lifecycle layer.
5. Control-plane parity (bgpd / ospf6d) + webui v6 — parallelizable.

L2/VLAN work is independent of the L3/IPv6 track and can run in parallel:
the 5610 L2-switch mode already works (document it + add toggle-off), then 4610
parity, the VLAN test harness, and a webui VLAN view.
