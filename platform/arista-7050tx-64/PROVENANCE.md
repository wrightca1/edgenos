# Provenance — what on this branch is ours, and what is not

Written because "can we make this our own?" is a question about *specific
files*, not a mood. Every item below is one of four things: **ours**, **someone
else's open code**, **EOS-derived**, or **an external dependency**.

---

## Ours, outright

Written from scratch against public documentation, our own measurements, and the
SDK's published API. No third-party code, no captures.

| file | what it is |
|---|---|
| `asic/bcm56855/bde_shim.c` | user-space BDE — PCI mapping, DMA pool, interrupt stubs |
| `asic/bcm56855/sdkpoc.c` | cold init, port bring-up, tap datapath, FIB sync |
| `platform/arista-7050tx-64/platmon.c` | sensors, PSU, fans, LEDs, cooling loop |
| `platform/arista-7050tx-64/initrd/init` | the initrd |
| `platform/arista-7050tx-64/kernel/config-7050tx64` | kernel configuration |
| `.../tools/*.sh`, `abootcatch.py` | image build, reset release, retimer, console |
| `.../deploy/*` | FRR configuration |

⚠ **An earlier version of this file said the platform layer "could be published
today". That was wrong, and a proper file-by-file audit before the public push
found two more items** — the cooling curve and the DS100KR800 retimer values,
both taken from the board's FDL, which is Arista Confidential. Neither appears in
Arista's open code. Both have since been moved out of the source and are read at
runtime from files the operator generates on their own switch with
`tools/fdl-extract.sh`. The claim was made from a partial look; the audit is what
should have come first.

The genuinely reassuring part is that **most of the platform layer is
independent by accident of how it was worked out.** The LED colour encoding came from Arista's
GPL `scd-led.c`, the fan CPLD register map from their GPL `crow-fan-driver.c`,
and the PSU PMBus address from their open SONiC platform tree — not from EOS.
The cooling curve came from the board's own FDL and was then *verified against
EOS behaviour* rather than copied from it: EOS was observed configuring 71% at
30 °C and 73% at 31 °C, and our independent interpolation reproduces both.
Observing a system's behaviour and reimplementing it is not derivation.

## Someone else's open code — usable, with obligations

| source | licence | how used |
|---|---|---|
| Arista `scd.ko` / `scd-hwmon.ko` | GPL-2.0 | **shipped as binaries in the SWI** |
| Arista `scd-led.c`, `crow-fan-driver.c` | GPL-2.0 | read for register maps; no code copied |
| Arista SONiC platform tree | open | read for the PSU model → PMBus address mapping |
| FRR 8.4.4 | GPL-2.0 | shipped unmodified from Debian |
| glibc and FRR's library closure | LGPL/GPL | shipped unmodified from Debian |

⚠ **We ship GPL binaries and must therefore offer their source.** `mkswi.sh`
puts prebuilt `scd.ko` and `scd-hwmon.ko` into the initrd, and the image carries
FRR and glibc. Distributing those obliges us to make the corresponding source
available. Today the image ships the binaries and says nothing. That is a real
compliance gap, it is cheap to close, and it is unrelated to whether the rest is
"ours".

## EOS-derived — the actual blocker

| item | source | status on this branch |
|---|---|---|
| PCS write replay, 63 registers | EOS capture | **REMOVED** |
| `config/config.bcm`, 549 properties | EOS capture | **REMOVED** — generate with `tools/mkconfigbcm.py` |
| cooling curve and level→speed table | FDL (Confidential) | **REMOVED** — read from `/etc/edgenos/cooling.conf` |
| DS100KR800 retimer tuning values | FDL (Confidential) | **REMOVED** — read from `/etc/edgenos/retimer.conf` |

**Nothing EOS-derived or FDL-derived is published on this branch.** The
mechanism is ours and is here; the board vendor's numbers are supplied at
runtime by whoever owns the switch, from the switch's own `/etc/fdl` and its own
NOS. `tools/fdl-extract.sh` and `tools/mkconfigbcm.py` do that, and both were
verified to reproduce exactly the values that used to be hard-coded.

⚠ **SCD register offsets are a deliberate exception, and the reason matters.**
`0x5000`, `0x6000`, `0x6050`, `0x6070`, `0x8000`, `0xA100` and friends came
originally from the FDL, but they also appear across **Arista's own open SONiC
platform tree** — 13 to 33 files each. Arista publishes them. They are public
facts about the hardware, and they stay.

The PCS table went because it cost nothing: a control run without it reaches
`link=1` identically, and the oracle register already read "locked" *before* the
replay ran. It was never the cause of anything.

`config.bcm` was the real problem and is now absent. It is a live capture of EOS's SDK property set
(`platform trident diag config`), remapped from unit 1 to unit 0 by
`tools/mkconfigbcm.py`. It carries the **port map** — which SerDes lane belongs
to which front-panel port — and the SDK will not even attach without it
(`soc_attach` fails with "Port config error !!"). So the board does not boot
without EOS-derived data, and no amount of relicensing the surrounding code
changes that.

### Replacing config.bcm — three routes, in increasing order of independence

1. **Generate it from the board's FDL.** The FDL gives, per front-panel port, the
   TSC macro, tx and rx lane separately, polarity inversion and pre-emphasis —
   everything the port map needs. `tools/fdl_portmap.py` and
   `crosscheck_portmap.py` in the research repo already do this and compare the
   result against the EOS capture. ⚠ This trades one problem for another: the
   `.fdl` is Arista Confidential. It is fine for a private build, useless for
   publication.
2. **Derive the port map by measurement.** Bring up each SerDes lane and
   determine empirically which front-panel port it lights, using loopback and a
   link partner. Slow, entirely ours, and it produces a port map with a
   defensible origin. This is the honest path.
3. **Have the operator supply it**, the way the SDK itself is supplied — ship no
   `config.bcm`, document its required contents, and let whoever runs the switch
   generate one from their own hardware. Weakest technically, strongest legally.

## External dependency — the Broadcom SDK

The OpenBCM SDK is **never vendored**, in this branch or the research repo. It is
referenced by `file:line` only, and `asic/bcm56855/Makefile` links against a tree
the builder supplies:

```
make STATIC=1 OPENBCM_SDK=/path/to/sdk-6.5.24
```

That is the same posture a NOS takes toward proprietary firmware: we ship the
integration, not the dependency. It keeps our source distributable without
resolving Broadcom's licence terms — but be clear about what it does *not* do:
**a build of this platform still requires an SDK the recipient must obtain
themselves**, and the resulting binary is a combined work. If EdgeNOS is ever
distributed as binaries, the SDK's terms have to be read properly; treating
"we didn't check it in" as a complete answer would be wishful.

The genuinely independent alternative is to drive the chip directly rather than
through the SDK — which is the road the FM6000/7150 work went down, and it is a
very large amount of work. It is not required to *use* this platform; it is
required to *own* it end to end.

---

## Summary

* The platform layer — sensors, fans, PSUs, LEDs, cooling, boot, image — is ours
  or built on Arista's own GPL code, and could be published today.
* The datapath is ours **except for `config.bcm`**, which is EOS-derived and
  load-bearing. That single file is what stops this being our own work.
* The Broadcom SDK is an unresolved external dependency, deliberately kept at
  arm's length.
* We ship GPL binaries without offering source. Fix that regardless of anything
  else here.


---

## Audit against what is actually published

Checked 2026-08-19, after finding that SCD register offsets we had treated as
confidential are published across Arista's own open tree. Reading something in a
confidential file is not the same as the thing being confidential, so each
withheld item was checked against Arista's GitHub, the SONiC build, and the
wider ecosystem.

| withheld here | is the data class public? |
|---|---|
| SCD register offsets | **yes** — Arista's own SONiC tree, 13–33 files each |
| SCD LED block map | **yes** — `scd.addLeds([(0x6050,'status'), (0x6060,'fan_status'), (0x6070,'psu1'), (0x6080,'psu2')])`, the same addresses and names |
| retimer tuning values | **yes** — `arista/drivers/ds125br.py` publishes per-board `amplitude[]`, `txDeEmphasis`, `rxEqualization`, and `disableCrc = 0x18` |
| fan / thermal policy | **yes** — `device/arista/x86_64-arista_7050_qx32/fancontrol` in sonic-buildimage |
| port map, polarity flips | **format yes, this board no** — `portmap_N=` in `stratum/stratum`; `phy_xaui_tx_polarity_flip_N` in `facebook/fboss` |

**`config.bcm` stays out, and the audit confirms why.** The format is thoroughly
public — Google and Meta both publish port maps and polarity flips — but nobody
publishes *this board's* values, and the ones we had came from a live capture of
the vendor NOS. A port map derived by measurement would carry none of that
problem, which is the argument for doing it.

**The retimer and cooling withholding was more cautious than it needed to be.**
Arista publishes the same class of data for their other boards, down to an
identical `disableCrc = 0x18`. The runtime-configuration split stays regardless,
because it costs nothing and keeps this branch free of anything taken from a
confidential source — but nobody should contort future work around treating
those numbers as secrets.

**An independent check of our own arithmetic.** Arista's published `fancontrol`
for a Trident2 box uses `MINPWM=179` across a 30–40 °C band on a `max6658` at
`0x4c`. This platform's fan scale of 180 was derived from a single observation,
and an unrelated public file agrees with it.

## Broadcom publishes this chip and this PHY

`Broadcom/OpenMDK`, Broadcom's own GitHub organisation, contains
`cdk/PKG/chip/bcm56855/` and `phy/PKG/chip/bcm84848/bcm84848_drv.c` — this exact
ASIC and PHY, the latter defining `BCM84848_PMA_PMD_ID0 0x600d`, the identity
read off the board. For anyone pursuing the independence question above, that is
vendor-published support rather than an SDK obtained under unexamined terms.

⚠ **Complete Broadcom SDK trees also appear on GitHub under third-party
accounts.** They are not Broadcom's organisation. A mirror existing is not a
licence, finding the SDK there changes nothing about its terms, and those
repositories are deliberately not cited as a source anywhere in this tree.
