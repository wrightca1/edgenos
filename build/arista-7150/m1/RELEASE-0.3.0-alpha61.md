# 0.3.0-alpha61 — a transcribed file retires: 83,790 → 58,364 relocated pairs

alpha59's register diff found that `fm6000_l2arpre` and `fm6000_l2arseq` are
alternatives for the same block, and that `l2arpre` is the incomplete one — 25,426
writes to `l2arseq`'s 29,110, because it captured only the first write of two-write
sequences. That fixed standalone mode. It did not retire the file, because the
replay path still selected `l2arpre` by default.

It does now. **`fm6000_l2arpre.c` is deleted**, and with it **25,426 transcribed
pairs — 30% of everything in the tree that was transcription rather than authorship.**

## Why it looked un-retirable, and wasn't

`L2ARSEQ=1` had been tried in the replay path and failed, with a recorded signature:
multicast punt survived, unicast forwarding did not. That result is why `l2arpre`
was the default.

But the two arms differed in **two ways at once**:

| | write set | placement |
|---|---|---|
| `L2ARPRE=1` | 25,426, incomplete | `gen_preloop` — hoist only pre-anchor writes, leave in-loop writes where the replay put them |
| `L2ARSEQ=1` | 29,110, complete | `gen_list_early` — hoist **every** L2AR write to the front |

Only placement was ever implicated. So the untried third arm is the obvious one:
keep `l2arpre`'s proven placement, swap in `l2arseq`'s complete write set.

    gen_preloop '0014' fm6000_l2arseq L2AR

`l2arseq` is a strict superset by address — 15,201 vs 15,185, **zero addresses
exclusive to `l2arpre`** — and they disagree on the final value at 422 addresses,
where alpha59's diff showed `l2arseq` is the correct one.

## Measured, and it is clean

| | alpha59 (`l2arpre`) | alpha61 (`l2arseq` pre-loop) |
|---|---|---|
| addresses matching a reference working boot | 413/413 | **413/413** |
| kernel routes | 44 | 44 |
| routes in silicon | 14 | 14 |
| `et1 rx` | 46 | 35 |
| unicast **to** the switch | 0% loss | **0% loss** |
| unicast **through** the switch | 0% loss | **0% loss** |
| `post-spico` | `et1=0cc0/0940` | `et1=0cc0/0940` |

Unicast is tested explicitly and separately from OSPF, because the earlier
`L2ARSEQ` failure was precisely "multicast punt survives, unicast does not". A
routing adjacency coming up is not evidence that this worked.

`L2ARSEQ_PRELOOP` is now the default arm. `L2ARSEQ=1` (the hoist-everything arm)
is kept as an explicit override so the original failure stays reproducible.

## ⚠ A packaging bug this uncovered: deleting source did not delete the tool

Removing `fm6000_l2arpre.c` and rebuilding produced an image that **still contained
and still ran `fm6000_l2arpre`**. Two independent causes, both now fixed:

1. Staging globbed `payload/fm6000_*`, and `payload/` keeps binaries from previous
   builds. The build now records exactly what it compiled and stages only that,
   naming anything stale it declined to ship.
2. The base initramfs is a **previous release**, so it already carried the tool, and
   overlaying onto it left the old copy in place. The build now prunes any
   `fm6000_*` from the overlaid image that it did not build and has no source for,
   and says so.

An image must not contain a tool whose source is gone. Verified: `l2arpre` is absent
from the image and from `/usr/bin` on the running switch.

A side effect worth noting — the staged tool count went 69 → 70, because the old
glob `payload/fm6000_*` never matched `fm6000reg` or `fm6000load` (no underscore).
Those were only ever reaching the image by surviving in the base initramfs. They are
now built from current source and staged explicitly.

## Also in this release

- **`/mnt/flash/fullseq.conf`** is sourced by `init-m1` and passed through to the
  bring-up sequence, so generator gates can be changed without rebuilding and
  reflashing an image — the same reason `pace.conf` exists. This release was tested
  with `echo 'L2ARSEQ_PRELOOP=1' > /mnt/flash/fullseq.conf` before it became the
  default.
- **Corrected a wrong instruction in the boot menu.** It said the management
  interface is "DHCP by default"; the default is static `192.168.1.1/24`. DHCP was
  tried and abandoned — the mgmt NIC's link comes up at t=27s, after init — so the
  text described a configuration the image has not had for some time.

## Provenance

    AUTHORED     8,352 pairs in 11 files      (unchanged)
    TABLE       56,329 pairs in 29 files      (unchanged)
    RELOCATED   58,364 pairs in  4 files      (was 83,790 in 5)

The four remaining: `fm6000_l2arseq.c` (29,110), `fm6000_eplseq.c` (22,051),
`fm6000_mapperpre.c` (5,662), `fm6000_mgmt2pre.c` (1,541).

Retiring a transcribed file by showing another already covers it costs one boot and
removes real transcription. `eplseq`, `mapperpre` and `mgmt2pre` have not been
checked for the same relationship, and should be.

## Build

    VERSION=0.3.0-alpha61 KERNEL=ex/linux-i386 BASE_INITRD=ex/initrd-i386 \
        sh ./build-release-swi.sh -o $PWD/edgenos-7150-0.3.0-alpha61.swi

Relative paths work again — alpha59 fixed `BASE_INITRD` resolution and the `zcat`
suffix requirement.

md5 `fbd0fbf83374a1de2619e23659d436a3`, 19,024,819 bytes, verified on the switch
after transfer.
