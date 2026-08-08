# EdgeNOS 7150 — 0.3.0-alpha7

**The FM6000 microcode files are no longer needed. One operator-supplied file left.**

Arista DCS-7150S-52 · Intel FM6000 "Alta" · x86_64 · Aboot

## What changed

`ucode_l2.raw` and `ucode_tail.raw` — 710 KB of FM6000 microcode extracted from a licensed EOS —
are gone. Every one of the 39,415 registers they write is also written by the register replay, so
the separate load was redundant.

It only became redundant once a bug in our own generators was fixed, and that is worth stating
plainly because it had been hiding in plain sight:

```
pure replay,             no microcode load  ->  works
generators at loop end,  no microcode load  ->  routes=2, et1 rx=0, ping 100%
generators placed EARLY, no microcode load  ->  works
```

PARSER, L2AR, MOD and MAPPER are written *early* by the replay, before the port bring-up depends on
them. Our generators were hoisting them to the end of the loop — too late — and the defect was
invisible because the microcode load was separately writing those same registers early. The
generators had been leaning on the very file they were meant to replace, and would have shipped
that way. `gen_list_early` places them correctly.

`UCODE=1` restores the old load if you want to compare.

## Verified

Three cold boots with the microcode files **physically moved off flash**:

| run | Et1 | Et2 | routes | rx | ping % (5 rounds x 10) |
|---:|---|---|---:|---:|---|
| 1 | `0xec0` | `0x8c0` | 35 | 133 | 0, 10, 0, 0, 0 |
| 2 | `0xec0` | `0x8c0` | 35 | 141 | 0, 0, 0, 0, 0 |
| 3 | `0xec0` | `0x815` | 35 | 129 | 0, 0, 0, 0, 0 |

14 of 15 ping rounds at zero loss, OSPF stable at 35 routes on every boot. Run 3 had Et2 down and
was unaffected, which is consistent with copper being irrelevant to this path.

An earlier single run of this configuration showed 100% loss; three subsequent boots did not
reproduce it. Recorded rather than omitted — see `docs/measurements/`.

## Where the replay stands

**79.7% of EOS's trace is no longer read at boot** (74.5% if you keep the SPICO firmware for
copper). 11 generators produce the table state for SAF, CM, FFU, L2L, L2AR, PARSER, MOD, L3AR,
HASH, MAPPER and L2F+LBS; each block's multi-write *control* registers stay in the replay with
their sequences intact.

What remains is mostly sequence rather than state — L2AR's 24,504 control writes and EPL's 22,051 —
so the collapse technique is at its limit. Those need the bring-up procedures understood, not
reorganised.

## Known issues

- **Et2 (10GBASE-CR copper) is intermittent** across boots. Roughly half.
- **Boot reliability is about 3 in 4.** Do not treat a single cold boot as proof.
- **EPL cannot be generated** — a bring-up procedure, not table state. `EPLGEN=1` to retry.
- The dataplane does not auto-start; run `edgenos-up.sh` after boot. It refuses to run twice.
- `init-m1` rewrites `boot-config` back to EOS every boot; `/mnt/flash/edgenos-sticky` holds a boot
  budget for automated testing.

## Still operator-supplied

| file | what it is |
|---|---|
| `/mnt/flash/fwd4.txt` | the register replay — 79,150 EOS-derived lines still read, of 389,809 |

That file still carries Intel's SerDes SPICO firmware (90,006 writes) verbatim. Strip it for a
fibre-only build: Et1 10GBASE-SR trains and forwards without it, Et2 copper does not.

Licensing: `docs/PROVENANCE.md`. Method: `docs/SELF-CONTAINED-PLAN.md`.

## Install

```sh
copy to /mnt/flash, then
echo SWI=flash:/edgenos-7150-0.3.0-alpha7.swi > /mnt/flash/boot-config && reboot
```

Recovery: serial console → Ctrl-C at boot → `Aboot#` → rewrite `boot-config`.
