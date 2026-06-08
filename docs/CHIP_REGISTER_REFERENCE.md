# EdgeNOS — Chip Register / Memory Reference (BCM56846 Trident+)

Every ASIC register (`*r`) and table/memory (`*m`) that `edged` reads or writes,
grouped by function, with what it does and how EdgeNOS uses it. Compiled from the
`asic/edged/` source (cumulus_replicate.c, datapath.c, l3.c, packet_io.c, etc.).

Conventions:
- **`*m`** = a *memory* (indexed table in the chip — e.g. per-port, per-VLAN, per-rule).
- **`*r`** = a *register* (single or per-port control word).
- Access is over **S-Channel** (`READ_*`/`WRITE_*` via the SDK), except the raw
  CMIC/DMA addresses noted at the end (poked directly because the CMICm register
  window doesn't accept writes on this board).

---

## 1. Port / MAC / SerDes (link bring-up)

| Reg | What it does | EdgeNOS use |
|---|---|---|
| `XMAC_CTRLr` | 10G/40G MAC master control (rx/tx enable, soft reset) | enable the unimac/xmac per port |
| `XMAC_TX_CTRLr` / `XMAC_RX_CTRLr` | MAC TX/RX datapath control (CRC mode, padding) | TX CRC-append mode; the CRC-replace gotcha that ate 4 FCS bytes was tuned here |
| `XMAC_TX_MAC_SAr` / `XMAC_RX_MAC_SAr` | MAC source-address used for pause/MAC control | per-port MAC SA |
| `XMAC_RX_MAX_SIZEr` | max RX frame size at the MAC | jumbo sizing |
| `COMMAND_CONFIGr` | unicore/CMAC command config (TX/RX enable, pause) | per-port MAC enable on some port types |
| `MAC_0r` / `MAC_1r` | low-level MAC config words | MAC init |
| `MAC_RSV_MASKr` | which MAC receive-status-vector reasons are reported | RX status filtering |
| `XLPORT_CONFIGr` | XLPORT (40G port block) config — port mode, lane count | 40G QSFP port mode |
| `XMODID_DUAL_ENr` | dual module-id enable (port→modid mapping) | port addressing |
| `RXLNSWAP1r` | per-lane RX lane swap/polarity | Warpcore lane mapping for 40G |
| `ANATXACONTROL0r` / `ANARXCONTROLPCIr` | Warpcore analog TX/RX control | SerDes analog tuning |
| `RX0..3_ANARXASTATUSr` | per-lane Warpcore analog RX status | 40G per-lane diag |
| `CL82_RX_STATUS_2/3/4r` | Clause-82 (40G) PCS lane status: alignment-marker lock, deskew | the `am_lock=0x6` 40G link-up decode |

---

## 2. L2 forwarding / VLAN / STP

| Mem/Reg | What it does | EdgeNOS use |
|---|---|---|
| `L2Xm` | the L2 forwarding (MAC) table — MAC+VLAN → port | L2 station entries |
| `L2_USER_ENTRYm` | static/protocol L2 match table (BPDU/LLDP/LACP 01:80:c2:* family) → trap to CPU | replicated 63 rows; CPU+BPDU bits for control-protocol MACs |
| `VLAN_TABm` | per-VLAN config: membership, STG, flood indices, profile pointer | service VLANs (3300+logical), profile ptr |
| `VLAN_PROFILE_TABm` | per-VLAN-profile behavior: L2_PFM, IPMCV4/6 enable, unknown-mc/uc-to-CPU | VLAN flood/forwarding policy |
| `STG_TABm` | spanning-tree group → per-port STP state (forwarding/blocking) | force ports to FORWARDING |
| `EGR_VLANm` | egress VLAN translation / membership | egress tagging per service VID |
| `EGR_VLAN_STGm` | egress STG state | egress STP = forwarding |
| `EGR_VLAN_CONTROL_1r` | global egress VLAN behavior | egress tag control |
| `LPORT_TABm` | logical-port table — per-port ingress classification (VLAN, L3 enable) | per-port ingress setup |
| `PORT_TABm` | per-port ingress config: PVID, FILTER_ENABLE, V4L3 enable, TRUST_INCOMING_VID | port VLAN + L3 enable (the V4L3-in-wrong-table bug was here) |

---

## 3. L3 routing / ECMP

| Mem/Reg | What it does | EdgeNOS use |
|---|---|---|
| `MY_STATION_TCAMm` | "is this DMAC a router MAC?" — gates L3 routing | router MAC entries so frames get L3-routed |
| `L3_IIFm` | ingress L3 interface → VRF, allow-global-route, uRPF mode | per-ingress-interface VRF=0 |
| `L3_DEFIPm` | the LPM route table (longest-prefix-match TCAM; PAIR mode = 2 routes/line) | all IPv4 routes + local-host /32→CPU |
| `L3_DEFIP_CAM_ENABLEr` / `L3_DEFIP_128_CAM_ENABLEr` | enable the DEFIP TCAM banks (v4 / v6-128) | turn on the route TCAM |
| `ING_L3_NEXT_HOPm` | ingress next-hop: destination port/trunk + properties | per-route next-hop |
| `EGR_L3_NEXT_HOPm` | egress next-hop: rewrite DMAC + egress L3 interface | the MAC rewrite for routed frames |
| `EGR_L3_INTFm` | egress L3 interface: source MAC + VLAN for routed frames | router SMAC/VLAN |
| `L3_ECMPm` | ECMP member table — the actual next-hop list | ECMP group members (swp1/swp2) |
| `L3_ECMP_COUNTm` | ECMP group descriptor: BASE_PTR_0 + COUNT_0 into `L3_ECMP` | ECMP group sizing |
| `HASH_CONTROLr` | global hash function selection for LAG/ECMP | ECMP hashing |
| `RTAG7_*` (`HASH_CONTROL_3`, `HASH_SEED_A`, `HASH_FIELD_BMAP_1/2`, `IPV4/6_TCP_UDP_HASH_FIELD_BMAP_1/2`) | RTAG7 hash: which packet fields feed the ECMP/LAG hash, seed | flow-based ECMP load-balancing (9/9 split) |

---

## 4. Field Processor (IFP) — the ACL / control-plane-trap engine

| Mem/Reg | What it does | EdgeNOS use |
|---|---|---|
| `FP_PORT_FIELD_SELm` | per-(virtual-slice) field selectors (F1/F2/F3 selcodes) — *which* packet fields each slice extracts into its key | replicated slice config (FPF2=IP/L4, etc.) |
| `FP_SLICE_MAPm` | virtual→physical slice mapping + group ids | route VS→physical TCAM slices |
| `FP_SLICE_KEY_CONTROLm` | per-slice key sub-selectors (TOS/TTL/TCP fn, class-id select) | DST_CLASS_ID_SEL on slices 2/9 |
| `FP_TCAMm` | per-rule match key + mask (the TCAM entry; index = phys slice × 256) | the 100 replicated control rules + the VS6 OSPF trap |
| `FP_GLOBAL_MASK_TCAMm` | per-rule global key/mask + **ingress-port-bitmap (IPBM)** | match-any-port IPBM (fixed the 65/66 exclusion) |
| `FP_POLICY_TABLEm` | per-rule action: copy-to-cpu, drop, CPU CoS, counter/meter ptrs | COPY_TO_CPU (DROP forced 0); CHANGE_CPU_COS/CPU_COS |
| `FP_COUNTER_TABLEm` | per-rule packet/byte counters | match probe (note: needs pool/base setup — read unreliable) |
| `FP_SLICE_ENABLEr` | **master per-slice lookup enable** (SLICE_ENABLE + LOOKUP_ENABLE bits) | `0x000f33ff` — the bit `bmd_init` never set; turns the IFP on |
| `FP_TCAM_BLK_SELr` / `FP_GM_TCAM_BLK_SELr` | enable the key-TCAM / global-mask-TCAM blocks for search | `0x00000fff` — also never set by `bmd_init` |
| `AUX_ARB_CONTROL_2r` | FP refresh-engine enable (`FP_REFRESH_ENABLE`) + arbiter | confirmed on (meter/counter refresh) |
| `AUX_ARB_CONTROLr` | L2-mod FIFO (learn/age) arbiter | L2 learn/age (not FP) |
| `ING_BYPASS_CTRLr` | ingress stage bypass (IFP/IVXLT/…) | confirmed IFP **not** bypassed (=0) |
| `IFP_METER_PARITY_CONTROLr` / `EFP_METER_CONTROLr` | FP meter parity / egress-FP meter refresh | parity/refresh config |

---

## 5. CPU punt / control-plane delivery

| Mem/Reg | What it does | EdgeNOS use |
|---|---|---|
| `CPU_CONTROL_1r` | global to-CPU trap enables (UMC, IPMC-miss, L3-err, etc.) | UMC_TOCPU / IPMCPORTMISS_TOCPU groundwork |
| `PROTOCOL_PKT_CONTROLr` | per-protocol copy-to-CPU traps (ARP, DHCP, IGMP, …) | the ARP-to-CPU trap that makes ARP work |
| `EPC_LINK_BMAPm` | egress-pipeline link bitmap — which ports (incl. CPU) are "up" for egress | `0x..07` = CPU+swp1+swp2; without it egress (incl. egress-to-CPU) is dropped |

---

## 6. MMU — buffers, queues, scheduling, flow control

| Reg | What it does | EdgeNOS use |
|---|---|---|
| `BUFFER_CELL_LIMIT_SPr` / `_SP_SHAREDr` | service-pool total + shared cell limits | MMU buffer pool sizing |
| `CELL_RESET_LIMIT_OFFSET_SPr` | service-pool reset (resume) offset | flow-control hysteresis |
| `USE_SP_SHAREDr` | use shared service pool | pool mode |
| `GLOBAL_HDRM_LIMITr` | global headroom cell limit | lossless headroom |
| `PG_MIN_CELLr` / `PG_SHARED_LIMIT_CELLr` | per-priority-group min + shared limits | ingress PG buffers |
| `PORT_PG_SPIDr` | per-port-PG → service-pool id | PG→pool map |
| `OP_BUFFER_*_CELLr` (shared/RED/YELLOW limits + resume) | egress output-buffer color limits | egress admission per color |
| `OP_PORT_LIMIT_COLOR_CELLr` / `_RESUME_` | egress per-port color limits | per-port egress buffers |
| `OP_UC_PORT_*_CELLr` | unicast per-port egress limits/config | UC egress buffers |
| `OP_QUEUE_CONFIG[1]_CELLr` / `OP_QUEUE_LIMIT_COLOR_CELLr` / `OP_QUEUE_RESET_OFFSET_CELLr` | per-egress-queue config + color limits | egress queue setup |
| `OP_VOQ_PORT_CONFIGr` | virtual-output-queue port config | VOQ setup |
| `OVQ_FLOWCONTROL_THRESHOLDr` | output-VOQ flow-control thresholds | backpressure |
| `COSWEIGHTSr` | per-CoS scheduler weights (WRR) | egress scheduler (incl. CPU port) |
| `COSMASKr` | per-port CoS-mask + `COSMASKRXEN` (RX class gate) | enable RX class processing |
| `ING_COS_MODEr` | per-port ingress CoS / queue mode | cos_mode=0/queue_mode=0 |
| `ES_QUEUE_TO_PRIOr` / `ES_TDM_CONFIGr` / `ESCONFIGr` | egress scheduler queue→prio, TDM, config | egress scheduling |
| `EGR_MTUr` / `PORT_MAX_PKT_SIZEr` | egress MTU / max packet size | frame sizing |

---

## 7. Egress masks / flood control

| Mem | What it does | EdgeNOS use |
|---|---|---|
| `EGR_MASKm` | per-port "ports to BLOCK on egress" mask (positive block mask) | zeroed (block nothing) — the EGR_MASK polarity that once broke the datapath |
| `UNKNOWN_MCAST_BLOCK_MASKm` | ports to block for unknown multicast flood | zeroed |
| `UNKNOWN_UCAST_BLOCK_MASKm` | ports to block for unknown unicast flood | zeroed |
| `NONUCAST_TRUNK_BLOCK_MASKm` | non-unicast trunk block mask | zeroed |
| `ING_CONFIG_64r` | global ingress config incl. `APPLY_EGR_MASK_ON_L2/L3` | APPLY bits (persist across restart — recovery gotcha) |

---

## 8. Storm control / metering / color

| Reg | What it does | EdgeNOS use |
|---|---|---|
| `STORM_CONTROL_METER_CONFIGr` | per-port storm-control meters | storm control config |
| `COLOR_AWAREr` | meter color-awareness mode | metering |
| `S2_CONFIGr` / `S3_CONFIGr` / `S3_CONFIG_MCr` | ingress meter/shaper stage config | scheduler/meter stages |

---

## 9. Diagnostics / counters (read-mostly)

| Reg | What it does | EdgeNOS use |
|---|---|---|
| `RUCr` | per-port RX unicast packet counter | RX-DIAG (SIGUSR1) |
| `RMCAr` | per-port RX multicast packet counter | proved the chip receives OSPF mcast |
| `RDBGC0/3/4/5/6r` | RX debug drop counters (selectable) | localize where RX frames drop |
| `RDBGC0/3/4/5/6_SELECTr` | choose which drop reasons each RDBGCn counts | aggregate / L3-hdr / disc / filter / drop |
| `TDBGC6_SELECTr` | TX debug counter select | TX-side diag |
| `MISCCONFIGr` | misc global config incl. refresh-enable | read for FP refresh check |
| `ING_MISC_CONFIG2r` | misc ingress config (IPMC-miss-as-L2MC, etc.) | multicast-to-CPU experiments |

---

## 10. DMA / CMIC (packet I/O between chip and CPU)

The proper CMICm register window (`0x31xxx`) does **not** accept writes on this
board (every read returns 0 even though DMA works), so RX/TX run through the older
**packed CMIC_DMA** registers at `0x100`, poked directly.

| Addr / Reg | What it does | EdgeNOS use |
|---|---|---|
| `CMIC_CONFIGr` (0x10c) | CMIC packet-DMA config: SG enable, reload, **`COS_RX_EN`** | `COS_RX_EN=0` so the single RX channel drains all CPU CoS |
| `CMIC_DMA_CTRLr` (0x100) | per-channel DMA control (dir, enable) | arm TX (ch0) / RX (ch1) |
| `CMIC_DMA_STATr` (0x104) | DMA status / DMA_EN | start DMA, poll DONE |
| `CMIC_DMA_DESC0r` (0x110 + 4·chan) | DCB (descriptor) base address per channel | point the engine at the ring/DCB |
| `CMIC_MISC_CONTROLr` | CMIC misc control | CMIC setup |
| `0x31140` (`CMIC_CMC_DMA_CTRL`) | CMICm per-channel CTRL (does not stick here) | diag only |
| `0x31150` (`CMIC_CMC_DMA_STAT`) | CMICm DMA state (sticky) | diag only |
| `0x31158` (`CMIC_CMC_DMA_DESC`) | CMICm current DCB ptr | diag only |
| `0x31414` (`CMIC_CMC0_PCIE_IRQ_MASK0`) | CMICm PCIe IRQ mask (CH chain/desc done) | IRQ-mask experiment (didn't stick) |
| `0x31168/0x3116c` (`FP_…`? no — `CMIC_CMC_COS_CTRL_RX_0/1`) | per-RX-channel CPU CoS-queue bitmap | dead path on this board (0x31xxx writes don't stick) |

---

## 11. Activation registers `bmd_init` (OpenMDK) never sets — and we do

These are the gaps where OpenMDK's minimal init differs from a full SDK
`soc_init`; EdgeNOS sets them explicitly:

- `FP_SLICE_ENABLEr` = `0x000f33ff` — turn the IFP slices on for lookup.
- `FP_TCAM_BLK_SELr` = `FP_GM_TCAM_BLK_SELr` = `0x00000fff` — enable the TCAM blocks.
- `CMIC_CONFIGr.COS_RX_EN` = 0 — drain all CPU CoS into the single RX channel.
- `EPC_LINK_BMAPm[0]` = CPU+swp1+swp2 — let egress (incl. egress-to-CPU) happen.
- `COSMASKr.COSMASKRXEN` = 1 — enable RX class processing.
- `ING_CONFIG_64r.APPLY_EGR_MASK_ON_L2/L3` = 1 + all block masks zeroed (SDK-standard).
- per-protocol `PROTOCOL_PKT_CONTROLr` ARP trap; `CPU_CONTROL_1r` UMC/IPMC bits.

---

## Notes / gotchas

- **FP `*_DROP` and `APPLY_EGR_MASK` bits persist across an `edged` restart**
  (init is read-modify-write). Recovery from a bad value = explicit write-0 or reboot.
- **`FP_COUNTER_TABLE` is not a reliable match signal** without pool/base setup —
  it read all-zero while a rule was definitively matching (100% drop). Use a DROP
  test or tcpdump for ground truth.
- **`0x31xxx` CMICm registers don't accept writes on this board** — anything that
  needs them must go through the packed `0x100` CMIC_DMA path.
- Memory indexing: `FP_TCAM` index = physical slice × 256; `FP_PORT_FIELD_SEL` and
  `FP_SLICE_ENABLE` are **virtual-slice** indexed, routed to physical via `FP_SLICE_MAP`.
</content>
