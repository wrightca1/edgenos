# EdgeNOS 7150 — 0.3.0-alpha8

**91.5% of EOS's register replay is no longer read at boot. One operator file left.**

Arista DCS-7150S-52 · Intel FM6000 "Alta" · x86_64 · Aboot

## What changed since alpha7

Three more blocks became ours, taking the total from 74.5% to **91.5%** (with the SPICO firmware
dropped for a fibre-only build; **68.4%** keeping it for copper).

- **EPL, 22,051 writes.** The SerDes/PCS bring-up. It cannot be *collapsed* — writing its end state
  discards the intermediate states that drive the hardware, and the chip wedges — but it can be
  *relocated*. Emitting the exact recorded sequence works.
- **L2AR, 25,426 of 29,110 writes.** Two things in one address range: a bulk pre-loop table load,
  and small in-loop bursts interleaved with the port bring-up. Lifting the whole block killed
  unicast forwarding; **splitting at the loop boundary** lifts the bulk and leaves the interleaved
  part exactly where the trace put it.
- **Eight small control blocks, 3,027 registers.** EACL and LAG are 100% write-once, GLORT and
  STATS_AR ~97%; MGMT2, MONITOR, CMM and SWEEPER are mixed, so only their write-once parts are
  lifted and their control writes stay in sequence.

```
20 generators emit             119,894 writes
final replay file              243,152 lines
  still EOS-derived             33,252   (SPICO dropped)
```

## Four techniques, one per kind of block

| kind | example | move |
|---|---|---|
| table the pipeline reads | SAF, CM, L2L | collapse to end state |
| table + control strobes | FFU (`0x3f0000` pulses 59×) | collapse the table, leave the strobes |
| hardware-driving sequence | EPL, L2F/LBS sweep | relocate the sequence, never collapse |
| bulk load + interleaved control | L2AR | split on the loop boundary |

Placement is per-block and had to be cold-boot tested every time. Nothing here generalised reliably
from one block to the next.

## Verified — including a failure

Six cold boots, microcode files **physically absent** from flash:

| | Et1 | Et2 | routes | rx | ping % (5 rounds × 10) |
|---:|---|---|---:|---:|---|
| 1 | `0xec0` | `0x815` | 35 | 126 | 0, 0, 0, 0, 0 |
| 2 | `0xec0` | `0x8c0` | 35 | 133 | 0, 0, 0, 0, 0 |
| 3 | `0xec0` | `0x8c0` | 35 | 135 | 0, 0, 0, 0, 0 |
| 4 | `0xec0` | `0x815` | 35 | 125 | 0, 0, 0, 0, 0 |
| 5 | `0xec0` | `0x8c0` | 35 | **82** | **100, 100, 100, 100, 100** |
| 6 | `0xec0` | `0x8c0` | 35 | 129 | 0, 0, 0, 0, 0 |

**Five of six boots are clean — 25 of 25 ping rounds at zero loss. One boot forwarded nothing.**
OSPF reached 35 routes on that boot too, and its RX counter was 82 against 125–135 on the good
ones, so the punt path came up partially. The cause is not known. It is not a regression from
alpha7 (whose first test run failed the same way before three clean boots), but it is a real
one-in-six chance of a dead dataplane and you should expect it.

## Known issues

- **One boot in six brings up OSPF but forwards nothing.** See above. Reboot.
- **Et2 (10GBASE-CR copper) is intermittent**, roughly half of boots, unrelated to the generators.
- **Boot reliability is about 3 in 4** — some boots do not come up at all.
- **EPL end-state generation wedges the chip** (`EPLGEN=1`), **L2AR whole-sequence relocation kills
  unicast** (`L2ARSEQ=1`), **the L2F sweep reproduction breaks forwarding** (`SWEEPGEN=1`). All
  three default off and are kept as records of what does not work.
- The dataplane does not auto-start; run `edgenos-up.sh` once. It refuses to run twice.

## Still operator-supplied

| file | what it is |
|---|---|
| `/mnt/flash/fwd4.txt` | the register replay — 33,252 EOS-derived lines still read, of 389,809 |

It still carries Intel's SPICO SerDes firmware (90,006 writes) verbatim. Strip it for a fibre-only
build: Et1 10GBASE-SR trains and forwards without it; Et2 copper does not.

`GENBLK=0` reverts every generator; `UCODE=1` restores the old microcode load.

Method: `docs/SELF-CONTAINED-PLAN.md`. Licensing: `docs/PROVENANCE.md`.

## Install

```sh
copy to /mnt/flash, then
echo SWI=flash:/edgenos-7150-0.3.0-alpha8.swi > /mnt/flash/boot-config && reboot
```

Recovery: serial console → Ctrl-C at boot → `Aboot#` → rewrite `boot-config`.
