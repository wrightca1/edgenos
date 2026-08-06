# DCS-7150S-52 (FM6000 "Alta") — Feature Status

**Updated 2026-08-06.** What works, what doesn't, and what has never been tested — for EdgeNOS
running cold on the Arista 7150 with **no EOS at runtime**.

Every ✅ below is backed by a specific observation on hardware, quoted in the Evidence column.
Anything not directly observed is marked ❓ **untested**, not assumed working.

Legend: ✅ working · ⚠️ partial · ❌ not implemented / broken · ❓ untested

---

## Headline

The **dataplane is up**: cold boot → 10G link → packets both ways → hardware IP routing with TTL
decrement and MAC rewrite. The **control plane is up**: OSPF peers with a neighbouring switch and
learns a full routing table.

Put plainly: **this is a working router.** It boots itself from cold, brings up its own ASIC,
peers with a neighbour over OSPF, learns a routing table, programs that table into silicon, and
forwards on it — with no EOS involved anywhere.

---

## 1. Boot and platform

| Feature | Status | Evidence |
|---|:--:|---|
| Aboot loads unsigned SWI | ✅ | boots `edgenos-m1-clean2.swi`; no signature/TPM enforcement |
| M1 kernel + initramfs | ✅ | busybox rootfs, 6.12.0, boots to shell in ~40 s |
| Self-contained tool image | ✅ | 24 tools staged in `/usr/bin`; no runtime `wget` needed |
| Mgmt Ethernet (tg3) | ✅ | 10.1.1.77 reachable, SSH via dropbear |
| Serial console | ✅ | ttyS0 @9600 via 10.22.1.56 |
| SCD (FPGA) reg access | ✅ | `scdreg`, watchdog `0x0120`, power-cycle `0x7000<-0xdead` |
| Boot-config self-revert | ✅ | `init-m1` rewrites boot-config → EOS each boot, so any unattended reboot lands on EOS |
| Recovery when both lifelines die | ✅ | serial → Ctrl-C → `Aboot#` → rewrite boot-config. **Aboot has `wget`** |
| Kernel build reproducibility | ❌ | the Aug-1 `KDIR` rebuild is **broken** (no NIC IRQ, no block devices). Must use the Jul-30 kernel from `bist17.swi` |

## 2. ASIC bring-up (cold, no EOS)

| Feature | Status | Evidence |
|---|:--:|---|
| FM6000 PCIe enumeration | ✅ | `8086:155b` at `02:00.0` after SCD reset release |
| Clocks / BOOT_CTRL / BIST | ✅ | `fm6000_coldreplay`; `PIN_STRAP=0x208` |
| Scheduler circulation | ✅ | `0x8062=0x00200200` cold |
| Memory init (129 fills) | ✅ | `fm6000_memfill` by direct MMIO; broke the MCAST bank wall |
| JSS SBus master | ✅ | `fm6000_initsbus`; transactions complete, Busy clears |
| Microcode (parser/L2AR/MOD) | ✅ | loads; **required** — without it the chip forwards nothing |
| SerDes SPICO firmware | ⚠️ | **not loaded, and not needed for SR fibre** (proven by bisect). May be needed for copper — see §3 |
| Full config replay | ⚠️ | works, but it is a **299,803-write transcription of EOS**, not our own configuration. This is the core technical debt |

## 3. Ports, SerDes and link

| Feature | Status | Evidence |
|---|:--:|---|
| Et1 — 10GBASE-**SR** (fibre) | ✅ | `PORT_STATUS=0x8c0`, `pcsRx=1`, far-end AS5610 swp6 carrier up. Reproduced across many cold boots |
| Et2 — 10GBASE-**CR** (DAC copper) | ❌ | `PORT_STATUS=0x815`, **`pcsRx=0`** cold. Far end reports "Link detected: yes" — so *our TX is fine, our RX never locks* |
| Same port under EOS | ✅ | EOS links Et2 at 10G, `0` input errors ⇒ **the DAC and the port are healthy; the gap is our bring-up** |
| SFP laser / port enable | ✅ | SCD `0x5010` (Et1), `0x5020` (Et2), clear bit 6 |
| Remaining 50 ports | ❓ | never attempted |
| Link up/down events, autoneg | ❓ | no link-state monitoring exists |

**Why Et2 fails — two theories tested and killed.**

1. ~~"The EPL `+5/+6/+7/+b/+c/+d` deltas vs EOS are the CR tuning we're missing."~~ **No.** The
   replay never writes those words for *either* port — only `+1`, `+2`, `+4`, identically. They are
   read-only status: zero on cold Et2 because the link is down, non-zero on Et1 because it is up.
   Symptom, not cause.
2. ~~"Et2 needs the SPICO firmware for RX equaliser adaptation."~~ **No.** Loading SPICO cold — alive
   check passes, CRC self-check OK, SPICO running — leaves Et2 at `0x815`/`pcsRx=0`, unchanged.

**What is established.** Both SerDes lanes (SBus receivers `0x45` and `0x49`) get *identical*
programming — 15 real config transactions each, then a `2a` toggle loop — and the replay contains
**no CR/copper-specific setup at all**, no per-lane polarity/drive/pre/post anywhere. `EPL_CFG_B`
(`PcsSel=3`) is also identical on both.

**Leading hypothesis:** 10GBASE-CR requires **link training**, which 10GBASE-SR does not. The
capture window very likely missed Et2's training sequence. **Next step:** trace EOS bringing Et2
down→up (`shutdown`/`no shutdown` — the "shut == cold" trick already proven for Et1) with
`fmPlatformTraceRegOps` armed, which yields the exact CR bring-up we're missing.

## 4. Dataplane (packet DMA)

| Feature | Status | Evidence |
|---|:--:|---|
| CPU → wire (inject) | ✅ | 30 frames queued, `DONE=30`, **+39 counted at the far-end AS5610** |
| Wire → CPU (punt) | ✅ | 28 frames received into the RX ring |
| F64 tag handling | ✅ | 8-byte tag inline at frame offset 12; stripped at egress (far end parses our frames) |
| Per-port egress steering | ⚠️ | works via the F64 tag's GLORT word. The mapping lives in `PARSER_INIT_FIELDS[port]` = `0x108200+4*port`; it is unstable only because we inherited EOS's arbitrary allocation — see `GLORT-MAPPING.md` for the fix |
| DMA kernel module | ✅ | `fm6000dma.ko`, coherent low-4 GiB pool + MSI |
| RX ring accounting | ⚠️ | `fm6000_rxcount` **double-counts** (re-arms before DMA clears DONE). Don't quote its numbers |
| Multi-queue / QoS / rate limit | ❌ | not implemented |

## 5. Switching (L2)

| Feature | Status | Evidence |
|---|:--:|---|
| GLORT / DMask / L2F steering | ⚠️ | programmed by the replay; works for CPU-injected traffic |
| **Port-to-port hardware switching** | ❓ | **never tested** — every frame so far has had the CPU as source or destination. Needs a second working port |
| MAC learning | ❌ | no learning, no aging, no `L2L_SWEEPER` use |
| VLANs (tagging, PVID, filtering) | ❓ | untested; EOS runs both ports as `routed`, not switched |
| STP / LACP / LAG | ❌ | not implemented |
| Broadcast/multicast replication | ❌ | MCAST bank initialises, but replication is untested |

## 6. Routing (L3) — **the direct answer**

| Feature | Status | Evidence |
|---|:--:|---|
| L3 **endpoint** (host) | ✅ | our own userspace stack answers ARP who-has and ICMP echo for `10.101.101.26`. **ping 8/8, 0% loss** |
| **Hardware routing / forwarding** | ✅ | **WORKS COLD, no EOS.** Frame in with `DMAC=44:4c:a8:31:5d:ab` (our router) → out with `SMAC=44:4c:a8:31:5d:ab`, `DMAC=80:a2:35:81:ca:b4` (nexthop), and **ttl 20 → 19**. FIB lookup + adjacency + MAC rewrite + TTL decrement, all in silicon |
| Route table / FIB programming | ✅ | the replay programs a **44-prefix FIB**, verified by reading it back off the live cold chip (`0x33bfd2`–`0x33bffd`) |
| ARP/neighbour table in hardware | ✅ | `NEXTHOP 0x160000` holds real adjacencies; decoded entries match EOS's ARP table exactly (`80a2.3581.cab4` Et1 / `cab5` Et2) |
| ECMP | ⚠️ | group present and both NEXTHOP entries exist; **not yet exercised** (needs Et2 up) |
| **OSPF** | ✅ | **Full adjacency with the AS5610, both sides confirming.** Complete routing table learned incl. a default route. Chain: ASIC → punt → TAP → kernel → ospfd → zebra → kernel FIB |
| Port as a Linux netdev | ✅ | `fm6000_portd` — `et1` is a real interface; ping answered by the kernel stack |
| FIB sync (kernel → ASIC) | ✅ | `fm6000_fibd` mirrors the kernel FIB into hardware. OSPF-learned prefixes verified forwarded in silicon (`ttl 40 → 39` on `10.22.1.0/24`) |
| BGP / other protocols | ❌ | not built (the `quagga` component supports it) |
| IPv6 | ❌ | parser recognises `0x86dd`, nothing above it |

> **Is routing working? YES — verified 2026-08-06.** A packet was forwarded *through* the switch by
> the ASIC, cold, with no EOS running:
> ```
> in : 80:a2:35:81:ca:b4 > 44:4c:a8:31:5d:ab   10.101.101.25 > 10.22.1.99   ttl 20
> out: 44:4c:a8:31:5d:ab > 80:a2:35:81:ca:b4   10.101.101.25 > 10.22.1.99   ttl 19
> ```
> The MACs were rewritten to the nexthop adjacency and **the TTL was decremented** — that is
> hardware IP routing, not a software reply.
>
> **This corrects the earlier "not implemented" verdict.** That was based on `NEXTHOP` having only
> 34 writes and `fm6000_l3.c` containing no forwarding path. Both observations were true but the
> conclusion was wrong: the **replay itself programs the FIB** (44 prefixes, read back live off the
> cold chip) and 34 NEXTHOP writes is simply all a two-port adjacency table needs.

**Still to do:** ECMP and true two-port routing need Et2, which is intermittent (§3). The test: (config already exists on the EOS side, so the topology is
proven): our switch holds `10.101.101.26/29` on Et1 and `10.101.101.34/29` on Et2, peering with the
AS5610 at `.25` and `.33`. Put swp7 in a network namespace on the AS5610 so its kernel cannot
short-cut between the two directly-connected subnets, then ping from the `.24/29` side to a host on
the `.32/29` side. If the frame arrives with **TTL decremented and MACs rewritten**, hardware
routing works.

## 7. Platform hardware

| Feature | Status | Evidence |
|---|:--:|---|
| SFP presence / EEPROM / DOM | ⚠️ | SCD SMBus reachable; topology extracted; no monitoring loop |
| 3rd-party transceiver unlock | ✅ | key = `MD5(licensee + Arista copyright)[0:4]` BE, must be present at boot |
| **Thermal loop** | ✅ | `thermal-control.sh`, automatic at boot. MAX6658 (board + FM6000 die) → 4× `raven-fan-driver` PWM. Fails safe to 255 on sensor loss or a stopped fan — both paths tested on hardware. PWM 255→102 with the die steady at 37–38 °C |
| Front-panel LEDs | ⚠️ | 57 LED-class devices exist and sysfs writes are accepted, but **no policy drives them** and it is unverified that a write reaches the hardware (the SCD register reads 0). See `docs/LEDS.md` |
| PSU / CPLD status | ❌ | not implemented |
| Watchdog | ⚠️ | works, but **must be petted every ~10 s or it resets the board**; arming it without a petting loop is a self-inflicted reboot |

## 8. Distribution / licensing

| Item | Status |
|---|:--:|
| Repo free of third-party blobs (FM6000 side) | ✅ |
| SerDes SPICO firmware dependency | ✅ removed |
| FM6000 microcode | ❌ still third-party; replaceable — it is a documented TCAM (see `PROVENANCE.md §4`) |
| Config replay `fwd5.txt` | ❌ EOS transcription; must be generated |
| Cumulus-derived tables (AS5610 side) | ⚠️ still in-tree, undecided — `PROVENANCE.md` |

---

## What to fix next, in order

1. ~~Thermal loop~~ — **done** (`thermal-control.sh`). The only safety item on the list is closed.
2. **Et2 / copper link.** Unblocks every multi-port capability: switching, routing, ECMP, LAG.
   SPICO and the EPL-register theories are both dead (above); the live lead is **CR link training**.
   Trace EOS doing `shutdown`/`no shutdown` on Et2 to capture it.
3. **A real FIB** — program `NEXTHOP`/L3AR from a route table so the ASIC can forward. This is what
   makes it a router rather than a host.
4. **Port-to-port switching test** — the cheapest big win once Et2 is up; likely already works,
   since the replay programs GLORT/DMask/L2F.
5. **Replace the replay** with generated configuration (`PROVENANCE.md §4`), which also retires the
   last distribution blocker.
