# EdgeNOS on Arista DCS-7150S-52-CL (Intel/Fulcrum FM6000 "Alta") — Port Roadmap

Goal: an **open-source, clean-room** NOS that boots our own Linux on the 7150 and drives the FM6000
to full L2/L3 packet forwarding — no Arista/Fulcrum SDK, no proprietary blobs in the repo.

Status: ✅ done · 🔨 in progress · ⬜ todo · ⏸ deferred/optional · ❓ needs decision

This is laid out the way a switch stack actually layers (the structure anyone who's brought up an
EdgeCore/SONiC box knows): **platform HAL → ASIC access → ASIC datapath bring-up → forwarding
primitives (the SAI surface) → NOS integration**. Each layer depends on the one below.

---

## Layer 0 — Board / boot (the "platform" foundation)
| Part | Status | Notes |
|---|---|---|
| Boot chain: Aboot → M0/M1 Linux (x86_64) | ✅ | serial-catch boot (`aboot-catch3.sh`); boot-config stays EOS for recovery |
| SCD/CPLD access (BAR0, resetGpo, SMBus masters) | ✅ | `scdreg`, in-kernel scd driver `new_object` |
| System clocks — Si5338 (Cotati map) | ✅ | THE root-cause fix; `fm6000-up.sh` §3 |
| Power / VRMs (Chl822X, UCD90160) | ✅ | characterized; undervolt refuted — no margining needed |
| Remote reboot / recovery (SCD power-cycle) | ✅ | `scdreg 0x7000 0xdead`; also works in-situ Si5338-glitch reset |
| prefdl / idprom (MAC base, serial, board vars) | ⬜ | read the board EEPROM; needed for per-port MAC + SerDes tuning vars |
| SFP+ cages ×52: presence, EEPROM/DOM, TX-disable, rate-sel, LED | ⬜ | SCD GPIO + SMBus; the optics HAL |
| Fans (Raven controller), temp/voltage sensors, PSU status | 🔨 | `raven-fan-driver.ko` present; sensors/PSU TODO |
| Front-panel + system LEDs | ⬜ | SCD LED registers |

## Layer 1 — ASIC access (the SDK/SAI foundation)
| Part | Status | Notes |
|---|---|---|
| PCIe enumeration of the FM6000 (02:00.0) | ✅ | `fm6000-pcie-init.sh` — auto on boot, Gen2 x4 |
| BAR0 CSR read/write from software | ✅ | `fm6000reg` (sysfs resource0 mmap; word addr → byte off<<2); MSE/BME set on boot |
| Bulk CSR-image loader | ✅ | `fm6000load` — replays `<addr> <val>` images (microcode/tables). Open tool, BYO image |
| I²C-slave sideband (pre-enum debug r/w) | ✅ | i2c-10/0x40; invaluable for bring-up before/without BAR0 |
| CPU packet DMA interface (FIBM + rings) | 🔨 | RE in flight — the inject/receive mechanism |
| Interrupts / MSI(-X) | ⬜ | for RX/link/error events; poll-mode first is fine |
| Kernel driver `fm6000dma.ko` (ioremap BAR, DMA, MSI) | 🔨 | clean-room stub exists; grows into the real access layer |

## Layer 2 — ASIC datapath bring-up (the `fm6000BootSwitch` equivalent)
| Part | Status | Notes |
|---|---|---|
| Chip reset → normal operating mode (scan chain) | ✅ | `SCAN_CHAIN_DATA_IN=0xFFFFFFFF` |
| Core PLL/DLL lock | ✅ | `PLL_STAT=0x0F` |
| PCIe SerDes up (JSS/SBus/`PCI_SERDES_CTRL_1`) | ✅ | in `fm6000-pcie-init.sh` |
| Block reset release order (MSB/FIBM/EPL) | 🔨 | RE in flight — which `SOFT_RESET` bit when |
| **Pipeline personality / "microcode"** (parser, L2AR, L3AR, modifier) | ⬜ | *the* content question — see clean-room note below |
| FFU / TCAM init (table-driven, not microcode) | ⬜ | fully documented (datasheet §5.6–5.7) — clean-room |
| Scheduler / traffic manager | ⬜ | egress; documented |
| GloRT → DMASK / logical-port map | ⬜ | fixed-function table; central to forwarding + CPU delivery |
| Front-panel SerDes (EPL) bring-up + per-lane tune | ⬜ | same pattern as PCIe SerDes, ×N lanes |
| SerDes SPICO (adaptive DFE) | ⏸ | not needed for a first link (PCIe came up without it); defer |
| Port / MAC config (10G, autoneg, flow control) | ⬜ | per front-panel port |
| External memory init (if the board uses it) | ❓ | verify whether Alta needs external DRAM here |

## Layer 3 — Forwarding primitives (the SAI API surface)
| Part | Status | Notes |
|---|---|---|
| L2: MAC/MA table (learn/age), VLAN, FID, flood/mcast domains | ⬜ | table-driven (datasheet §5.17) — clean-room |
| Port state / STP | ⬜ | |
| L3: LPM route table, next-hop, ARP/ND, router MAC | ⬜ | L3AR |
| ACL / FFU classification rules | ⬜ | TCAM + action RAM |
| Trap / CPU-copy (LLDP, ARP, STP, DHCP punt to CPU) | ⬜ | depends on FIBM/DMA (Layer 1) |
| Counters / statistics (per-port, per-queue) | ⬜ | |
| QoS / scheduling / shaping / mirror(SPAN) | ⬜ | later |

## Layer 4 — NOS integration (EdgeNOS proper)
| Part | Status | Notes |
|---|---|---|
| ASIC abstraction (SAI-like driver / lib over Layers 1–3) | ⬜ | the clean-room "SDK" |
| Port / interface management | ⬜ | |
| L2/L3 forwarding agents (bridge, FIB sync) | ⬜ | Linux switchdev/netdev vs custom — ❓ decision |
| Control plane (BGP/OSPF/LLDP/…) | ⬜ | EdgeNOS existing stack |
| Management (CLI, config store, telemetry/SNMP) | ⬜ | EdgeNOS existing stack |

---

## Proof-of-life milestones (the order we actually chase)
1. **M-A — BAR0 register access** ✅
2. **M-B — CPU inject → pipeline → CPU receive** 🔨 *(next)* — "the datapath moves a byte"; needs FIBM/DMA + one GloRT loopback entry + microcode. Lowest content, no SerDes/ports.
3. **M-C — one front-panel port links at 10G** ⬜ — EPL + SerDes + MAC bring-up.
4. **M-D — two-port L2 forward** ⬜ — VLAN + MAC table flood/learn.
5. **M-E — an L3 route** ⬜.
6. **M-F — all 52 ports + the NOS forwarding agents** ⬜.

## The one big content decision — ❓ the pipeline "microcode"
RE-confirmed: `fm6000Microcode.raw` is **not** a secret ISA — it's 39,461 lines of documented
`<word-addr> <value>` CSR/SRAM/TCAM writes (parser 17.6k, L2AR 12.5k, L3AR 5k, modifier 3.8k; FFU=0,
configured separately). Two paths, both open-source-clean because our loader ships no firmware:
- **BYO-firmware (fast):** user replays their OWN licensed `fm6000Microcode.raw` (extracted from an EOS
  they own) via `fm6000load` → a full working pipeline immediately. Zero Arista IP in the tree.
- **Blob-free (authored, 2nd milestone):** hand-write a minimal FlexPipe personality from the documented
  encodings (small parser image + VLAN/MAC/GloRT tables + a few L2AR DMASK rules + trivial modifier).
Plan: validate Layers 1–3 infrastructure (all our own code) with BYO-firmware, then swap in our own
minimal personality.

## Clean-room / open-source discipline (applies to every layer)
- Register maps + values are **hardware facts** (datasheet) → reimplement freely as our own code.
- Never paste SDK disassembly; document behavior, then write our own.
- **No proprietary blobs in the repo** — microcode/SPICO/`.si5338`/`.srec` are `.gitignore`'d; users
  bring their own from a licensed EOS. RE notes stay on the private GitLab, code on public GitHub.

## Open questions to resolve
- ❓ Microcode: BYO vs authored-minimal for the first forwarding demo.
- ❓ NOS dataplane control model: Linux switchdev/netdev vs a custom EdgeNOS abstraction.
- ❓ Does this board wire external DRAM to the FM6000 (Layer 2 memory init)?
- ❓ Interrupt vs poll for RX/link events (poll first).
