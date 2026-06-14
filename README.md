# EdgeNOS - Network Operating System for Edgecore AS5610-52X

Open-source NOS for the Edgecore AS5610-52X bare metal switch.

- **CPU**: Freescale P2020 (PowerPC e500v2, dual-core 1.2 GHz)
- **ASIC**: Broadcom BCM56846 (Trident+) -- 48x SFP+ 10GbE + 4x QSFP+ 40GbE
- **SDK**: OpenMDK (open-source CDK/BMD/PHY, no proprietary Broadcom SDK)
- **Kernel**: Linux 6.1.175 LTS (upgraded from 5.10 via the 5.15 LTS step)
- **Root**: SquashFS + OverlayFS (read-only base + writable overlay), **dual-slot A/B**

> **Status (2026-06-14): L2/L3 switch with dynamic routing, on dual-slot 6.1.**
> The box does hardware **IPv4 L3 routing + ECMP**, runs **OSPF** (Quagga) with a
> live-Nexus adjacency, brings up a **40 G QSFP uplink** (preferred over the 10 G
> links), drives the **front-panel port link/activity LEDs**, reports **real link
> speed/carrier** via `ip link`/`ethtool`, runs **active fan/thermal control**,
> applies its config **race-free at boot**, supports **in-place A/B upgrades with
> automatic rollback** (`nos-upgrade`), and can drop into the **ONIE installer
> from the running OS** (`fw_setenv onie_boot_reason install`).
>
> **New here?** Start with [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) (what the
> software is and where the knowledge came from),
> [`docs/ONIE_IMAGE_BUILD.md`](docs/ONIE_IMAGE_BUILD.md) (how to build an image),
> and [`docs/DUAL_SLOT.md`](docs/DUAL_SLOT.md) (how A/B upgrades work). Deeper
> runtime detail is in
> [`docs/EDGED_ARCHITECTURE_AND_OPERATIONS.md`](docs/EDGED_ARCHITECTURE_AND_OPERATIONS.md)
> and [`docs/DATAPATH_BRINGUP.md`](docs/DATAPATH_BRINGUP.md).

## Capabilities

- **L2** — MAC learning + VLAN forwarding in hardware.
- **L3 (IPv4)** — host + prefix routes programmed to the chip (MY_STATION /
  L3_DEFIP / next-hop / intf), **ECMP** multipath, CPU punt for control traffic.
- **OSPF** — Quagga `zebra`+`ospfd`; per-interface cost (40 G primary, 10 G ECMP
  backup), loopback router-id; FIB→chip mirroring means any routing daemon gets
  hardware acceleration for free.
- **Ports** — 48× 10 G SFP+ and 4× 40 G QSFP+; real carrier + speed surfaced on
  the `swpN` interfaces; SFP/QSFP DOM (optical power) readable over i2c.
- **Port LEDs** — front-panel link/activity LEDs driven by `edged` (the two
  on-chip LED microprocessors render solid-green-on-link / blink-on-traffic).
- **Config-driven, race-free boot** — addresses/MTU/routes loaded from
  `/etc/edged/*.conf` only after edged signals readiness (`/run/edged.ready`).
- **Platform** — active fan control with thermal emergency-shutdown, PSU/sensor
  monitoring via the CPLD driver.
- **Dual-slot A/B upgrades** — in-place install to the inactive slot, byte-verify,
  activate, and **automatic rollback** if the new image's datapath doesn't come
  up healthy (`nos-upgrade`); see [`docs/DUAL_SLOT.md`](docs/DUAL_SLOT.md).
- **Recovery** — reach ONIE from the OS with `fw_setenv onie_boot_reason install`
  (bootloader + ONIE image kept read-only); see
  [`docs/FLASH_MTD_AND_ONIE_RECOVERY.md`](docs/FLASH_MTD_AND_ONIE_RECOVERY.md).

---

## Requirements

- **Docker** 20.10+ (with BuildKit support)
- **Host OS**: Linux x86_64 (tested on Fedora 36)
- **Disk space**: ~10 GB for Docker image + build output
- **RAM**: 4 GB minimum, 8 GB recommended
- **Network**: Internet access for kernel and Buildroot downloads (first build only)
- **Switch**: Edgecore AS5610-52X with ONIE bootloader
- **Serial cable**: RJ45-to-DB9 or USB console cable (115200 baud)

## Building

The build runs in Docker (no host cross-compiler needed for most steps). The
image is assembled from three independent pieces — kernel/DTB/initramfs (the FIT),
the ASIC SDK + `edged`, and the Buildroot rootfs — then stitched into a
self-extracting ONIE `.bin`.

**The full, step-by-step build guide — including the fast "rebuild just edged and
repackage" path and the build-environment gotchas — is
[`docs/ONIE_IMAGE_BUILD.md`](docs/ONIE_IMAGE_BUILD.md).** The short version:

```bash
export DOCKER_HOST=unix:///run/user/1000/docker.sock
cd edgenos

scripts/apply-openmdk-patches.sh                  # restore OpenMDK divergence (clean checkout)
docker build --network host -t edgenos-builder .

KVER=6.1.175 scripts/build-kmodules.sh            # kernel uImage + DTB + .ko modules
scripts/rebuild-edged-with-sdk.sh                 # SDK libs + edged  (-> output/edged-rebuilt)
scripts/build-quagga.sh                           # zebra + ospfd
SRC=$(pwd) bash scripts/assemble-rootfs-from-base.sh   # -> output/images/rootfs.sqsh
#   (build the FIT with package-image.sh only if the kernel/DTB/initramfs changed)
bash installer/build-image.sh --dual-slot         # -> output/images/edgenos-as5610-52x-dualslot.bin
```

Fast path for a userspace-only change (the common case): rebuild `edged`,
re-run `assemble-rootfs-from-base.sh`, and `build-image.sh --dual-slot` reusing
the existing FIT. See the build guide for details.

Build toolchain (in the `edgenos-builder` image / `debian:bullseye` for the
initramfs): `gcc-powerpc-linux-gnu`, Linux 6.1.175 source, Buildroot 2023.02.9
base, `mkimage` (FIT), `dtc`, `mksquashfs`, `qemu-user-static`.

---

## Installing on the Switch

For an already-running EdgeNOS, the normal way to update is an **in-place A/B
upgrade** (no ONIE, with automatic rollback) — see
[`docs/DUAL_SLOT.md`](docs/DUAL_SLOT.md):

```bash
scp output/images/edgenos-as5610-52x-dualslot.bin root@<box>:/tmp/edgenos.bin
ssh root@<box> 'nos-upgrade --activate /tmp/edgenos.bin'   # installs to the inactive slot
ssh root@<box> 'fw_printenv cl.active'                     # confirm before rebooting
ssh root@<box> 'reboot'
```

The first-time install (bare metal, via ONIE) follows.

### Step 1: Boot into ONIE Install Mode

Connect serial console (115200 8N1). Power on the switch and interrupt U-Boot:

```
Hit any key to stop autoboot
=> run onie_bootcmd
```

**Or, from a running EdgeNOS (no serial needed):**

```bash
fw_setenv onie_boot_reason install && reboot   # U-Boot then boots ONIE install
```

This writes only the U-Boot env partition; the bootloader and ONIE image stay
read-only. See [`docs/FLASH_MTD_AND_ONIE_RECOVERY.md`](docs/FLASH_MTD_AND_ONIE_RECOVERY.md).

### Step 2: Serve the Image

On a machine reachable from the switch management port:

```bash
cd output/images
python3 -m http.server 8080
```

### Step 3: Install from ONIE

In ONIE shell (via serial console):

```bash
onie-nos-install http://<your-ip>:8080/edgenos-as5610-52x.bin
```

### Step 4: Configure U-Boot (Required After Install)

ONIE resets `nos_bootcmd`. After install, before reboot:

```bash
# Stop ONIE auto-discovery
killall -9 discover; sleep 1

# Set boot variables (write to temp file, not stdin - ONIE fw_setenv quirk)
cat > /tmp/env.txt <<'E'
fdt_high 0xffffffff
initrd_high 0xffffffff
nos_bootcmd usb start; usbiddev; setenv bootargs console=ttyS0,115200 root=/dev/sda6 rootfstype=squashfs ro rootwait; usbboot 0x02000000 ${usbdev}:5 && bootm 0x02000000#accton_as5610_52x
boot_count 0
E
# CRITICAL: DELETE onie_boot_reason (empty value = delete)
# If set to ANY value (even "nos"), U-Boot boots ONIE instead of NOS!
echo "onie_boot_reason" >> /tmp/env.txt

fw_setenv -f -s /tmp/env.txt

# Reboot into EdgeNOS
reboot -f
```

**Important U-Boot notes for AS5610:**
- `usbboot` (not `usb read`) is the proven command for this U-Boot build
- `onie_boot_reason` must be **deleted** (empty), not set to "nos"
- `usbiddev` detects the USB device number automatically
- `bootm ... #accton_as5610_52x` selects the FIT configuration by name

### Step 5: Login

- **Serial**: ttyS0 at 115200 baud
- **SSH**: `ssh root@<dhcp-ip>`
- **Password**: `as5610`

---

## Architecture

```
U-Boot (in NOR flash)
  nos_bootcmd -> boot active slot (cl.active): FIT from sda5 (slot 1) or sda7 (slot 2)
              -> scripted auto-rollback past boot_limit (see docs/DUAL_SLOT.md)
        |
Linux 6.1.175-edgenos (PowerPC e500v2, SMP)
  gianfar (end0) | mpc-i2c (x2) | fsl-ehci (USB) | PCIe
        |
per-slot initramfs (nos-init) -> squashfs root (sda6/sda8) + per-slot overlay (sda3) + persist (sda1)
        |
systemd -> platform-init.sh -> edged -> networking
                |                   |
                |                   +-- OpenMDK CDK/BMD/PHY
                |                   +-- bmd_reset -> bmd_init (WC firmware)
                |                   +-- bmd_port_mode_set (10G/40G)
                |                   +-- bmd_tx/rx (DMA packet I/O)
                |                   +-- TUN interfaces (swp1-52)
                |                   +-- netlink mirror: kernel FIB -> chip L3/ECMP
                |                   +-- front-panel port LEDs (link/activity)
                |
                +-- Load kernel modules (bde, cpld, tmon, retimer)
                +-- GPIO init (QSFP reset, SFP TX enable)
                +-- 32x DS100DF410 retimer programming
                +-- CPLD fan PWM + thermal control
```

### ASIC Packet Path

```
TX: kernel -> TUN read -> bmd_tx() -> DMA -> BCM56846 -> wire
RX: wire -> BCM56846 -> DMA -> bmd_rx_poll() -> TUN write -> kernel
```

### Hardware Components (133 devices)

| Category | Count | Chips |
|----------|-------|-------|
| CPU | 1 | P2020 (I2C x2, UART, USB, PCIe, GbE) |
| Switch ASIC | 1 | BCM56846 (14 Warpcore SerDes, 2 LED processors) |
| CPLD | 1 | Accton custom (LEDs, PSU, fan, watchdog) |
| I2C Mux | 10 | PCA9548 x7 + PCA9546 x3 |
| GPIO Expander | 9 | PCA9506 x5 + PCA9538 x4 |
| Retimer | 32 | DS100DF410 (signal conditioning) |
| Temp Sensor | 2 | MAX6697 + MAX1617 |
| EEPROM | 59 | Board + 48 SFP + 4 QSFP |
| RTC | 1 | RTC8564 |

---

## Project Structure

```
Makefile                        Top-level build orchestration
Dockerfile                      Docker cross-compile environment
docker-compose.yml              Docker Compose config
config/
  kernel/as5610_defconfig       Kernel config (P2020/e500v2)
  bcm/                          ASIC config (config.bcm, rc.soc, LED hex)
  rootfs/
    buildroot_defconfig          Buildroot config
    overlay/                    Files merged into rootfs
      etc/systemd/system/       Service files (edged, platform-init, thermal)
      etc/modules-load.d/       Kernel module load order (12 modules)
      usr/sbin/                 Init scripts (platform-init.sh, edged-init, etc.)
kernel/
  dts/as5610-52x.dts            Device tree (full I2C mux topology)
installer/
  install.sh                    ONIE self-extracting installer (single-slot)
  install-dual-slot.sh          ONIE installer, dual-slot A/B layout
  build-image.sh                Stitch FIT + rootfs behind an installer header
asic/
  openmdk/                      OpenMDK (CDK/BMD/PHY) -- vendored tree, git-ignored
                                (restore with scripts/apply-openmdk-patches.sh)
  bde/                          BDE kernel modules (PCI, DMA, IRQ, mmap)
    linux-kernel-bde.c          Kernel BDE (ioread32/iowrite32 with PPC barriers)
    linux-user-bde.c            Userspace BDE bridge
  edged/                        Switch daemon
    edged.c                     Main daemon (init, main loop)
    bde_interface.c             BDE/CDK/BMD integration (register access, DMA)
    portmap.c                   Port config + link polling (bmd_port_mode_set)
    packet_io.c                 Packet I/O (TUN + bmd_tx/rx with DMA coherent)
    l2.c                        L2 MAC table (bmd_port_mac_addr_add/remove)
    l3.c                        L3 routing -> chip (MY_STATION/L3_DEFIP/next-hop, ECMP)
    led.c                       Front-panel port link/activity LEDs (LEDUP0/1)
    vlan.c                      VLAN management (bmd_vlan_create/port_add)
    netlink.c                   Netlink listener (route/neigh/link events)
    datapath.c                  CPU-punt + datapath tuning (rc.datapath_0 port)
    cumulus_replicate.c         Replays 4 chip memories OpenMDK omits
  mdk-init/                     OpenMDK standalone init tool
  tmon/                         BCM on-die thermal hwmon module
platform/
  cpld/                         CPLD kernel module (LEDs, PSU, fan, watchdog)
  retimer/                      DS100DF410 retimer kernel module
  onlp/                         Platform library (SFP, sensors)
```

---

## Implementation Status

### Verified Working (end-to-end ping through ASIC)

```
AS5610 swp2 (10.101.101.10) <--10GbE SFP+--> Nexus Eth1/34 (10.101.101.9)
ping: 5/5, 0% loss, RTT ~0.57ms
```

| Component | Status | Source |
|-----------|--------|--------|
| Link UP (10G SFP+) | Working | OpenMDK SerDes + retimer CDR reset |
| Link UP (40G QSFP) | Working | Warpcore CL82 (4-lane AM-lock + deskew) |
| TX/RX DMA | Working | OpenMDK xgs_dma + endianness + VLAN strip |
| ARP / ICMP | Working | protocol punt + static L2 + CML learning fix |
| **L3 IPv4 (hardware)** | Working | `l3.c`: MY_STATION / L3_DEFIP / next-hop / SCHAN HASH_INSERT |
| **ECMP (hardware)** | Working | L3 next-hop group hash; multipath FIB mirrored to chip |
| **OSPF** | Working | Quagga zebra+ospfd → kernel FIB → netlink → chip |
| **Port LEDs** | Working | `led.c`: LEDUP0/1 microcode, link/activity |
| **Dual-slot A/B + rollback** | Working | `nos-upgrade`, U-Boot `cl.active`, boot-success reset |
| TUN interfaces (swp1-52) | Working | Custom `packet_io.c` |

End-to-end verified: ping to a Cisco Nexus at **0% loss** on all three uplinks
(swp1/swp2 10G, swp49 40G), OSPF adjacencies **Full**, routes installed to the
chip. See [`docs/DATAPATH_BRINGUP.md`](docs/DATAPATH_BRINGUP.md) and
[`docs/ECMP_AND_OSPF_BRINGUP.md`](docs/ECMP_AND_OSPF_BRINGUP.md).

### Not yet / out of scope

| Component | Note |
|-----------|------|
| ACL/FP COPY_TO_CPU delivery | FP rule *matching* works; explicit FP copy-to-CPU delivery is unfinished (OSPF/control punt works via the datapath CPU-control path instead). See [`docs/FP_FIELD_PROCESSOR_PORT_SCOPE.md`](docs/FP_FIELD_PROCESSOR_PORT_SCOPE.md). |
| BGP / LLDP / STP | Not deployed; BGP is straightforward to add (FIB→chip mirroring gives it HW forwarding for free). |

---

## Key Technical Discoveries

Each fix below was required to get end-to-end packet I/O working on
BCM56846 (iProc) with a PowerPC (big-endian) host. Documented here
because none of this is in public Broadcom documentation.

### 1. Retimer CDR Reset (link UP)

**Problem**: DS100DF410 retimer passes no signal even with correct EQ settings.
**Root cause**: CDR (Clock Data Recovery) must be explicitly reset after power-on.
**Fix**: Toggle register 0x0A between 0x1C (assert reset) and 0x10 (release).
**How we found it**: DS100DF410 datasheet register map. Register 0x03 (CDR lock
status) showed 0x00 (unlocked). After CDR reset, it shows 0x04 (locked) and
the link comes up immediately.
**Where**: `platform-init.sh` init_retimer(), `portmap.c`

### 2. DMA Engine Selection (xgs not xgsd)

**Problem**: CMICm DMA registers at 0x31xxx are inaccessible through BAR0.
**Root cause**: iProc PAXB sub-window IMAP registers cannot be written via BAR0
MMIO or PCI config space (only 4KB config on P2020). Without IMAP remap,
only sub-window 0 (0x000-0xFFF → AXI 0x18000000) works.
**Fix**: Use old CMIC DMA (xgs_dma at offset 0x0100) instead of CMICm
(xgsd_dma at 0x31xxx). Old CMIC is in sub-window 0 and works directly.
**How we found it**: Added AXI remap debug to BDE kernel module. IMAP register
writes returned only the valid bit — page address was always lost. Traced
through Broadcom SDK `shbde_iproc.c` which also uses BAR0 MMIO for IMAP,
confirming the mechanism is correct but doesn't work on P2020 due to 4KB
PCI config space limitation preventing proper PAXB initialization.
**Where**: `mdk-init/Makefile` (reverted chip files to xgs_dma), `cdk_custom_config.h`

### 3. CMC Offset Fix (CDK_XGSD_CMC=0)

**Problem**: xgsd register access adds uninitialized CMC offset (0x8000) to addresses.
**Root cause**: `CDK_XGSD_CMC_OFFSET(unit)` returns `CMC * 0x1000`. Without
`CDK_XGSD_CMC` defined, uses dynamic value which is uninitialized = garbage.
**Fix**: `-DCDK_XGSD_CMC=0` in build flags. BCM56846 iProc uses CMC 0 for PCI.
**How we found it**: BDE AXI remap debug showed page=0x18039000 instead of
expected 0x18031000. The 0x8000 difference = CMC 8 * 0x1000. Traced through
CDK headers to `CDK_XGSD_CMC_OFFSET` macro.
**Where**: `mdk-init/Makefile`, `cdk_custom_config.h`

### 4. DMA Endianness (CMIC_ENDIANESS_SEL)

**Problem**: DMA descriptors unreadable by ASIC — DMA_ACTIVE forever, never completes.
**Root cause**: `CMIC_ENDIANESS_SEL` (register 0x174) is cleared by CPS reset during
`bmd_reset()`. The CDK's `cdk_xgs_cmic_init()` tries to re-set it, but
iowrite32 on PPC + CDK SYS_BE_PIO double-swap issue means the write may
not take effect reliably.
**Fix**: Set `ENDIAN_SEL = 0x04000004` (DMA_OTHER only) AFTER all bmd_init
completes, right before packet_io starts. DMA_OTHER enables descriptor
word byte-swap (BE host memory → LE CMIC internal). DMA_PACKET must NOT
be set — packet data is already in network byte order.
**How we found it**: Added debug printk to xgs_dma chan_start/poll showing
DMA_STAT register. DMA_ACTIVE was set but DESC_DONE never appeared. Tested
all ENDIAN_SEL values (0x00-0x07) via devmem. Discovered that
`iowrite32(0x06000006)` works from kernel but CDK path doesn't persist
after CPS reset. Added `bde_set_dma_endianness()` called from edged.c.
**Where**: `bde_interface.c` bde_set_dma_endianness(), `edged.c`

### 5. Single DCB TX (no scatter-gather)

**Problem**: Nexus receives frames as "UnderSize" (InOctets shows ~70 bytes for 90-byte frame).
**Root cause**: OpenMDK scatter-gather TX splits frame into 2 DCBs:
DCB[0]=16 bytes (L2 header), DCB[1]=remaining. On BCM56846 iProc, the ASIC
consumes DCB[0]'s data as metadata and only sends DCB[1] on the wire.
**Fix**: Use a single DCB for the entire frame. Removed SG/CHAIN flags.
**How we found it**: Nexus `show interface counters` showed InOctets=70 for
a 90-byte frame. 90-16=74, 74-4(FCS)=70 — exactly DCB[1]'s byte count.
The first 16 bytes (Ethernet header) were being eaten by the ASIC's
scatter-gather handling on iProc.
**Where**: `bcm56840_a0_bmd_tx.c` (pkgsrc — modified via Docker)

### 6. Minimum Frame Padding (64 bytes)

**Problem**: Nexus reports "UnderSize" for ARP frames (42 bytes from kernel).
**Root cause**: The ASIC strips 4 bytes (VLAN tag) on egress for untagged ports,
even if no VLAN tag was present. With standard 60-byte padding: 60-4=56+4FCS=60 → UnderSize.
**Fix**: Pad frames to 64 bytes minimum (not the standard 60). After ASIC
strips 4 bytes: 64-4=60+4FCS=64 → exactly at Ethernet minimum.
**How we found it**: Nexus `show interface counters errors` showed UnderSize
but 0 FCS-Err. Frames had valid CRC but were too short. Incrementally
tested padding from 60→64 bytes until Nexus accepted the frames.
**Where**: `packet_io.c` handle_tun_tx()

### 7. RX VLAN Tag Strip

**Problem**: Kernel can't parse received frames — ARP doesn't resolve despite frames arriving.
**Root cause**: ASIC inserts 802.1Q VLAN tag (81 00 00 01) on all frames
forwarded to CPU port. The kernel sees an unexpected VLAN tag before the
EtherType and can't parse the protocol.
**Fix**: Strip 4-byte VLAN tag at offset 12 in the RX path before writing
to TUN interface.
**How we found it**: Added hex dump to RX path in packet_io.c. Received
frames showed `ff ff ff ff ff ff 6c b2 ae cd 13 33 81 00 00 01 08 06`
— VLAN tag 0x8100 between src MAC and EtherType.
**Where**: `packet_io.c` handle_asic_rx()

### 8. Protocol Packet CPU Punt (ARP/DHCP)

**Problem**: ASIC receives ARP broadcasts but drops them — never reach CPU.
**Root cause**: `PROTOCOL_PKT_CONTROLr` (per-port register) defaults to 0,
meaning ARP/DHCP/BPDU packets are forwarded normally through L2 but not
copied to CPU.
**Fix**: Set `ARP_REQUEST_TO_CPU`, `ARP_REPLY_TO_CPU`, `DHCP_PKT_TO_CPU`
on all valid ports (1-72).
**How we found it**: RX DMA debug showed zero packets during ARP attempts.
Nexus showed it was sending ARP broadcasts (OutBcastPkts incrementing).
Searched BCM56840 register definitions for "ARP" and "CPU" and found
PROTOCOL_PKT_CONTROLr with per-port punt bits.
**Where**: `datapath.c` datapath_cpu_punt_init()

### 9. Static L2 MAC Entries for CPU

**Problem**: Unicast frames to our MAC addresses don't reach CPU.
**Root cause**: Each swp interface has a unique MAC (TUN-generated). Frames
destined to these MACs go through L2 lookup. Without an entry pointing to
CPU port, they're flooded or dropped.
**Fix**: Read each TUN's MAC via `ioctl(SIOCGIFHWADDR)` and add to L2 table
via `bmd_port_mac_addr_add(unit, port=0, vlan=1, mac)` after TUN creation.
**How we found it**: ARP resolved (uses protocol punt) but ICMP ping failed
(uses L2 forwarding). Added static L2 entries for all swp MACs → CPU.
**Where**: `packet_io.c` packet_io_init()

### 10. Disable CPU Port MAC Learning (CML_FLAGS)

**Problem**: ICMP replies still don't reach CPU despite static L2 entries.
**Root cause**: When CPU sends a frame via DMA, the ASIC hardware-learns the
source MAC on the CPU port. But the SOBMH directs the frame to a front-panel
port, causing the L2 entry to be overwritten: MAC → front-panel port instead
of MAC → CPU. Incoming unicast replies then get forwarded back out the
front-panel port (same-port drop) instead of to CPU.
**Fix**: Set `PORT_TABm.CML_FLAGS_NEW=0` and `CML_FLAGS_MOVE=0` on CPU port 0.
This disables hardware MAC learning for CPU-originated frames, letting the
static L2 entries remain authoritative.
**How we found it**: After adding L2 entries and protocol punt, ARP worked
(protocol punt bypasses L2) but ICMP didn't (uses L2 forwarding).
RX counter didn't increase during ping → frame never reached CPU.
Traced through BCM56840 L2 learning architecture: PORT_TABm CML_FLAGS
controls per-port learning behavior. Setting to 0 on CPU port fixed it.
**Where**: `datapath.c` datapath_cpu_punt_init()

---

## Documentation

Local documents that go deeper than this README:

**Start here**

| File | Topic |
|------|-------|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | The runtime software stack, how routes/packets reach the chip, and **where all the knowledge came from** (OpenMDK, Cumulus RE, datasheets, upstreams) + licensing posture |
| [`docs/ONIE_IMAGE_BUILD.md`](docs/ONIE_IMAGE_BUILD.md) | How to build the installable image — full build, fast incremental path, and build-environment gotchas |
| [`docs/DUAL_SLOT.md`](docs/DUAL_SLOT.md) | The A/B slot layout, U-Boot env + scripted auto-rollback, per-slot initramfs, and `nos-upgrade`/`nos-slot-clone` |

**Boot / flash**

| File | Topic |
|------|-------|
| [`BOOT.md`](BOOT.md) | AS5610 boot flow: U-Boot env, FIT image format, kernel config, initramfs, ONIE recovery |
| [`docs/FLASH_MTD_AND_ONIE_RECOVERY.md`](docs/FLASH_MTD_AND_ONIE_RECOVERY.md) | NOR flash / MTD layout and reaching ONIE from the running OS |
| [`installer/ONIE_ISSUES.md`](installer/ONIE_ISSUES.md) | ONIE BusyBox quirks and the workarounds baked into the installers |

**Datapath / chip / control plane**

| File | Topic |
|------|-------|
| [`docs/EDGED_ARCHITECTURE_AND_OPERATIONS.md`](docs/EDGED_ARCHITECTURE_AND_OPERATIONS.md) | edged daemon internals + operations |
| [`docs/DATAPATH_BRINGUP.md`](docs/DATAPATH_BRINGUP.md) | The working L2/L3 datapath and the bugs solved to get there |
| [`docs/ECMP_AND_OSPF_BRINGUP.md`](docs/ECMP_AND_OSPF_BRINGUP.md) | Hardware ECMP + OSPF control-plane punt |
| [`docs/CHIP_REGISTER_REFERENCE.md`](docs/CHIP_REGISTER_REFERENCE.md) | Every chip register/memory edged reads or writes |
| [`docs/TECHNICAL_DEEPDIVE_BRINGUP_ORDER.md`](docs/TECHNICAL_DEEPDIVE_BRINGUP_ORDER.md) | Dependency-ordered chip bring-up recovered from Cumulus captures |
| [`docs/IPROC_SUBWINDOW_ACCESS.md`](docs/IPROC_SUBWINDOW_ACCESS.md) | PAXB sub-window 7 remap to reach CMICm registers |

**SerDes / optics + narrative**

| File | Topic |
|------|-------|
| [`docs/TECHNICAL_REFERENCE_OPTICS_SERDES.md`](docs/TECHNICAL_REFERENCE_OPTICS_SERDES.md) | SFP+/QSFP optics + SerDes signal chain |
| [`docs/QSFP_40G_INVESTIGATION.md`](docs/QSFP_40G_INVESTIGATION.md) | 40G QSFP bring-up (solved) |
| [`docs/10G-LINK-BRINGUP-CHECKLIST.md`](docs/10G-LINK-BRINGUP-CHECKLIST.md), [`docs/WC40-TX-DRIVER-ANALYSIS.md`](docs/WC40-TX-DRIVER-ANALYSIS.md) | 10G link checklist + Warpcore TX-driver analysis |
| [`docs/JOURNEY_WRITEUP.md`](docs/JOURNEY_WRITEUP.md) | Narrative field notes of the whole bring-up |

**Kernel**

| File | Topic |
|------|-------|
| [`docs/KERNEL_UPGRADE_5.15.md`](docs/KERNEL_UPGRADE_5.15.md), [`docs/KERNEL_ROADMAP_6.x.md`](docs/KERNEL_ROADMAP_6.x.md) | The 5.10 → 5.15 → 6.1 LTS upgrade write-up and roadmap |

Build-side references:

| File | Topic |
|------|-------|
| `scripts/build-all.sh` | Single-Docker full build (debootstrap variant); the shipped image uses the Buildroot base + `assemble-rootfs-from-base.sh` (see [`docs/ONIE_IMAGE_BUILD.md`](docs/ONIE_IMAGE_BUILD.md)) |
| `scripts/pre-build-checks.sh` | Pre-build sanity assertions (IND_40BITIF=bit15, PAXB sub-window 7, no CPLD writes > 0x1F, no stale `switchd`, CDR-reset wired) |
| `installer/install.sh` / `install-dual-slot.sh` | Single-slot / dual-slot ONIE installer headers |

---

## Reverse Engineering Data

This NOS was built using reverse-engineered data from Cumulus Linux 2.5 running
on the same hardware. The RE project is at:
[github.com/wrightca1/edgecore-5610-reverse-engineering](https://github.com/wrightca1/edgecore-5610-reverse-engineering)

Key RE findings used (parameter values only, no proprietary code):
- TX driver values: idriver=2, predriver=3 (from live TX_DRIVERr capture)
- MDIO PHY address map: 6 addresses across 3 buses (from GDB MIIM capture)
- Retimer EQ coefficients: channels=12, cdr_rst=28/16, tap_dem=23
- LED programs: 256-byte CMIC bytecode (from /etc/bcm.d/)
- I2C bus topology: 70 buses, 133 devices (complete sysfs enumeration)
- S-Channel protocol: 1789 operations captured via GDB breakpoint
- CMICm register architecture: direct window vs PIO indirect access
- CPU punt: `modreg cpu_control_1` from Cumulus rc.datapath_0

---

## License

- Platform drivers: GPL-2.0-or-later
- OpenMDK: Broadcom Switch APIs License
- EdgeNOS components: GPL-2.0-or-later
