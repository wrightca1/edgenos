# EdgeNOS Architecture & Sources

This document explains how EdgeNOS is put together — the runtime software stack,
how a packet and a route get from Linux into the switch chip — and, importantly,
**where the knowledge came from**. EdgeNOS is not a port of an existing NOS; it
was assembled from several open and reverse-engineered sources, and this doc
credits each one.

For the build pipeline see [`ONIE_IMAGE_BUILD.md`](ONIE_IMAGE_BUILD.md); for the
A/B boot system see [`DUAL_SLOT.md`](DUAL_SLOT.md).

---

## 1. The hardware

| Part | Detail |
|---|---|
| Platform | Edgecore AS5610-52X, 48× SFP+ 10 GbE + 4× QSFP+ 40 GbE |
| Host CPU | Freescale/NXP **P2020** — PowerPC e500v2, 32-bit, **big-endian**, no hardware FPU (SPE) |
| Switch ASIC | **Broadcom BCM56846 (Trident+)**, iProc/CMICm, 14 Warpcore SerDes, 2 LED microprocessors |
| Bootloader | U-Boot (in NOR flash) |
| L1 conditioning | 32× TI **DS100DF410** quad retimers on the SFP+ path; QSFP uses internal Warpcore |
| Mgmt/glue | Accton CPLD, 10× I²C muxes, 9× GPIO expanders, MAX6697 + NE1617A thermal |

Two hardware facts shape everything: the CPU is **32-bit big-endian PowerPC**
(so endianness and the lack of a modern, well-supported toolchain/userland are
constant concerns), and the SerDes are **internal Warpcore** (so there is *no*
external 84758 PHY firmware here — that belongs to the sibling AS4610 platform).

---

## 2. Runtime software stack

```
            Quagga (zebra + ospfd)            <- routing protocols
                     │ writes
            Linux kernel FIB / neigh / link   <- the kernel is the source of truth
                     │ netlink (RTM_*)
                  ┌──┴───────────────┐
                  │      edged       │         <- userspace datapath daemon
                  │  (asic/edged/)   │
                  └──┬───────────────┘
       /dev/linux-user-bde │ ioctls          (register + DMA relay)
                  ┌──┴───────────────┐
                  │ linux-kernel-bde │         <- PCIe probe, BAR0 mmap, DMA pool, IRQ
                  └──┬───────────────┘
                     │ MMIO / DMA
            BCM56846 (Trident+) ASIC           <- L2/L3/ACL tables, ports, DMA
```

The design choice that defines EdgeNOS: **the Linux kernel routing tables are the
source of truth, and `edged` mirrors them into the chip.** Any routing daemon
(Quagga today; FRR/BGP in principle) gets hardware acceleration "for free" because
it just writes the kernel FIB, and `edged` reflects the FIB onto the ASIC. This is
the same conceptual model as Cumulus Linux, implemented independently in userspace
because there is **no upstream switchdev/SAI driver for Trident+** and SONiC/SAI
won't run on a 32-bit big-endian PPC box.

### `edged` — the datapath daemon (`asic/edged/`)

A single-process, mostly single-threaded daemon. Front-panel ports are exposed to
Linux as **`swpN` TUN/TAP** interfaces, so standard Linux tooling (`ip`,
`ethtool`, `tcpdump`, routing daemons) "just works."

| File | Responsibility |
|---|---|
| `edged.c` | Lifecycle, ASIC init order, the ~100 µs main poll loop, the `/run/edged.ready` readiness sentinel |
| `bde_interface.c` | OpenMDK CDK/BMD bring-up + register/DMA access to the kernel BDE (incl. the PAXB sub-window-7 remap) |
| `portmap.c` | swpN ↔ chip logical port ↔ SerDes lane mapping; port speed/mode; link polling (PCS block-lock based) |
| `packet_io.c` | Creates the swpN TAPs; TX (TAP→`bmd_tx`→DMA) and RX (`bmd_rx_poll`→TAP) |
| `netlink.c` | Listens to LINK/ROUTE/NEIGH/ADDR; an initial `RTM_GETADDR` dump so pre-existing IPs get programmed |
| `l2.c` | L2 MAC table programming |
| `l3.c` | L3 FIB into chip tables — `MY_STATION`, `L3_DEFIP`, next-hop/intf, and the SCHAN `HASH_INSERT` into `L3_ENTRY` |
| `vlan.c` | Default VLAN, per-port reserved service VLANs, optional L2 forwarding groups |
| `datapath.c` | CPU-punt + datapath tuning (the EdgeNOS equivalent of Cumulus `rc.datapath_0`) |
| `cumulus_replicate.c` | Replays four chip memories bare OpenMDK omits (`EPC_LINK_BMAP`, `L2_USER_ENTRY`, `EGR_VLAN`/STG, `FP_TCAM`/`FP_POLICY`) |
| `led.c` | Front-panel link/activity LEDs (software-driven; see below) |

### Out-of-tree kernel modules

| Module | Source dir | Role |
|---|---|---|
| `linux-kernel-bde.ko` | `asic/bde/` | Broadcom Device Enumerator: PCIe-probes the BCM56846, maps BAR0, allocates the DMA pool, exposes `/dev/linux-kernel-bde` (REG/DMA/IPROC ioctls). On PPC a kernel BDE is required (not raw `/dev/mem`) for the `eieio`/`sync` MMIO barriers and DMA-coherent allocation. |
| `linux-user-bde.ko` | `asic/bde/` | Thin `/dev/linux-user-bde` proxy giving `edged` userspace register/DMA access. |
| `accton_as5610_52x_cpld.ko` | `platform/cpld/` | CPLD driver (eLBC-mapped); sysfs for PSU/fan PWM/LED/watchdog. |
| `retimer_class.ko` + `ds100df410.ko` | `platform/retimer/` | Retimer device-class + TI DS100DF410 quad-10G retimer driver (the SFP+ L1 path). |
| `linux-bde-tmon.ko` | `asic/tmon/` | hwmon driver for the BCM5684x on-die thermal sensors. |

### The SDK — OpenMDK (`asic/openmdk/`)

EdgeNOS uses **OpenMDK** (Broadcom's open "Mini Driver Kit", v2.10.9), not the
proprietary full Broadcom SDK. Three layers:

- **CDK** — Chip Development Kit: register/memory access and chip definitions
  (e.g. `bcm56840_a0_defs.h`).
- **BMD** — Mini Driver: the port/switch/L2/L3 API (`bmd_init`, `bmd_port_mode_set`, …).
- **PHY** — transceiver drivers including the internal Warpcore/TSC SerDes
  firmware-set code.

OpenMDK is a *partial* reimplementation of the full SDK — it lacks pieces like a
complete `soc_init` and per-lane SerDes RX calibration. That gap is exactly why
`cumulus_replicate.c` and the hand-ported register writes in `datapath.c` exist:
they supply the chip state OpenMDK's `bmd_init` doesn't.

### Control plane

`Quagga` (`zebra` + `ospfd`, static cross-build) runs OSPF; config under
`config/rootfs/overlay/etc/quagga/`. The flow is:

```
zebra/ospfd → kernel FIB/neigh → netlink → edged → chip tables
```

For the control plane to *see* its neighbors, `edged` programs **CPU punt** so the
chip delivers the switch's own-IP traffic, ARP/DHCP, and OSPF link-local
multicast (224.0.0.5/6) and TTL=1 control unicast to the host CPU
(`datapath.c` CPU_CONTROL_1 bits + `l3_local_host_add`).

---

## 3. Where the knowledge came from

This is the part that's hard to reconstruct later, so it's recorded explicitly.
EdgeNOS is a synthesis of several sources:

### a. Broadcom OpenMDK (open, source-available)
The CDK/BMD/PHY SDK under `asic/openmdk/`. Provides chip register definitions, the
BMD driver API, and Warpcore SerDes/firmware code. Upstream:
`github.com/Broadcom-Network-Switching-Software/OpenMDK`. EdgeNOS's local
divergence (Warpcore driver fixes + ucode, bcm56840 datapath files, MIIM
block-select, DMA) lives in `patches/openmdk/` and is reapplied by
`scripts/apply-openmdk-patches.sh` (the `asic/openmdk` tree itself is git-ignored).

### b. Broadcom full SDK / OpenBCM (read-only reference)
Used for *reading source* to understand mechanisms OpenMDK underdocuments (e.g.
the `PORT_TAB` vs `LPORT_TAB` indexing trap, the PAXB sub-window remap in
`shbde_iproc.c`). The kernel BDE here is "based on OpenMDK libbde."

### c. Cumulus Linux reverse engineering (the largest source)
By far the deepest well of knowledge. Cumulus Linux 2.5 ran on this exact
hardware, and ~600 MB of its live state was captured and mined offline:

- Trees: `edgecore-5610-reverse-engineering/` (cumulus_baseline_2013*,
  `ghidra-decomp/`, dozens of analysis notes) and `extracted/` (Cumulus 2.5.0/2.5.1
  image extractions), both in the parent `/home/smiley/edgecore/`.
- What it fed directly: `datapath.c` (the `rc.datapath_0` CPU-punt values),
  `cumulus_replicate.c` (four chip memories captured from live dumps),
  `linux-bde-tmon.c` (Ghidra decompile of Cumulus's `.ko`), the per-port
  service-VLAN scheme, the **LED chain map** (`led.c`), the DMA model, the chip
  init/bring-up ordering, and the dual-slot `cl.active` boot scheme.
- Only *parameter values and observed behavior* were used — no proprietary
  Broadcom/Cumulus source was copied into EdgeNOS.

### d. Vendor datasheets & IEEE standards
The TI DS100DF410 retimer register map (CDR-reset sequence), and IEEE 802.3
Clause 45/49/82 for the 10 G/40 G PCS/MDIO bring-up (see the SerDes/optics docs).

### e. Open-source upstreams
The **Linux kernel** (6.1.175 LTS, vanilla + DTS + defconfig), **Quagga**, and a
**Buildroot 2023.02.9** base userland (systemd, openssh, iproute2, ethtool,
i2c-tools, u-boot-tools).

A running narrative of how these came together — first 10 G light, the
three-bug L3 ping hunt, the 40 G QSFP bring-up — is in
[`JOURNEY_WRITEUP.md`](JOURNEY_WRITEUP.md), with deeper register/SerDes detail in
[`TECHNICAL_DEEPDIVE_BRINGUP_ORDER.md`](TECHNICAL_DEEPDIVE_BRINGUP_ORDER.md) and
the optics/SerDes references.

---

## 4. Licensing posture

EdgeNOS is **distributable, but source-available rather than pure-OSI**, because
of the Broadcom-licensed components. Two buckets:

**GPL / copyleft (fully open):**
- All EdgeNOS-original code — `edged` and its modules, the kernel BDE
  (`linux-kernel-bde`/`linux-user-bde`), CPLD, retimer, tmon, the installer and
  initramfs — is **SPDX: GPL-2.0-or-later**.
- The Linux kernel, Quagga, and the Buildroot base userland under their
  respective upstream licenses (mostly GPL/permissive).
- The out-of-tree `.ko`s link GPL kernel symbols, so GPL obligations apply.

**Broadcom source-available (NOT OSI, NOT GPL):**
- **OpenMDK** under `asic/openmdk/Legal/LICENSE` (Avago/Broadcom). The grant is
  broad — use, reproduce, distribute, create derivatives, and **sublicense** —
  but it carries restrictions: you must keep all proprietary notices intact,
  reproduce them in every copy, and accept the "as-is"/no-high-risk terms. This
  includes the Warpcore SerDes firmware shipped inside OpenMDK.

**Net compliance:** keep the Broadcom/OpenMDK `Legal/LICENSE` and all proprietary
notices; honor GPL for the kernel, all EdgeNOS modules, and Quagga; and label
components by their respective licenses. The result can be redistributed and
built upon, but it is not 100% OSI-licensed end to end.

---

## See also

- [`EDGED_ARCHITECTURE_AND_OPERATIONS.md`](EDGED_ARCHITECTURE_AND_OPERATIONS.md) — deeper on edged internals and operations.
- [`DATAPATH_BRINGUP.md`](DATAPATH_BRINGUP.md) — the working L2/L3 datapath and the bugs solved to get there.
- [`ECMP_AND_OSPF_BRINGUP.md`](ECMP_AND_OSPF_BRINGUP.md) — hardware ECMP + OSPF control-plane punt.
- [`CHIP_REGISTER_REFERENCE.md`](CHIP_REGISTER_REFERENCE.md) — every chip register/memory edged touches.
