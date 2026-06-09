# edged — Architecture, Capabilities & Operations

`edged` is the EdgeNOS switch datapath daemon for the **Edgecore AS5610-52X**
(Broadcom Trident+ / **BCM56846**, 48× 10G SFP+ + 4× 40G QSFP). It is built on
the open-source **OpenMDK** chip library (no proprietary Broadcom SDK) and turns
a stock Linux box into a hardware-forwarding L2/L3 switch.

This document covers three things:
1. **How it works** — architecture, the init sequence, and the data path.
2. **Commands / interface** — how you actually drive it (it's a daemon, not a CLI toolbox).
3. **ASIC capabilities** — what the chip can be made to do through edged.

For the register/memory-level detail behind each capability, see
[`CHIP_REGISTER_REFERENCE.md`](CHIP_REGISTER_REFERENCE.md) and
[`DATAPATH_BRINGUP.md`](DATAPATH_BRINGUP.md).

---

## 1. How it works

### 1.1 The big idea: edged mirrors the Linux control plane into the chip

edged owns no routing/forwarding policy of its own. Instead it **listens to the
Linux kernel** (via netlink) and programs whatever the kernel decides into the
ASIC's hardware tables. The kernel's FIB, ARP/ND cache, interface addresses, and
link admin state are the source of truth; edged is the agent that makes the
silicon match.

The practical consequence is large: **any standard Linux tool or routing daemon
that installs state into the kernel gets hardware acceleration for free.** You
configure the switch with `ip addr`, `ip route`, `ip neigh`, and routing daemons
like Quagga/FRR (OSPF is already in use) — edged turns each of those into the
corresponding chip table write. There is no vendor CLI to learn.

```
   ┌────────────┐   netlink    ┌────────┐   OpenMDK/S-channel/DMA   ┌─────────┐
   │ ip / quagga │ ───────────▶ │ edged  │ ────────────────────────▶ │ BCM56846 │
   │  (kernel)  │  RTM_* events │ daemon │   table & register writes │  ASIC   │
   └────────────┘              └────────┘                           └─────────┘
```

### 1.2 Per-port TAP interfaces

At startup edged creates one **TAP interface per front-panel port**, named
`swp1`…`swp52` (`packet_io.c:tun_create`, `IFF_TAP|IFF_NO_PI`). Each `swpN` is the
Linux-visible handle for a physical port:
- The kernel sends/receives the port's CPU-bound traffic through the TAP.
- edged assigns a stable MAC per port (`base_mac + port_num`, the Cumulus scheme).
- Link **carrier** and **speed** on each `swpN` are driven from real chip state
  (see §3.6), so `ip link` and `ethtool` reflect the wire.

### 1.3 Init sequence (`edged.c:main`)

1. `parse_config()` — read `/etc/edged/config.bcm` (port map, speeds, flags).
2. `asic_init()` — open the BDE, `bmd_init_all()` (chip core init), then
   `datapath_init()` and `cumulus_replicate_init()` (the post-init tuning that
   OpenMDK's `bmd_init` doesn't do — CPU punt, FP engine, copy-replication, etc.).
3. `bde_dma_pool_reset()` + `bde_set_dma_endianness()` — prep DMA for packet I/O.
4. `packet_io_init()` — create the `swpN` TAP interfaces + RX DMA ring.
5. `netlink_init()` — open the netlink socket and dump current kernel state.
6. `l2_init()`, `l3_init()` — program the static L2/L3 scaffolding.
7. Enter the main loop.

### 1.4 The main loop (single-threaded, `edged.c`)

```c
while (running) {
    packet_io_rx_poll();     // ASIC RX DMA -> swpN TAP (and CPU punts)
    netlink_poll();          // kernel RTM_* events -> chip programming
    if (rx_diag_req) datapath_rx_diag();   // SIGUSR1 diagnostic dump
    if (++poll_count >= 300) portmap_link_poll();  // PHY/PCS link scan ~30ms
    usleep(100);
}
```

(Cumulus used three threads — RX on interrupt, TX on `select()`, link poll at
30 ms. edged collapses these into one 100 µs poll loop for simplicity.)

### 1.5 Data path

- **TX (CPU → wire):** kernel routes a packet out `swpN` → edged reads it from the
  TAP fd → maps `swpN`→chip port → `bmd_tx()` → DMA → ASIC → wire.
- **RX / punt (wire → CPU):** ASIC receives a frame and (per the CPU-punt rules)
  copies/redirects it to the CPU port → DMA → `bmd_rx_poll()` → edged maps chip
  port→`swpN` → writes it to the TAP → kernel.
- **Transit (wire → wire):** programmed L2/L3/ECMP entries forward **entirely in
  hardware** — those frames never touch the CPU.

---

## 2. Commands / interface

`edged` is a long-running daemon managed by systemd. Its surface is intentionally
small; you operate the switch through Linux, not through edged subcommands.

### 2.1 Command-line flags (`edged --help`)

| Flag | Long form | Meaning |
|------|-----------|---------|
| `-c FILE` | `--config FILE` | ASIC config file (default `/etc/edged/config.bcm`) |
| `-d` | `--debug` | Enable debug logging |
| `-f` | `--foreground` | Run in foreground (don't daemonize) |
| `-h` | `--help` | Show help |

That's the entire CLI — there are **no** `--l3`, `--scan-link`, `--show`, etc.
on the 5610 build (those exist on the separate AS4610 port). Runtime behaviour is
driven by config files, signals, and netlink.

### 2.2 Signals

| Signal | Effect |
|--------|--------|
| `SIGTERM` / `SIGINT` | Clean shutdown |
| `SIGUSR1` | **RX-DIAG dump** — `datapath_rx_diag()` writes a read-only diagnostic snapshot (CPU-punt counters, ingress drops, L3_DEFIP/CPU_COS_MAP state, FP gating regs, RX totals) to the log. Safe on a live box. |
| `SIGPIPE` | Ignored |

Trigger a diagnostic dump:
```sh
systemctl kill -s USR1 edged       # or: kill -USR1 $(pidof edged)
journalctl -u edged --since "5 sec ago"
```

### 2.3 systemd services (the operational control surface)

| Service | Role |
|---------|------|
| `edged.service` | The datapath daemon |
| `swp-l3.service` | Applies `/etc/edged/swp-addrs.conf` + `swp-routes.conf` at boot — **waits for the edged readiness sentinel first** (see below) |
| `zebra.service` / `ospfd.service` | Quagga routing stack (OSPF) |
| `fan-controller.service` | Thermal/fan control (environmentals) |
| `platform-init.service` | Platform/CPLD bring-up |

**Readiness handshake (race-free boot).** edged creates the `swpN` TAPs ~25 s
into init, so a fixed-timeout config loader used to race it and drop whichever
port edged made last. Instead, edged writes **`/run/edged.ready`** only after it
is fully up (all TAPs created, netlink handler about to run); `swp-l3-config.sh`
waits on that sentinel before applying addresses/routes (best-effort after
120 s). So the config file is the source of truth and is applied
deterministically once edged is genuinely ready — no race.

### 2.4 Config files (`/etc/edged/`)

| File | Purpose |
|------|---------|
| `config.bcm` | Port map, port speeds, ASIC flags (read at startup, `-c`) |
| `swp-addrs.conf` | Persistent L3 addresses: `<iface> <addr/plen> [mtu]` (e.g. `swp1 10.101.101.1/29 1600`, `lo 10.101.101.241/32`) |
| `swp-routes.conf` | Persistent static ECMP/transit routes: `<dst/plen> <gw1>:<dev1> [<gw2>:<dev2> …]` (multiple gateways = ECMP) |
| `datapath.conf`, `rc.soc`, `rc.forwarding`, `rc.ports_0` | Chip-init recipe fragments |

### 2.5 File-triggered debug hooks (developer use)

edged checks for a few `/tmp` files to enable on-the-fly diagnostics without a
rebuild. **Read-oriented ones are safe on a live box; the QSFP ones change PHY
state — use with care.**

| File | Effect |
|------|--------|
| `/tmp/regdump.in` | At `asic_init`, reads each `0xADDR NAME = 0xVAL` line, reads our chip's value via S-channel, and writes `/tmp/regdump.out` with `cum=… ours=… [DIFF]`. This is the **register-diff harness** used to find "Cumulus sets X we don't" gaps. |
| `/tmp/cl82dump` | Dump CL82 (40G PCS) block registers for a port |
| `/tmp/qsfp_rxremap`, `/tmp/qsfp_polflip` | QSFP lane remap / polarity-flip experiments (PHY-mutating) |

### 2.6 Typical operations

```sh
# Give a front port an L3 address (edged auto-programs the CPU-punt for it)
ip addr add 10.101.101.1/29 dev swp1 ; ip link set swp1 up

# Static 2-way ECMP transit route (edged builds an L3_ECMP group in the chip)
ip route replace 203.0.113.0/24 \
    nexthop via 10.101.101.2 dev swp1 \
    nexthop via 10.101.101.9 dev swp2

# Dynamic routing: just run OSPF — edged programs every learned route into HW
systemctl status ospfd

# See real link speed / up-down (driven from chip PCS state)
ip -br link show swp49        # UP (LOWER_UP) / DOWN (NO-CARRIER)
ethtool swp49                 # Speed: 40000Mb/s

# Diagnostics
systemctl kill -s USR1 edged ; journalctl -u edged -n 50
```

---

## 3. ASIC capabilities

Each capability below is implemented as a function that programs one or more
chip tables/memories. They are invoked from netlink handlers, the init sequence,
or the link poll — not from a CLI.

### 3.1 L2 switching (`l2.c`)
- `l2_mac_add` / `l2_mac_del` — program/remove **L2 station (FDB)** entries (from `RTM_NEWNEIGH`/bridge events).
- `l2_init` — base L2 setup; MAC learning to the CPU.
- Hardware L2 forwarding within a VLAN happens entirely in the chip.

### 3.2 VLANs (`vlan.c`)
- `vlan_create` / `vlan_destroy` — VLAN_TAB entries.
- `vlan_port_add` / `vlan_port_remove` — tagged/untagged port membership + STG.
- `vlan_init_resv_per_port` / `vlan_init_default` — the per-port reserved-VID
  scheme (Cumulus "service VLAN" model) so each port can be an isolated L3 leg.

### 3.3 L3 routing & hosts (`l3.c`)
- `l3_my_station_add` — **MY_STATION_TCAM**: which {MAC,VLAN} the chip routes for.
- `l3_local_host_add` — `/32` **CPU-punt** for our own swp IPs (so pings to us arrive).
- `l3_host_add` / `l3_host_del` — **L3_ENTRY** host routes + **ING/EGR_L3_NEXT_HOP**
  + **EGR_L3_INTF** for resolved neighbors (from `RTM_NEWNEIGH`).
- `l3_route_add` / `l3_route_add_paths` — **L3_DEFIP** prefix routes; single-path
  or multi-path (builds an **L3_ECMP** group, chip hashes flows across members).
- `l3_route_del` — invalidates the matching L3_DEFIP entry on route withdrawal.
- `l3_ecmp_group_create` — ECMP group/member tables.

### 3.4 CPU punt / control-plane trapping (`datapath.c:datapath_cpu_punt_init`)
Programs **CPU_CONTROL_1** trap bits and per-port **PROTOCOL_PKT_CONTROL** so the
right control traffic reaches the CPU:
- L3 dst-miss / slowpath / MTU-fail → CPU (`V4/ V6 L3DSTMISS`, `L3_SLOWPATH`, `L3_MTU_FAIL`).
- TTL=1 traps (`L3UC_TTL1_ERR`, `IPMC_TTL1_ERR`) — needed for OSPF adjacency
  (DBD packets are TTL=1).
- Unregistered multicast (`UMC_TOCPU`, `IPMCPORTMISS`) — for OSPF hellos (224.0.0.5/6).
- Per-port ARP request/reply + DHCP punt (`PROTOCOL_PKT_CONTROL`).
- Plus the **copy-to-CPU replication** chain (MC_CONTROL_*/EGR_MC_CONTROL_*/MCQ_CONFIG/
  SW2_FP_DST_ACTION_CONTROL) that gates FP `COPY_TO_CPU` — see §3.5.

### 3.5 Field Processor + Cumulus replication (`cumulus_replicate.c`)
OpenMDK's `bmd_init` under-initializes the chip versus a full SDK `init all`, so
this module replays the missing post-init state captured from a working Cumulus box:
- **FP / IFP engine**: slice infra (FP_PORT_FIELD_SEL, FP_SLICE_MAP, FP_SLICE_ENABLE,
  FP_TCAM_BLK_SEL/GM), global-mask (IPBM), and the FP_TCAM/FP_POLICY rules — e.g.
  the 224/8 OSPF-multicast → COPY_TO_CPU trap.
- **Copy-to-CPU replication** registers (MC_CONTROL_*/MCQ_CONFIG/…), CPU_PBM,
  CPU_COS_MAP — the MMU plumbing that actually generates a CPU copy.
- **EPC_LINK_BMAP, L2_USER_ENTRY, EGR_VLAN/STG** — chip memories replicated from
  captured dumps.

### 3.6 Link state → kernel (`portmap.c` + `packet_io.c`)
- `portmap_link_poll` — every ~30 ms reads PCS state (CL82 AM-lock+deskew for 40G,
  CL49 block-lock for 10G), drives MAC RX_EN / EPC_LINK_BMAP, and auto re-inits a
  QSFP port that's up but not fully AM-locked.
- `tun_set_carrier` (`TUNSETCARRIER`) / `tun_set_speed` (ethtool) — push real link
  carrier and speed onto each `swpN` TAP so `ip link`/`ethtool`/routing see the wire.
- `portmap_*_to_*` — the swp ↔ logical ↔ physical ↔ i2c-bus mapping helpers.

### 3.7 Packet I/O & DMA (`packet_io.c`)
- 64-DCB RX DMA ring (CMICm continuous-DMA model), `bmd_rx_poll`/`bmd_tx`.
- swp↔chip port mapping for every punted/injected frame.

### 3.8 Register access primitives (for diagnostics / new capabilities)
- **S-channel** (`cdk_xgs_reg32_read/write`) — core registers & memories (works reliably).
- **Direct BAR0** (`bde_bar0_read32`) — sub-window-0 PCIe register reads.
- Table accessors — `READ_/WRITE_<TABLE>m` + `<TABLE>m_<FIELD>f_GET/SET` for every
  chip memory (L3_DEFIP, FP_TCAM, VLAN_TAB, …).

---

## 4. What you can build on this

Because the model is "kernel state → chip", extending the switch is mostly a
matter of (a) getting the state into the kernel and (b) making sure edged has a
handler that programs it. Already working: L2 switching, VLANs, IPv4 L3
host+prefix routing, ECMP, OSPF (via Quagga), CPU-punt control plane, real
link speed/state, 10G + 40G ports. Natural next steps that fit the same pattern:
IPv6 L3 hardware routing, ACLs via the FP engine, QoS/policing, and additional
routing protocols (BGP) — each is "kernel installs it → edged mirrors it".

---

*See also: `ECMP_AND_OSPF_BRINGUP.md` (how hardware ECMP + OSPF were brought up), `FLASH_MTD_AND_ONIE_RECOVERY.md` (NOR/MTD layout + reaching ONIE from the OS via `fw_setenv`), `DATAPATH_BRINGUP.md`, `CHIP_REGISTER_REFERENCE.md`,
`TECHNICAL_DEEPDIVE_BRINGUP_ORDER.md`, `JOURNEY_WRITEUP.md`.*
