# EdgeNOS 7150 — 0.3.0-alpha6

**The dataplane actually works now, and three quarters of EOS's replay is no longer read at boot.**

Arista DCS-7150S-52 · Intel FM6000 "Alta" · x86_64 · Aboot

## The important fix: RX stopped dying

alpha5 and everything before it had a dataplane that died after about 157 received packets —
silently, with no error and no counter moving. OSPF would form an adjacency, learn 35 routes, then
lose them; ping degraded to 100% loss. It had been recorded as a pre-existing platform defect.

It was ours. `rx_drain()` in portd recycled a descriptor by writing the status byte alone, but the
hardware overwrites the descriptor's LEN field with the *received* length when it fills a slot. Each
recycle therefore handed the buffer back sized to the last frame, shrinking with every short packet
until the ring could accept nothing.

```
before:  RX 157 packets, then delta 0 over 30s while hellos arrived every 10s
         TX still fine. Routes 35 -> 2. Ping 50% -> 95% -> 100%.

after:   24 consecutive rounds, 240 pings, 0% loss on every round
         RX 67 -> 376, straight past the old death point. Routes held at 35.
```

A second, self-inflicted trap went with it: `edgenos-up.sh` could be run twice, leaving two portd
instances fighting over the same rings (`ip link del et1` removes the netdev but not the process).
It now refuses unless `FORCE=1`.

**If you are running alpha5, upgrade.** Its dataplane stops forwarding within a few minutes.

## 74.5% of EOS's replay is no longer read at boot

Measured from the artifacts — lines of EOS's trace the switch no longer reads:

```
original replay, all EOS-derived      389,809
  after the 11 generators             243,152 lines
    of which OUR generators emit       73,996
    of which still EOS-derived        169,156
EOS lines eliminated                  220,653   74.5%
```

Drop the SPICO firmware as well (fibre-only, see below) and it is **310,659 of 389,809 — 79.7%.**

| block | EOS writes | our tool | emits |
|---|---:|---|---:|
| L2F + LBS | 74,674 | `fm6000_l2finit` | 637 |
| CM | 47,742 | `fm6000_cminit` | 8,180 |
| SAF | 34,668 | `fm6000_safinit` | 168 |
| L2AR | 29,110 | `fm6000_l2arinit` | 4,606 |
| L2L | 24,620 | `fm6000_l2linit` | 24,568 |
| PARSER | 22,246 | `fm6000_parserinit` | 16,960 |
| FFU | 14,549 | `fm6000_ffuinit` | 8,680 |
| MAPPER | 6,644 | `fm6000_mapperinit` | 361 |
| MOD | 4,834 | `fm6000_modinit` | 3,855 |
| L3AR | 4,489 | `fm6000_l3arinit` | 3,933 |
| HASH | 2,354 | `fm6000_hashinit` | 2,048 |

Each generator emits its block's table state; the multi-write **control** registers stay in the
replay with their sequences intact, because collapsing a strobe breaks it — the FFU's commit pulse
at `0x3f0000` fires 59 times, and performing it once means the CPU-punt traps never apply.

`GENBLK=0` reverts to replaying EOS's writes for every block.

## Intel's SerDes firmware is not needed for fibre

The SPICO image is 90,006 writes and the clearest piece of non-distributable code in the replay.
Stripped entirely and cold-booted with all 11 generators active:

```
Et1 (10GBASE-SR)  0xcc0, pcsRx=1     up
Et2 (10GBASE-CR)  0x815               DOWN -- copper needs the firmware
OSPF 35 routes, 13 in silicon, ping 10 rounds: 0% loss on 9, 10% on one
```

So a **fibre-only** deployment needs no Intel firmware at all. Copper does — that is a functional
limitation, not a licensing footnote. This image ships with SPICO retained; strip it yourself if you
run SR optics only.

## Verified on this image

Cold boot, stock operator replay, 11 generators active:

```
Et1 0x00000cc0   Et2 0x000008c0        both links up
OSPF             adjacency, 35 routes, still 35 at end of run
hardware FIB     13 routes programmed into silicon
ping             10 rounds x 10 packets, 0% loss on every round
et1 RX           199 packets and climbing
```

An A/B soak against the stock replay (same image, `GENBLK=0`) is indistinguishable: both arms
0% loss on all six rounds. See `docs/measurements/`.

## Known issues

- **Et2 (10GBASE-CR copper) is intermittent** across boots, with or without the generators. It came
  up on this one; it links in roughly half of cold boots.
- **Boot reliability is about 3 in 4.** Across two soaks, 4 of 16 runs failed to boot or to report.
  The switch always recovered, but do not treat a single cold boot as proof of anything.
- **EPL cannot be generated.** It is the SerDes/PCS bring-up procedure, not table state, and
  collapsing it to an end state wedges the chip. Gated off behind `EPLGEN=1`.
- The dataplane does not auto-start; run `edgenos-up.sh` after boot.
- `init-m1` rewrites `boot-config` back to EOS on every boot — a deliberate one-shot safety net, so
  each EdgeNOS boot must be armed. `/mnt/flash/edgenos-sticky` holds a boot budget for automated
  testing.

## Still operator-supplied (not distributable)

| file | what it is |
|---|---|
| `/mnt/flash/fwd4.txt` | the register replay — the boot still reads 169,156 EOS-derived lines of it (down from 389,809) |
| `/mnt/flash/ucode_l2.raw`, `ucode_tail.raw` | FM6000 microcode |

⚠ PARSER, MOD and L2AR are microcode blocks. Generating them shrinks `fwd4.txt` but does **not**
remove the `ucode_*.raw` dependency, which is still loaded separately. The remaining replay also
still carries Intel's SerDes SPICO firmware (90,006 writes) verbatim.

Progress and method: `docs/SELF-CONTAINED-PLAN.md`. Licensing: `docs/PROVENANCE.md`.

## Install

```sh
copy to /mnt/flash, then
echo SWI=flash:/edgenos-7150-0.3.0-alpha6.swi > /mnt/flash/boot-config && reboot
```

Recovery: serial console → Ctrl-C at boot → `Aboot#` → rewrite `boot-config`.
