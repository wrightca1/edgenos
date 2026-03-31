# EdgeNOS - Network Operating System for Edgecore AS5610-52X

Open-source NOS for the Edgecore AS5610-52X bare metal switch.

- **CPU**: Freescale P2020 (PowerPC e500v2, dual-core 1.2 GHz)
- **ASIC**: Broadcom BCM56846 (Trident+) -- 48x SFP+ 10GbE + 4x QSFP+ 40GbE
- **SDK**: OpenMDK (open-source CDK/BMD/PHY, no proprietary Broadcom SDK)
- **Kernel**: Linux 5.10.224 LTS
- **Root**: SquashFS + OverlayFS (read-only base + writable overlay)

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

### Docker Build (Recommended)

The entire build runs inside Docker -- no cross-compiler needed on the host.

```bash
# Clone the repo
git clone https://github.com/wrightca1/edgenos.git
cd edgenos

# Build the Docker container (downloads kernel + buildroot, ~5 min first time)
docker build --network host -t edgenos-builder .

# Run the full build (kernel + SDK + switchd + rootfs + ONIE installer)
mkdir -p output
docker run --rm --network host \
    -v $(pwd)/output:/build/output \
    edgenos-builder all

# Output: output/images/edgenos-as5610-52x.bin
```

### Build Targets

Build individual components for faster iteration:

```bash
# Interactive shell inside build environment
docker run --rm -it --network host \
    -v $(pwd)/output:/build/output \
    edgenos-builder bash

# Inside the container:
./docker-build.sh kernel      # Build Linux kernel + DTB (~5 min)
./docker-build.sh modules     # Build platform kernel modules
./docker-build.sh sdk          # Build OpenMDK libraries (CDK/BMD/PHY)
./docker-build.sh switchd     # Build switch daemon
./docker-build.sh rootfs      # Build root filesystem via Buildroot (~20 min)
./docker-build.sh image       # Create ONIE installer
./docker-build.sh all          # Full build
```

### Iterative Development

Mount source directories for live editing without rebuilding the container:

```bash
docker run --rm -it --network host \
    -v $(pwd)/output:/build/output \
    -v $(pwd)/asic/switchd:/build/asic/switchd \
    -v $(pwd)/platform:/build/platform \
    -v $(pwd)/config:/build/config \
    edgenos-builder bash

# Edit code on host, rebuild inside container:
./docker-build.sh switchd     # Rebuild just switchd (~10 sec)
./docker-build.sh image       # Repackage installer (~30 sec)
```

### Docker Compose

```bash
docker compose build                    # Build container
docker compose run --rm build           # Full build
docker compose run --rm build kernel    # Kernel only
docker compose run --rm build bash      # Interactive shell
```

### What the Docker Container Includes

| Component | Version | Purpose |
|-----------|---------|---------|
| Ubuntu 22.04 | base | Build environment |
| gcc-powerpc-linux-gnu | 11.4.0 | PPC cross-compiler |
| Linux kernel source | 5.10.224 | Kernel + modules |
| Buildroot | 2023.02.9 | Root filesystem |
| mkimage | - | FIT image creation |
| dtc | - | Device tree compiler |
| mksquashfs | - | SquashFS creation |
| qemu-user-static | - | PPC binary testing |

---

## Installing on the Switch

### Step 1: Boot into ONIE Install Mode

Connect serial console (115200 8N1). Power on the switch and interrupt U-Boot:

```
Hit any key to stop autoboot
=> run onie_bootcmd
```

Or from an existing OS: `onie-nos-install`

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
U-Boot 2013.01 (in NOR flash)
  nos_bootcmd -> usb read FIT from sda5 -> bootm
        |
Linux 5.10.224 (PowerPC e500v2, SMP)
  gianfar (eth0) | mpc-i2c (x2) | fsl-ehci (USB) | PCIe
        |
SquashFS rootfs (sda6) + ext2 overlay (sda3) + persist (sda1)
        |
systemd -> platform-init.sh -> switchd -> networking
                |                   |
                |                   +-- OpenMDK CDK/BMD/PHY
                |                   +-- bmd_reset -> bmd_init (WC firmware v0x0101)
                |                   +-- bmd_port_mode_set (10G/40G)
                |                   +-- bmd_tx/rx (DMA packet I/O)
                |                   +-- TUN interfaces (swp1-52)
                |
                +-- Load 12 kernel modules
                +-- GPIO init (QSFP reset, SFP TX enable)
                +-- 32x DS100DF410 retimer programming
                +-- CPLD LED + fan PWM
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
      etc/systemd/system/       Service files (switchd, platform-init, thermal)
      etc/modules-load.d/       Kernel module load order (12 modules)
      usr/sbin/                 Init scripts (platform-init.sh, switchd-init, etc.)
kernel/
  dts/as5610-52x.dts            Device tree (full I2C mux topology)
installer/
  install.sh                    ONIE self-extracting installer
asic/
  openmdk/                      OpenMDK (CDK/BMD/PHY) -- git submodule
  bde/                          BDE kernel modules (PCI, DMA, IRQ, mmap)
    linux-kernel-bde.c          Kernel BDE (ioread32/iowrite32 with PPC barriers)
    linux-user-bde.c            Userspace BDE bridge
  switchd/                      Switch daemon
    switchd.c                   Main daemon (init, main loop)
    bde_interface.c             BDE/CDK/BMD integration (register access, DMA)
    portmap.c                   Port config + link polling (bmd_port_mode_set)
    packet_io.c                 Packet I/O (TUN + bmd_tx/rx with DMA coherent)
    l2.c                        L2 MAC table (bmd_port_mac_addr_add/remove)
    l3.c                        L3 routing (software forwarding, HW needs OpenNSL)
    vlan.c                      VLAN management (bmd_vlan_create/port_add)
    netlink.c                   Netlink listener (route/neigh/link events)
  mdk-init/                     OpenMDK standalone init tool
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
| TX DMA (kernel → wire) | Working | OpenMDK xgs_dma + endianness fix |
| RX DMA (wire → kernel) | Working | OpenMDK xgs_dma + VLAN strip |
| ARP resolution | Working | PROTOCOL_PKT_CONTROLr punt |
| ICMP ping | Working | Static L2 + CML learning fix |
| TUN interfaces (swp1-52) | Working | Custom packet_io.c |

### Not Yet Implemented

| Component | Reason |
|-----------|--------|
| L3 hardware offload | Requires OpenNSL SDK (L3 is software-forwarded via kernel) |
| ACL/FP table programming | Requires OpenNSL |
| ECMP hardware hash | Works at L2; L3 ECMP needs OpenNSL |
| LLDP/STP/BGP | Install from packages (lldpd, mstpd, frr) |
| swp1 link | Retimer CDR on bus 11 doesn't lock (hardware issue on that channel) |

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
after CPS reset. Added `bde_set_dma_endianness()` called from switchd.c.
**Where**: `bde_interface.c` bde_set_dma_endianness(), `switchd.c`

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
