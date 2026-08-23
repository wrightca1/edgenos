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
  * `quagga` — Quagga 1.2.4 zebra/ospfd/ospf6d/bgpd + vtysh, static x86_64
    (`build/build-quagga-x86_64.sh`)
  * `edgenos-cli` — on-box `edgenos` (version, platform hal, pkg)
* **Persistence**: `squashfs-overlay` — `EDGENOS-BOOT` (ext4) carries
  `/edgenos/{bzImage,initrd.img,rootfs.sqsh}` + `/grub/grub.cfg`; `EDGENOS-DATA` (ext4) is
  the overlay upper, so every change (configs, packages, journal) survives reboots. The
  initramfs (`arch/x86_64/buildroot/board/x86_64/initramfs/init`) assembles the root.

## Ports and addressing

| inside the VM | what | EVE-NG | containerlab |
|---|---|---|---|
| `ma1` | first PCI NIC = management, **DHCP**, sshd | first port (`ma1`) | container eth0 / vrnetlab usernet |
| `ge0 … geN-1` | every further NIC, PCI order, admin-up, unaddressed | `ge0`… | `eth1`… |

Naming is done at boot by `edgenos-ports.service` (`services/edgenos-ports.sh`), so it is
the same whatever the hypervisor calls the NICs. Addresses: `vtysh` (zebra/ospfd/bgpd) or
`/etc/edged/addrs.conf` + `routes.conf` (applied by `edgenos-l3.service`), both persistent.

Default login **root / edgenos** on the serial console (`ttyS0`, 115200), VGA (`tty0`) and
SSH.

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
| `services/{zebra,ospfd,ospf6d,bgpd}.service`, `config/*.conf` | Quagga units (`/opt/edgenos/<daemon>-x86_64`) and default confs |
| `config/network/*.network` | systemd-networkd: `ma1` DHCP, `ge*` up/unaddressed/keep-config |
| `boot/grub.cfg` | the menu on EDGENOS-BOOT (normal, rescue, ONIE chain) |
| `board.yml` | board manifest |

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
