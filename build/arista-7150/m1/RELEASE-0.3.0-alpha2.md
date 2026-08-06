# EdgeNOS for the Arista DCS-7150S-52 — 0.3.0-alpha2 (early release)

**Built 2026-08-06.** `edgenos-7150-0.3.0-alpha2.swi`, 18,713,677 bytes.

**alpha2 adds thermal management** — see below. alpha1 had none and should not be used.

An early release: a switch that boots itself from cold, brings up an Intel FM6000 ASIC with no
vendor SDK, forwards in hardware, speaks OSPF, and programs the routes it learns into silicon —
**with no EOS running.**

This is alpha. Read the limitations before putting it near anything you care about.

---

## Verified on hardware, from this exact image

```
cold boot -> Et1 0x8c0 rx=1, Et2 0xcc0 rx=1        both 10G links up automatically
thermal   -> sensors found, die 37 C, fans 100%->40%   automatic, no manual steps
OSPF converged in 48 s                              35 routes learned from the peer
fibd programmed 13 routes into the hardware FIB
ttl 50 -> 49 through an OSPF-learned prefix         forwarded by the ASIC
```

The whole chain runs from the image: **ASIC → punt → TAP → kernel → ospfd → zebra → kernel FIB →
fibd → ASIC.**

## What's in it

- Our own kernel + initramfs (busybox, SCD board driver, FM6000 DMA kmod, dropbear SSH)
- **21 FM6000 tools built from source in this repo** — cold bring-up, packet DMA, port netdev,
  route programming, FIB sync, diagnostics
- **Control plane**: Quagga zebra + ospfd (GPL) with working configs
- **Thermal control** (`thermal-control.sh`) — starts automatically at boot
- Automatic dataplane bring-up at boot (`init-m1`)
- `edgenos-up.sh` — one command for the whole stack

## What's deliberately NOT in it

Third-party works you must supply yourself from a **licensed EOS** on your own switch:

| file | goes on flash as |
|---|---|
| FM6000 microcode | `/mnt/flash/ucode_l2.raw`, `ucode_tail.raw` |
| register replay set (contains the SerDes SPICO firmware inline) | `/mnt/flash/fwd4.txt` |

Without them the image still boots, brings up management SSH, and says the dataplane is down. It
will not forward. See `docs/PROVENANCE.md`.

## Install

```sh
# copy the .swi to /mnt/flash, then
echo SWI=flash:/edgenos-7150-0.3.0-alpha1.swi > /mnt/flash/boot-config
sync && reboot
```

Aboot boots unsigned SWIs on this platform (verified — no signature, key or TPM enforcement).

**Recovery, if it will not boot:** serial console → spam Ctrl-C during boot → `Aboot#` prompt →
rewrite `/mnt/flash/boot-config` and `boot /mnt/flash/<image>.swi`. Aboot also has `wget`, so a
replacement image can be pulled with no working NOS at all.

**Safety net:** `init-m1` rewrites `boot-config` back to EOS on every boot, so an unattended reboot
always lands on EOS. That is deliberate — keep it.

## Bringing it up

The dataplane comes up automatically at boot. For the control plane:

```sh
sh /usr/lib/edgenos/platform/edgenos-up.sh
```

which loads the modules, brings up loopback, creates `et1`, and starts zebra, ospfd and fibd.
Edit `/etc/quagga/ospfd.conf` for your topology first.

## Thermal management

Starts automatically at boot; no manual step. Reads the MAX6658 (board + FM6000 die) on SCD SMBus
master 0 bus 2 and drives the four fans via the `raven-fan-driver` hwmon PWMs.

Measured on this image: fans drop from the hardware default **PWM 255 (~17,900 RPM) to PWM 102
(~11,700 RPM)** with the die steady at 37–38 °C — much quieter, same temperature.

**It is written to fail loud, not quiet.** In priority order:

| rule | behaviour |
|---|---|
| 1 | any sensor read failure, missing hwmon or unparsable value → **PWM 255** |
| 2 | never commands below `PWM_FLOOR` (40%), so airflow never stops |
| 3 | die ≥ 85 °C → **PWM 255** and a loud log line (crit is 100 °C) |
| 4 | ramping *down* needs a full 5 °C hysteresis band, so it cannot oscillate |
| 5 | a fan present but reading 0 RPM → **PWM 255** and a loud log line |
| — | on SIGTERM/SIGINT it sets **PWM 255** before exiting |

Both failure paths were tested on hardware, not just reasoned about: with the sensor removed it
goes to 255 and logs `SENSOR READ FAILED`, and on the next poll it re-instantiates the sensor and
resumes normal control.

Log: `/var/log/thermal`. Tunables are at the top of the script.

## Limitations — please read

- **Alpha.** One lab switch, one link partner, one afternoon of soak. Not a product.
- **Two ports.** Et1 (10GBASE-SR) and Et2 (10GBASE-CR). The other 50 have never been tried.
- **Et2 is intermittent.** The copper link comes up roughly 2 of 3 cold boots. Pacing the replay
  appears to help but the evidence is thin — see `docs/ET2-COPPER-LINK.md` for the full matrix,
  including the theories that were tested and *disproved*.
- **fibd cannot create new next hops.** The FFU action-array fields are still undecoded, so it
  reuses slots the boot config already pointed at the egress we want. That covers learned routes
  sharing a next hop — the normal case here — but it is not a general FIB, and it warns rather than
  silently dropping when there are more routes than slots.
- **The port netdev is a control path, not a data path.** `fm6000_portd` does a
  `TX_STOP → fill → TX_START` per frame (~10 ms), capping near 100 pps. Fine for ARP and OSPF
  hellos. Do not push traffic through it.
- **Thermal control is new and lightly soaked.** It works and fails safe (see below), but it has
  hours of runtime, not weeks. Watch `/var/log/thermal` the first time you leave it alone.
- **No L2 switching, VLANs, MAC learning, LAG, STP, ACLs, QoS, or IPv6 forwarding.**
- **The config is not persistent** — the initramfs is rebuilt from the SWI on every boot.
- Restarting `fm6000_portd` repeatedly without a chip reset can wedge the DMA rings (RX silently
  goes to 0). Reboot rather than restart.

## Reproducing this build

```sh
unzip -o edgenos-m1-bist17.swi linux-i386 initrd-i386     # known-good kernel + modules
VERSION=0.3.0-alpha1 KERNEL=linux-i386 BASE_INITRD=initrd-i386 \
  CONTROL_PLANE=<dir with zebra/ospfd/libs> \
  sh build/arista-7150/m1/build-release-swi.sh -o edgenos-7150-0.3.0-alpha1.swi
```

⚠ Do **not** use the tree's default `KDIR` kernel — that build produces an image with no management
NIC IRQ and no block devices, which removes both remote lifelines at once. See
`notes/analysis/m1-kernel-aug1-BROKEN.md`. Building Quagga: `platform/arista-7150s-52/deploy/README.md`.
