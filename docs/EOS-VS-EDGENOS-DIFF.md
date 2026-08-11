# Diffing the live chip: EOS vs EdgeNOS

**2026-08-11.** After a long unsuccessful hunt for why EdgeNOS stopped forwarding, the productive
move was to stop reasoning from the replay and **compare the running chip under EOS (which
forwards) against the running chip under EdgeNOS (which does not)**. EOS is a known-good
configuration of this exact hardware, so whatever differs is the candidate set — and nothing else
is.

This is the same differential-oracle method that settled the parser and L3AR work, applied to
whole register regions instead of single tables.

## Method

`fmdump <start_hex> <count>` over two regions, taken under each OS and diffed offline:

```
/mnt/flash/fmdump 110000 65536     # CM  0x110000-0x11ffff
/mnt/flash/fmdump   e0000 16384    # EPL 0x0e0000-0x0e3fff
```

81,920 words each. Staging `fmdump` on `/mnt/flash` matters — the M1 rootfs is in RAM, so anything
in `/tmp` is gone after the reboot between the two captures.

⚠ **The EdgeNOS capture is not a pristine boot.** It was taken after I had re-run the full replay
*and* `fm6000_cmfill`, so some EdgeNOS-side values are mine, not the boot's — `0x112800` in
particular reads the raw fill `0xffffffff` where the replay had correctly written `0x0013003a`,
because my `cmfill` run overwrote it. Treat CM watermark differences with that in mind; the EPL
differences are unaffected.

## Result 1 — it killed my leading hypothesis immediately

I had spent considerable effort on CM `0x114000`, which the replay writes (`0x000000d6`) and the
chip reads back as zero, and which `fm6000_cmfill` fails to write.

```
0x114000: EOS=0x00000000   EdgeNOS=0x00000000
0x113810: EOS=0x00000000   EdgeNOS=0x00000000
```

**Zero on EOS too.** That region is zero on a system that forwards perfectly, so it never mattered.
An hour of "the replay can't program this memory" was real but irrelevant. One diff against a
working reference would have said so at the start.

## Result 2 — 1,746 words of uninitialised memory

Classifying all 4,373 differing words:

| signature | count | meaning |
|---|---:|---|
| EOS zero, EdgeNOS non-zero | 1,746 | **uninitialised on EdgeNOS** — this is the "no CRM fill" gap |
| EOS non-zero, EdgeNOS zero | 12 | mostly FIFO *status*, see below |
| both non-zero, differing | 2,615 | real configuration differences |

The uninitialised runs hold random values (`0xdbd15510`, `0x2761cffa`, `0x5e8cccfd`) where EOS
holds zeros — `0x111780`-`0x111917`, `0x112942`-`0x112afb` (442 words), `0x112b42`-`0x112b8b`.
That is exactly what `COLD90 PROBE MODE ... no CRM fill` means, now measured rather than inferred.

⚠ **Do not chase the 12 "missing" words.** Seven are `0xe3b08`-`0xe3b0e`, which the header names
`EPL_TX_FIFO_B/C/D_PTR_STATUS` and `EPL_RX_FIFO_*_PTR_STATUS` — read-only status. EOS's are
non-zero because traffic was flowing and EdgeNOS's are zero because it was not. That is a symptom
of the fault, not its cause, and it looks like a smoking gun until you read the register names.

## Result 3 — the EPL14 configuration deltas

EPL14 is Et1. Stripping status registers, the genuine configuration differences are small and
specific:

| register | EOS | EdgeNOS | delta |
|---|---|---|---|
| `0xe3b01` `EPL_CFG_A` | `0x7e1d7899` | `0x7e0d7899` | bit 20 |
| `0xe3b02` `EPL_CFG_B` | `0x00090033` | `0x00090003` | bits 4,5 |
| `0xe3b00` `EPL_IP` | `0x00000000` | `0x00000010` | bit 4 |
| `0xe3804` | `0x00002a00` | `0x00003b87` | lane-0 block |
| `0xe3821` | `0x00402003` | `0xffc0c79a` | lane-0 block |

Three bits in `EPL_CFG_A`/`EPL_CFG_B` are the most promising leads for the egress fault, and they
are cheap to test: write EOS's values under EdgeNOS and see whether Et1 starts forwarding.

## Result 4 — EOS has your test host's port up, and that is the port-3 reference

```
Et1  to-Edgecore5610-port6      connected  routed  10G  10GBASE-SR
Et2  to-Edgecore5610-port7-DAC  connected  routed  10G  10GBASE-CR
Et3                             connected  vlan 1  10G  10GBASE-SR
```

`0xe3880` — EPL14 **lane 1**, which `fm6000_serdes_ports.h` maps to front-panel port 3 — reads
`0x00000ac0` under EOS (RxLinkUp, HeartbeatOk, Transmitting, SerXmit) against `0x00000015` (three
link faults) under EdgeNOS.

So the port-3 bring-up we were about to attempt from first principles does not need to be derived
at all: **EOS's own lane-1 configuration is now captured**, and the whole lane-1 block
`0xe3880`-`0xe38ff` is in the dump. That is the reference for A4/B1's transit traffic, and it also
gives a second working example to check the lane-0 deltas against.

## What this changes

The next steps are concrete rather than speculative:

1. Boot EdgeNOS, write EOS's `EPL_CFG_A`/`EPL_CFG_B`/`EPL_IP` values for EPL14, retest Et1.
2. If that restores forwarding, apply the same to lane 1 and bring up port 3 from the captured
   config rather than from `fm6000_serdes_enable` guesswork.
3. Fill the uninitialised CM regions to zero to close the CRM gap — cheap, and now known to be a
   real difference rather than a suspicion.

**Provenance.** The dumps are register *state* read from hardware at runtime and are kept outside
the tree, like `fwd4.txt`. What is recorded here are the addresses, the register names from the
header, and the deltas — facts about the chip, not a copy of anyone's configuration.
