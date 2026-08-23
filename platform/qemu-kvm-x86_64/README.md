# platform/qemu-kvm-x86_64 — EdgeNOS x86_64 virtual switch (QEMU/KVM)

The first ASIC-less EdgeNOS target: a VM for EVE-NG, containerlab and plain qemu, on Intel
and AMD hosts, under legacy BIOS or UEFI. It exists so the control plane, the platform
framework, the package/image system and (M2) the `asic_ops` datapath seam can be built,
tested and CI'd without a switch, and so labs can run EdgeNOS fabrics.

Switch-DB key: **`x86_64-kvm_x86_64-r0`** — ONIE's own machine string for its `kvm_x86_64`
reference VM, so the ONIE install flow works unchanged.

| axis | entry |
|---|---|
| arch | `x86_64` (`switchdb/arch/x86_64.yml`, `arch/x86_64/`) — generic x86-64 baseline |
| asic | `vswitch` (`switchdb/asic/vswitch.yml`) — no silicon; Linux netdevs + kernel forwarding |
| platform | `switchdb/platforms/qemu-kvm-x86_64.yml`, this directory |

## What the image is

* **Base** (`arch/x86_64/buildroot`, built by `build/build-base-x86_64.sh`): Buildroot
  2026.02 LTS, kernel **6.1** LTS, glibc, **systemd** (networkd/resolved/timesyncd),
  busybox + GNU userland, python3, iproute2, ethtool, lldpd, nftables/iptables, tcpdump,
  iperf3, openssh. Kernel: virtio / e1000 / e1000e / vmxnet3, bridge, 802.1Q, bonding,
  VXLAN, VRF, MPLS, policy routing, netfilter, overlayfs + squashfs, BIOS + EFI stub.
* **Components** (`.epk`, overlaid by `imgbuild`):
  * `platform-svc` — port naming, networkd defaults, platform class, boot-time L3 config
  * `frr` — FRR 10.5 (Buildroot package: zebra/bgpd/ospfd/ospf6d/staticd/bfdd + vtysh): EVPN-VXLAN,
    BGP unnumbered, EVPN-MH, BFD — a DC fabric control plane. The daemons file, default config,
    unit enablement and the **startup-config** mechanism (`/etc/edgenos/startup/{netconf.sh,
    frr.conf,daemons,sysctl.conf}`, applied by `edgenos-startup.service` before FRR; the vrnetlab
    launcher pushes a `/startup` bind into it) ship in this component. (`quagga` — static Quagga
    1.2.4 via `build/build-quagga-x86_64.sh` — is kept as an alternative spec for parity with
    the hardware boards.)
  * `edgenos-cli` — on-box `edgenos` (version, platform hal, pkg)
* **Persistence**: `squashfs-overlay` — `EDGENOS-BOOT` (ext4) carries
  `/edgenos/{bzImage,initrd.img,rootfs.sqsh}` + `/grub/grub.cfg`; `EDGENOS-DATA` (ext4) is
  the overlay upper, so every change (configs, packages, journal) survives reboots. The
  initramfs (`arch/x86_64/buildroot/board/x86_64/initramfs/init`) assembles the root.

## Ports and addressing

| inside the VM | what | EVE-NG | containerlab |
|---|---|---|---|
| `ma1` | first PCI NIC = management, **DHCP**, in VRF `mgmt` (table 1000; `ip vrf exec mgmt …` for mgmt-side tools; sshd/vtysh accept via l3mdev_accept) | first port (`ma1`) | container eth0 / vrnetlab usernet |
| `ge0 … geN-1` | every further NIC, PCI order, admin-up, unaddressed | `ge0`… | `eth1`… |

Naming is done at boot by `edgenos-ports.service` (`services/edgenos-ports.sh`), so it is
the same whatever the hypervisor calls the NICs. Addresses: `vtysh` (zebra/ospfd/bgpd) or
`/etc/edged/addrs.conf` + `routes.conf` (applied by `edgenos-l3.service`), both persistent.

Default logins: **root / edgenos** and **admin / admin** (operator account, `wheel` + `frrvty`,
so `vtysh` works) — serial console (`ttyS0`, 115200), VGA (`tty0`) and SSH. containerlab nodes
also get `clab / clab@123` from the launcher.

## Build (any x86_64 Linux box, no docker, no vendor SDK)

```sh
build/build-base-x86_64.sh                     # Buildroot: kernel + rootfs + initrd + GRUB, captures base .epk  (~30 min first time)
build/build-quagga-x86_64.sh                   # static quagga with the Buildroot toolchain
bin/edgenos pkg build packaging/specs/qemu-kvm-x86_64/platform-svc.yml --source-root . --platform x86_64-kvm_x86_64-r0
bin/edgenos pkg build packaging/specs/qemu-kvm-x86_64/quagga.yml       --source-root . --arch x86_64 --asic any
bin/edgenos pkg build packaging/specs/edgenos-cli.yml                  --source-root . --arch any --asic any   # (spec paths: see note)
bin/edgenos build x86_64-kvm_x86_64-r0 --source-root .
```
→ `output/images/EdgeNOS-<ver>-x86_64-kvm_x86_64-r0.bin` (ONIE installer) and
`output/images/EdgeNOS-<ver>-x86_64-kvm_x86_64-r0.qcow2` (ready-to-boot disk).

`build/build-vm-image.sh` runs all of the above in order (`BR_TRIM=1` keeps the Buildroot
output ~1 GB for CI caches).

## Run

**qemu (quick check)**
```sh
qemu-system-x86_64 -enable-kvm -m 1024 -nographic \
  -drive file=EdgeNOS-<ver>-x86_64-kvm_x86_64-r0.qcow2,if=virtio \
  -netdev user,id=m,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=m \
  -netdev tap,id=p0,ifname=tap0,script=no -device virtio-net-pci,netdev=p0      # ge0 (optional)
```
UEFI: add `-bios /usr/share/ovmf/OVMF.fd` (or `-drive if=pflash,...`) — same image.

**EVE-NG** (Intel or AMD host, EVE-NG 5/6/7): copy the qcow2 + `tools/eve-ng/` to the EVE
host and run `sudo tools/eve-ng/install-eve-template.sh EdgeNOS-<ver>-...qcow2`. Adds the
"EdgeNOS" node type (telnet console, virtio NICs: `ma1`, `ge0`…).

**containerlab** (kind `generic_vm` via vrnetlab):
`tools/containerlab/build-clab-image.sh EdgeNOS-<ver>-...qcow2` → docker image
`vrnetlab/edgenos_vswitch:<ver>`; example topology in `tools/containerlab/examples/`.

**ONIE** (the real-switch flow, also works in ONIE's KVM VM):
`onie-nos-install http://<server>/EdgeNOS-<ver>-x86_64-kvm_x86_64-r0.bin` — creates
EDGENOS-BOOT/EDGENOS-DATA next to ONIE's partitions (GPT), installs GRUB (BIOS via ONIE's
`grub-install`, UEFI via our `bootx64.efi` + `efibootmgr`), keeps an "ONIE" menu entry.

## Files

| path | role |
|---|---|
| `platform.py` | `EdgeNOSPlatform_x86_64_kvm_x86_64_r0`: dynamic `ge*` port list, hypervisor/NIC info, HAL (hwmon thermals; fans/PSUs/SFPs unsupported), `baseconfig()` = port naming |
| `services/edgenos-ports.{sh,service}` | PCI-ordered naming: `ma1` + `ge0..N`; writes `/run/edgenos/ports` |
| `services/edgenos-l3*`, `config/addrs.conf`, `config/routes.conf` | boot-time L3 config (same format as AS4610/AS5610) |
| `config/frr/*`, `services/frr-edgenos.conf`, `services/edgenos-startup.*` | FRR daemons/config, unit drop-in, startup-config applier |
| `services/{zebra,ospfd,ospf6d,bgpd}.service`, `config/*.conf` | Quagga units/confs (alternative `quagga` component) |
| `config/network/*` | systemd-networkd: `mgmt` VRF + `ma1` DHCP in it, `ge*` up/unaddressed/keep-config, `cpu0` |
| `boot/grub.cfg` | the menu on EDGENOS-BOOT (normal, rescue, ONIE chain) |
| `board.yml` | board manifest |

## Real-world run: the ecloud two-DC EVPN-VXLAN lab on EdgeNOS

`github.com/aramidetosin/ecloud-containerlab` (`build_clab.py --edgenos`) replaces all 20 Cumulus
VX switches of a two-DC lab with this image: spines, leaves with EVPN-MH dual-homed hosts, border
leaves, aggregation, two tenants (L2 + L3 VNIs), k8s/Cilium BGP, a GoBGP anycast controller and
two PA-VM firewalls. Configs are the Cumulus-rendered `/etc/network/interfaces` + `frr.conf`,
translated (`swpN→ge(N-1)`, per-VNI vxlan devices, VRR macvlans) into the startup-config. Result
on both an AMD EPYC and an Intel Xeon host: all 20 switches healthy ~1 min after deploy, every
eBGP-unnumbered IPv4 + EVPN session Established, L2/L3 VNIs and cross-DC forwarding in both
tenants, EVPN-MH LAGs (host bond partner = ES sys-mac), Cilium BGP, GoBGP, k8s 6+4 Ready, the
app answered through the PAN NAT from the clients — the README verification of the original lab.

### Lessons from the lab run (folded into the image / the lab configs)
* **802.1Q subinterfaces vs networkd**: `20-ge.network` matched `ge1.100` too (`Name=ge*`); networkd
  configured those later-created subinterfaces and, with its default `KeepMaster=no`, pulled them out
  of their VRF (a race that only bit on the slower host). Now `Kind=!vlan` + `KeepMaster=yes` +
  `KeepConfiguration=yes`.
* **Anycast gateway MAC on a VLAN-aware bridge**: a macvlan above the SVI only gives the bridge a
  VID-less local FDB entry, so frames to the gateway MAC in that VLAN miss the (MAC, VID) lookup, get
  flooded to every port and the VNI, and the EVPN-MH peer (same anycast MAC) routes them a second
  time (3 copies of every routed packet). Pin it per VLAN:
  `bridge fdb replace <anycast-mac> dev br_default vlan <vid> self permanent` (switchd does this on
  Cumulus; the ecloud translator emits it).
* **802.3ad on virtio**: the link speed is "Unknown", the actor key is 0, no LACPDUs. The port
  service sets `ethtool -s geN speed 1000 duplex full autoneg off`.
* **mgmt VRF**: vrnetlab/containerlab give every VM `10.0.0.15/24` on `ma1`; keep `ma1` in VRF `mgmt`
  or it collides with fabric addresses in the default VRF.

## Verified (2026-08)

| | BIOS | UEFI | EVE-NG 7 | containerlab (routed) | containerlab (vswitch M2) | ONIE install |
|---|---|---|---|---|---|---|
| AMD EPYC host | ✅ | ✅ | ✅ (qemu64 CPU) | ✅ OSPF Full, h1→h2 | ✅ | ✅ BIOS + UEFI (ONIE kvm ISO) |
| Intel Xeon host | ✅ | — | ✅ (QEMU 2.4.0 default) | ✅ | ✅ | — |

(`tools/qemu/smoke-test.py`, `tools/containerlab/examples/*.clab.yml`, `tools/onie/`; ~15 s to login.)

## Status / roadmap

* **M1 (this)** — mgmt plane + Quagga, Linux kernel forwarding (`datapath: none`), qcow2 +
  ONIE installer, EVE-NG + containerlab packaging, Intel + AMD, BIOS + UEFI.
* **M2 (this, opt-in)** — `edged-vswitch`: an `asic_ops` L2 learning switch over the `pge*`
  netdevs (AF_PACKET) with the CPU as a port (`cpu0` TAP); `edgenos-datapath-mode vswitch
  [--now]`. VLANs / L3 offload behind the same seam are the follow-ups.
* CI: this platform needs none of the external source trees, so it can be the target that
  builds end-to-end in GitHub Actions (base build is cacheable; compose + boot-test is minutes).
