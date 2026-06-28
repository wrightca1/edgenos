# EdgeNOS on Accton AS4610-54

> ONIE platform string: **`arm-accton-as4610-54-r0`**  ·  EdgeNOS **0.1.0**  ·  status: **production**

## Hardware

| | |
|---|---|
| CPU | ARM 32-bit hard-float (Cortex-A9, ARMv7-A) (`armhf`) |
| Switch ASIC | Broadcom BCM56340 (Helix4) (`bcm56340`) |
| Kernel | 6.1 |
| Datapath | `bcmd` |
| Verify the string on the box | `onie-sysinfo -p` → `arm-accton-as4610-54-r0` |

## Download

Grab the installer for this switch from the EdgeNOS releases:

- **`EdgeNOS-0.1.0-arm-accton-as4610-54-r0.swi`**

## Install

### 1. Get the switch into ONIE install mode

Power on (or reboot) and at the boot menu choose **ONIE → ONIE: Install OS**
(or from a running NOS: `onie-select -i -f` then reboot).

### 2. Install over the network

Serve `EdgeNOS-0.1.0-arm-accton-as4610-54-r0.swi` from any HTTP/TFTP server, then from the ONIE prompt:

```sh
onie-nos-install http://<your-server>/EdgeNOS-0.1.0-arm-accton-as4610-54-r0.swi
```

ONIE downloads the image, runs the EdgeNOS installer, writes the OS to disk,
sets the bootloader, and reboots into EdgeNOS automatically.

> This switch uses an ONL-style installer (`.swi`): it installs the loader FIT +
> SWI under `/mnt/onl` and boots via the ONL loader.

## Verify after first boot

```sh
# version + platform identity
cat /etc/edgenos/version.json
cat /etc/os-release | grep EDGENOS_
# what the image is made of (self-describing package list)
ls /var/lib/edgenos/epkg/installed/
```

Expected: `EDGENOS_ARCH=armhf`, `EDGENOS_ASIC=bcm56340`, `EDGENOS_KERNEL=6.1`.

## What's installed

Components on this image: `bcmd`, `linux-kernel-bde`, `linux-user-bde`, `linux-bcm-knet`, `quagga`, `platform-svc`.

## Recover / reinstall

Boot back into ONIE and either reinstall (`onie-nos-install …`) or uninstall (`onie-nos-uninstall`). ONIE is the safety net — it can always be re-entered from the boot menu.

---

_Generated from `switchdb/platforms/accton-as4610-54.yml` by `edgenos docs`._
