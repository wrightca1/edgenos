# EdgeNOS on Accton AS5610-52X

> ONIE platform string: **`powerpc-accton_as5610_52x-r0`**  ·  EdgeNOS **0.1.0**  ·  status: **production**

## Hardware

| | |
|---|---|
| CPU | PowerPC 32-bit (Freescale e500v2) (`powerpc`) |
| Switch ASIC | Broadcom BCM56846 (Trident+) (`bcm56846`) |
| Kernel | 6.1 |
| Datapath | `edged` |
| Verify the string on the box | `onie-sysinfo -p` → `powerpc-accton_as5610_52x-r0` |

## Download

Grab the installer for this switch from the EdgeNOS releases:

- **`EdgeNOS-0.1.0-powerpc-accton_as5610_52x-r0.bin`**

## Install

### 1. Get the switch into ONIE install mode

Power on (or reboot) and at the boot menu choose **ONIE → ONIE: Install OS**
(or from a running NOS: `onie-select -i -f` then reboot).

### 2. Install over the network

Serve `EdgeNOS-0.1.0-powerpc-accton_as5610_52x-r0.bin` from any HTTP/TFTP server, then from the ONIE prompt:

```sh
onie-nos-install http://<your-server>/EdgeNOS-0.1.0-powerpc-accton_as5610_52x-r0.bin
```

ONIE downloads the image, runs the EdgeNOS installer, writes the OS to disk,
sets the bootloader, and reboots into EdgeNOS automatically.

> This switch uses a self-extracting installer (`.bin`): it partitions the disk,
> writes the kernel FIT + rootfs squashfs, and configures U-Boot.

## Verify after first boot

```sh
# version + platform identity
cat /etc/edgenos/version.json
cat /etc/os-release | grep EDGENOS_
# what the image is made of (self-describing package list)
ls /var/lib/edgenos/epkg/installed/
```

Expected: `EDGENOS_ARCH=powerpc`, `EDGENOS_ASIC=bcm56846`, `EDGENOS_KERNEL=6.1`.

## What's installed

Components on this image: `edged`, `linux-kernel-bde`, `linux-user-bde`, `bde-tmon`, `quagga`, `platform-svc`.

## Recover / reinstall

Boot back into ONIE and either reinstall (`onie-nos-install …`) or uninstall (`onie-nos-uninstall`). ONIE is the safety net — it can always be re-entered from the boot menu.

---

_Generated from `switchdb/platforms/accton-as5610-52x.yml` by `edgenos docs`._
