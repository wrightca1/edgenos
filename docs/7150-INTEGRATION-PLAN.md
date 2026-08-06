# Bringing the 7150 into EdgeNOS proper

**2026-08-06.** How to land the FM6000 work as a real EdgeNOS platform — at parity with the AS5610
— package it for a switch that isn't ONIE, and merge it on GitHub.

> **The headline finding: EdgeNOS already has all the architecture this needs.** The 7150 is already
> registered in the switch DB with `boot: aboot`, `installer: aboot-swi`, `datapath: none`. The
> `struct asic_ops` seam exists, `asic/fm6000/fm6000_edged.c` exists to fill it, and
> `platform/arista-7150s-52/edged.c` was already written to bridge the CPU port to a TUN netdev.
>
> Much of today's tooling (`fm6000_portd`, `fm6000_route`) **re-implements existing seams**. The
> integration work is mostly *moving proven code into them*, not writing new subsystems.

---

## 1. Parity gap vs the AS5610

| capability | AS5610 (working) | 7150 today | what's needed |
|---|---|---|---|
| ASIC bring-up | SDK/BMD | ✅ our own cold init | — |
| Dataplane packets | ✅ | ✅ TX+RX verified | — |
| **Hardware routing** | ✅ via `edged` | ✅ **verified**, and we can program the FIB ourselves | fold into `l3.c` ops |
| **OSPF** | ✅ `quagga` (zebra+ospfd) | ❌ | **reuse the component as-is** — it's arch-portable and ASIC-agnostic |
| FIB → ASIC sync | `core/datapath/netlink.c` + `l3.c` | ❌ | implement FM6000 `l3` ops behind `asic_ops` |
| Port netdevs (`swp*`) | ✅ | ⚠️ `edged-7150` TUN bridge exists but unproven; `fm6000_portd` is a working prototype | finish inside `edged-7150` |
| L2 / VLAN | ✅ `l2.c`, `vlan.c` | ❌ | later |
| Platform services | 9 (fan, diag, init, SFP, retimer, swp-l3) | 2 (`scd-setup`, `sfp-enable`) | **fan/thermal is the safety gap** |
| CLI / WebUI | ✅ shared components | ❌ not packaged | add to components |
| Packaging | ONIE `.bin` | ❌ hand-built SWI | implement `aboot-swi` |

**OSPF is not a research problem.** `core/control-plane/` already ships zebra + ospfd with a
cross-build recipe and systemd units, and `platform/accton-as5610-52x/deploy/ospfd.conf` shows the
pattern. The 7150's switch-DB entry already lists `quagga` as a component. What's missing is the
layer *below* it: routes that zebra installs into the kernel must reach the ASIC.

## 2. The one real piece of new work: FIB sync

```
ospfd ──> zebra ──> kernel FIB ──> [ netlink.c ] ──> l3.c ──> asic_ops ──> FM6000
                                        ▲                                    ▲
                                   already exists                   we now know how (§ROUTING-FIB)
```

Everything on the left exists and is proven on the 5610. We now have the right-hand side:

| operation | how | status |
|---|---|---|
| add/del route | sorted prefix array `0x33bxxx` + shadow, action array `0x337xxx`, commit strobe | ✅ decoded, `fm6000_route` works |
| nexthop/adjacency | `NEXTHOP 0x160000 + 10*idx`, `w0=MAC[5:2]`, `w1=GLORT<<16\|MAC[1:0]` | ✅ decoded, matches EOS's ARP |
| port netdev | TAP + punt/inject with inline F64 tag | ⚠️ prototype: TX works, RX partially |
| FFU slice setup | one-time ~900 writes | ❌ undecoded — take from the replay |

So: implement `fm6000_l3_route_add/del` and `fm6000_l3_nexthop_set` behind `asic_ops`, port
`fm6000_route.c`'s logic into it, and `edged-7150` gets OSPF-driven hardware routing for free.

## 3. Packaging: a switch that isn't ONIE

`packaging/imgbuild.py` implements two installer envelopes:

```
onie-sfx  -> version-templated install.sh + tar(FIT, rootfs.sqsh)   (.bin)   [AS5610]
onl-swi   -> zip(rootfs.sqsh, manifest.json) SWI [+ mkshar ONIE installer]
```

The switch DB already says the 7150 wants **`installer: aboot-swi`** — and `imgbuild.py` contains
**zero** references to `aboot`. That is the gap, and it is small.

**Aboot's contract** (verified live, and the 7150 enforces no signing):

```
edgenos-m1.swi = ZIP {
    version        key=value metadata Aboot reads
    boot0          stage-0 shell entry: unzip kernel+initrd, kexec
    linux-i386     bzImage
    initrd-i386    initramfs cpio.gz
}
```

That differs from `onl-swi` (which ships `rootfs.sqsh` + `manifest.json`). Two options:

| option | shape | verdict |
|---|---|---|
| **A. initramfs SWI** | kernel + initramfs, everything in RAM | what we run today; simple, proven, but the rootfs is rebuilt per image |
| **B. squashfs SWI** | kernel + small initrd that loop-mounts `rootfs.sqsh` from flash, then `switch_root` | **recommended** — matches the EdgeNOS model, reuses the composed rootfs from `imgbuild`, and `build-m1-rootfs.sh --squashfs` already exists |

Recommendation: **B**, implemented as a new `aboot-swi` envelope that emits
`zip(version, boot0, linux-i386, initrd-i386, rootfs.sqsh)`. `boot0` gains a loop-mount +
`switch_root`. This makes the 7150 a first-class `imgbuild` target instead of a bespoke script, and
keeps one rootfs-composition path for all platforms.

**Persistence.** No ONIE partition. `/mnt/flash` (the existing Aboot FAT partition, 264 MB free) is
the natural home for `rootfs.sqsh`, config and the operator-supplied firmware. `boot-config` selects
the image — and `init-m1` already self-reverts it to EOS on each boot, which is a good failsafe to
keep.

## 4. Merging on GitHub

Current: branch `feature/arista-7150-fm6000`, **60 commits ahead**, history already cleaned
(blobs stripped, fresh clone 1.1 MB, zero tracked binaries — see `PROVENANCE.md`).

Blockers to resolve before a merge request:

1. **Cumulus-derived tables still in-tree** (`asic/bcm56846/generated/*`, `core/datapath/generated/cmp_regs.h`,
   `docs/cumulus-acl-capture/*`). Same class as the FM6000 replay — transcriptions of a proprietary
   NOS. `cmp_regs.h` is referenced by `core/datapath/datapath.c`, so removing it is not free.
   **Decision needed** (see `PROVENANCE.md §2.4`) — this is the one thing I would not merge without.
2. **Prototypes vs product.** `fm6000_portd` / `fm6000_route` should either move behind `asic_ops`
   or be clearly marked as `tools/`-grade diagnostics. Shipping both a seam and a bypass invites rot.
3. **Switch-DB status.** Flip `datapath: none` → `edged-fm6000` and `status: planned` → the honest
   current state once the `l3` ops land.
4. **Don't claim more than is proven.** `FEATURE-STATUS-7150.md` is the honest inventory; Et2 copper
   is still intermittent and the pacing theory has one controlled trial. Say so in the MR.

Suggested sequencing: land the packaging + switch-DB changes first (self-contained, low risk), then
the `asic_ops` FM6000 backend, then flip status.

## 5. Order of work

1. **Thermal/fan service** — the only safety item; the 5610 has `fan-controller.service`, we have nothing.
2. `aboot-swi` envelope in `imgbuild.py` + a `packaging/specs/arista-7150s-52/` component set.
3. FM6000 `l3` ops behind `asic_ops` (port `fm6000_route.c`).
4. Finish the TUN bridge in `edged-7150` (fold in `fm6000_portd`, incl. the RX descriptor-scan fix).
5. Enable the `quagga` component + a 7150 `ospfd.conf` → OSPF-driven hardware routing.
6. Resolve the Cumulus-table question, then open the MR.
