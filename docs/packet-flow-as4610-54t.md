# AS4610-54T — Packet Flow

**ASIC:** Broadcom BCM56340 (Helix4) · **CPU:** ARM (on-die, part of the SoC) · **Datapath daemon:** `bcmd`
(OpenBCM SDK **with KNET** — the `linux-bcm-knet` kernel module moves packets between the chip and the
`geN`/`xeN` netdevs; `bcmd` sets up the filters and the L3 tables).
**Front ports:** `ge1..48` = 1G copper 10/100/1000BASE-T (BCM54282 PHYs) · `xe0..5` = 10G SFP+ (BCM84758 PHY).
No PCIe/PAXB — the **CMIC is on-die**.

---

## 1. Big picture

```
        FRONT PANEL                      SWITCH ASIC (BCM56340)                 CPU (on-die ARM)
 ┌──────────────────┐        ┌──────────────────────────────┐         ┌────────────────────────┐
 │ ge1..48  1G copper│─54282─▶│  SerDes → MAC/PCS            │         │  bcmd  (OpenBCM SDK)   │
 │ (10/100/1000-T)  │  PHY   │        │                     │         │   sets KNET filters +  │
 ├──────────────────┤        │        ▼                     │ on-die  │   L3 tables            │
 │ xe0..5  10G SFP+ │─84758─▶│  ingress pipeline            │◀─CMIC──▶│                        │
 │                  │  PHY   │  VLAN·L2·L3·ACL              │  DMA    │  linux-bcm-knet (KNET) │
 └──────────────────┘        │        │                     │         │   ↕ geN/xeN netdevs     │
                             │  forward ── or ── punt       │         │  ┌──────────────────┐   │
                             └──────────────────────────────┘         │  │ Linux kernel/IP  │   │
                                                                       │  │ ospfd·ospf6d·zebra│  │
                                                                       │  └──────────────────┘   │
                                                                       └────────────────────────┘
```

---

## 2. Ingress (RX) — a frame arriving on a front port

```
 wire
  │
  ▼
 RJ45 (ge1..48) ──▶ [BCM54282 copper PHY] ─┐
 SFP+ (xe0..5)  ──▶ [BCM84758 10G PHY]     ─┴─▶ BCM56340 SerDes → MAC
                                                       │
                                                       ▼
                                           INGRESS PIPELINE
                                      VLAN classify → L2 learn/lookup
                                      → MY_STATION? (dest MAC = router)
                                          → L3 lookup → ACL / field proc
                                                       │
                        ┌───────────────────────────────┴───────────────────┐
                        ▼                                                      ▼
               HARDWARE FORWARD  (ASIC fast path, ~µs)               PUNT TO CPU  (slow path)
        • L2: flood/forward within the VLAN                 • dest is a switch IP (loopback/iface)
        • L3: route hit → rewrite dst-MAC + dec TTL         • OSPF/ARP/ND, or L3-miss trap
              → next-hop / ECMP member                      • control-plane trap rules
                        │                                                      │
                        ▼                                                      ▼
                 egress port ──▶ PHY ──▶ wire              on-die CMIC DMA → linux-bcm-knet
                                                            (KNET filter matches → copies to netif)
                                                                               │
                                                                               ▼
                                                                    geN / xeN kernel netdev
                                                                               │
                                                                               ▼
                                                            Linux kernel IP stack → ospfd / ping / …
```

Unlike the 5610, the **kernel (KNET)** owns the RX/TX DMA here — `bcmd` doesn't poll it; it just
installs the KNET filters (ingress-port match → netif) and programs forwarding.

---

## 3. Egress (TX) — a CPU-originated frame (OSPF hello, ping reply, punted-then-routed)

```
 ospfd / ospf6d / kernel IP stack
          │  (sends out interface geN/xeN)
          ▼
 geN/xeN kernel netdev
          │
          ▼
 linux-bcm-knet  (netif is TX_LOCAL_PORT) ──▶ on-die CMIC DMA ──▶ BCM56340
          │
          ▼
        egress port ──▶ PHY (54282 / 84758) ──▶ RJ45 / SFP+ ──▶ wire
```

The `geN`/`xeN` are **KNET netdevs**. They don't report carrier on their own, so `bcmd` pushes the
chip link into each netif's operstate (`IFLA_OPERSTATE`) — that's what makes `ip link` show UP/DOWN.

---

## 4. L3 forwarding tables (in the ASIC, via the SDK)

```
   ┌──────────────────────────────────────────────────────────────────────┐
   │  BCM56340 L3 lookup (bcmd programs via the OpenBCM SDK bcm_l3_* API)  │
   │                                                                        │
   │   MY_STATION ............ router MAC(s) → send frame to L3, not L2      │
   │   bcm_l3_host  (v4/v6) .. resolved hosts / ARP-ND cache                 │
   │                           (BCM_L3_IP6 flag selects v6)                  │
   │   bcm_l3_route (v4/v6) .. longest-prefix routes (LPM)                   │
   │   bcm_l3_egress ......... next-hop objects (dst MAC / port / VLAN)      │
   │   bcm_l3_egress_ecmp .... multipath groups (hash load-balance)         │
   │   l3_intf ............... egress L3 interface (source MAC / VLAN)       │
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
                          bcmd netlink listener  (asic/bcm56340/bcmd.c)
                                      │  translate → SDK calls
                                      ▼
                bcm_l3_host_add / bcm_l3_route_add / bcm_l3_egress_create
                            (BCM_L3_IP6 for IPv6)  → SDK writes the chip
```

**Key notes**
- The switch's own IPs (interfaces **and** loopback, `ifindex 1`) are programmed as CPU-punt local
  hosts so the chip delivers traffic *to the switch* up to the CPU.
- `bcmd` was already robust for v6: it drains each netlink dump fully (no EBUSY) and accepts
  `rtm_table==0`, so no `RTA_TABLE` fix was needed (unlike edged).
- To-the-switch pings take the CMIC/KNET punt path (~1–2 ms); transit through the box is the ASIC
  fast path (~µs).
```
