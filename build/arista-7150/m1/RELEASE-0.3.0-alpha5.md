# EdgeNOS 7150 — 0.3.0-alpha5

**The first release where part of the ASIC bring-up is our own code rather than a replay of EOS.**

Arista DCS-7150S-52 · Intel FM6000 "Alta" · x86_64 · Aboot

## What changed since alpha4

`fm6000_safinit` programs the SAF store-and-forward matrix directly — **168 register writes**,
generated from our own board port table — replacing the **34,668 writes** EOS spends building the
same end state one port-bit at a time.

The matrix turns out to be four patterns over 56 ports:

```
0010000f 00000100 00000000   50 swept front-panel ports
00000007 00000000 00000000   ports 3, 20, 40
ffffffff ffffffff ffffffff   ports 0, 2   (internal)
ffffffff ffffffff 0003ffff   port 1
```

`fm6000-fullseq.sh` splits the operator's replay at the first in-loop SAF write, runs part one,
calls `fm6000_safinit`, then runs the remainder with the recorded SAF writes filtered out.
`SAFGEN=0` reverts to replaying EOS's accumulation.

**Replay: 389,809 → 355,480 writes.**

## Verified on hardware

Cold boot of this image on a real 7150S-52, with the **stock, unmodified** operator replay:

```
[fs]   SAF is ours: 219083 + safinit(168) + 136229
fm6000_safinit: 168 SAF writes (replaces 34668 from the replay)
[fs]   safinit rc=0

Et1 0x00000cc0     Et2 0x000008c0        both links up
SAF readback       0x0a0054 = 0010000f, 0x0a00a0 = 00000007   as generated
OSPF               adjacency, 35 kernel routes
hardware FIB       13 routes programmed into silicon
```

Equivalence was established before the image was ever booted: `fm6000_safinit -n` emits exactly the
168 writes, and splicing them into the replay reproduces the cold-boot-tested file **byte for byte**.

## Known issues

- **Dataplane ping loss degrades over minutes** — successive 10-ping rounds gave 0%, 0%, 60%, 60%
  while management stayed at 0%. This is **pre-existing and not caused by this change**: a cold boot
  of the unmodified replay degrades identically (0/0/70/60/10/70/50/60/90/80%). It also corrects an
  earlier note of ours claiming a sustained 10/10, 0% loss — that was a fresh-boot snapshot. Prime
  suspect is the portd DMA ring rather than the ASIC.
- **Et2 (10GBASE-CR copper) is intermittent** across boots, with or without this change. It came up
  on this boot and did not on the two before it.
- The dataplane does not auto-start; run `edgenos-up.sh` after boot.
- `init-m1` rewrites `/mnt/flash/boot-config` back to EOS on every boot — a deliberate one-shot
  safety net, so each EdgeNOS boot must be armed again.

## Still operator-supplied (not distributable)

| file | what it is |
|---|---|
| `/mnt/flash/fwd4.txt` | the register replay — **355,480 of 389,809 writes still EOS's** |
| `/mnt/flash/ucode_l2.raw`, `ucode_tail.raw` | FM6000 microcode |

The Si5338 clock map was eliminated in alpha4 and remains gone. Without the two files above the
image still boots to a shell with management SSH; the dataplane stays down and says so.

Progress toward a self-contained image is tracked in `docs/SELF-CONTAINED-PLAN.md`. Next by size is
the CM block (46,110 writes).

## Install

```sh
copy to /mnt/flash, then
echo SWI=flash:/edgenos-7150-0.3.0-alpha5.swi > /mnt/flash/boot-config && reboot
```

Recovery: serial console → Ctrl-C at boot → `Aboot#` → rewrite `boot-config`.
