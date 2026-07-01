# AS5610-52X — Packet Flow

**ASIC:** Broadcom BCM56846 (Trident+) · **CPU:** PowerPC · **Datapath daemon:** `edged`
(user-mode BDE — **no KNET**; it mmaps the chip's BAR0 via `/dev/mem` and polls the DMA rings).
**Front ports:** `swp1..48` = 10G SFP+ (through DS100DF410 retimers) · `swp49..52` = 40G QSFP+.

---

## 1. Big picture

```
        FRONT PANEL                    SWITCH ASIC (BCM56846)                 CPU (PowerPC)
 ┌──────────────────┐          ┌────────────────────────────┐       ┌────────────────────────┐
 │ swp1..48  SFP+10G│──retimer─▶│  SerDes → MAC/PCS          │       │  edged  (user-mode BDE)│
 │ (DS100DF410)     │          │        │                   │       │    polls DMA rings      │
 ├──────────────────┤          │        ▼                   │ CMICm │    ↕ swpN TAP devices    │
 │ swp49..52 QSFP40G│─direct──▶│  ingress pipeline          │◀─DMA─▶│                          │
 │ (no retimer)     │          │  VLAN·L2·L3·ACL            │ (BAR0 │            ↕             │
 └──────────────────┘          │        │                   │  mmap │  ┌────────────────────┐  │
                               │  forward ── or ── punt     │  via  │  │ Linux kernel / IP  │  │
                               └────────────────────────────┘ /dev/ │  │ ospfd·ospf6d·zebra │  │
                                                                mem) │  └────────────────────┘  │
                                                                     └────────────────────────┘
```

---

## 2. Ingress (RX) — a frame arriving on a front port

```
 wire
  │
  ▼
 SFP+/QSFP cage ──▶ [DS100DF410 retimer, 10G ports only] ──▶ BCM56846 SerDes → MAC
                                                                    │
                                                                    ▼
                                                        INGRESS PIPELINE
                                                   VLAN classify → L2 learn/lookup
                                                   → MY_STATION? (dest MAC = router)
                                                       → L3 lookup → ACL / field proc
                                                                    │
                        ┌───────────────────────────────────────────┴───────────────────┐
                        ▼                                                                  ▼
               HARDWARE FORWARD  (ASIC fast path, ~µs)                          PUNT TO CPU  (slow path)
        • L2: flood/forward within the VLAN                          • dest is a switch IP (loopback/iface)
        • L3: route hit → rewrite dst-MAC + decrement TTL            • OSPF/ARP/ND, or L3-miss trap
              → pick next-hop / ECMP member                          • control-plane trap rules
                        │                                                                  │
                        ▼                                                                  ▼
                 egress port ──▶ MAC ──▶ retimer ──▶ SFP+ ──▶ wire            CMICm DMA → DCB ring in host mem
                                                                                           │
                                                                          BAR0 (mmap /dev/mem, PAXB sub-window)
                                                                                           │
                                                                              edged polls the ring, reads the frame
                                                                                           │
                                                                                 writes it into the swpN TAP device
                                                                                           │
                                                                                           ▼
                                                                         Linux kernel IP stack → ospfd / ping / …
```

---

## 3. Egress (TX) — a CPU-originated frame (OSPF hello, ping reply, punted-then-routed)

```
 ospfd / ospf6d / kernel IP stack
          │  (sends out interface swpN)
          ▼
 swpN TAP device  ──▶  edged reads the TAP
                              │
                              ▼
                    builds a DCB, DMAs the frame into the chip (CMICm)
                              │
                              ▼
        BCM56846 injects it ──▶ egress port ──▶ MAC ──▶ retimer ──▶ SFP+/QSFP ──▶ wire
```

The `swpN` interfaces are **TAP devices**; `edged` sets their carrier with `TUNSETCARRIER`, so
`ip link` shows real link up/down and speed.

---

## 4. L3 forwarding tables (in the ASIC)

```
   ┌──────────────────────────────────────────────────────────────────────┐
   │  BCM56846 L3 lookup (programmed by edged via SCHAN writes)            │
   │                                                                        │
   │   MY_STATION_TCAM ....... router MAC(s) → send frame to L3, not L2     │
   │   L3_ENTRY_IPV4_UNICAST . resolved v4 hosts / ARP cache  (hash)        │
   │   L3_ENTRY_IPV6_UNICAST . resolved v6 hosts / ND cache   (hash, 2×wide)│
   │   L3_DEFIP .............. v4 longest-prefix routes (LPM TCAM)          │
   │   L3_DEFIP_128 ......... v6 longest-prefix routes (128-bit LPM)        │
   │   ING_/EGR_L3_NEXT_HOP .. per-gateway next-hop (MAC/port/VLAN)         │
   │   EGR_L3_INTF ........... egress L3 interface (source MAC/VLAN)        │
   │   L3_ECMP / L3_ECMP_COUNT  multipath groups (hash load-balance)        │
   └──────────────────────────────────────────────────────────────────────┘
```

---

## 5. Control plane → chip (route programming)

```
  static routes ─┐
  ospfd  (v4)   ─┼─▶ zebra ─▶ Linux kernel FIB  (ip route / ip -6 route)
  ospf6d (v6)   ─┘                    │
  bgpd (optional)                     │  netlink: RTM_NEWROUTE / NEWNEIGH / NEWADDR
                                      ▼
                          edged netlink listener  (core/datapath/netlink.c)
                                      │  translate → SCHAN table ops
                                      ▼
                          BCM56846 L3 tables (section 4)
```

**Key notes**
- The switch's own IPs (interfaces **and** loopback) are programmed as CPU-punt local hosts so
  the chip delivers traffic *to the switch* up to the CPU.
- IPv6 routes from zebra carry `rtm_table=UNSPEC` + `RTA_TABLE`; edged honors `RTA_TABLE`.
- To-the-switch pings take the DMA punt path (~1–2 ms); transit through the box is the ASIC fast
  path (~µs).
```
