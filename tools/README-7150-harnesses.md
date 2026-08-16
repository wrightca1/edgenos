# 7150 hardware harnesses (2026-08-15/16)

Scripts that drive the 7150 over the network. They assume `sw.sh` (EOS CLI),
`eg.sh` (EdgeNOS shell) and `p5.sh` (the AS5610 peer) exist alongside them —
those hold credentials and are not in the repo.

## The rig

```
7150 Et1  10.101.101.26/29  <-->  AS5610 swp6  10.101.101.25/29
7150 Et2  10.101.101.34/29  <-->  AS5610 swp7  10.101.101.33/29
```

Two different /29s on one peer, so a `/32` route on the peer forces a hairpin —
in Et2, routed, out Et1 — and `tcpdump` on swp6 captures **what the switch
emitted**. That is the observation A4 and B1 were blocked on. The peer is
`10.1.1.238` and has `tcpdump 4.99.4`.

## What each script is for

| script | question it answers |
|---|---|
| `et2-baserate.sh` | how often does Et2 link? (measured: 5 of 10 identical boots) |
| `hunt-et2.sh` | reboot until Et2 is up, archiving every boot's settle trace |
| `cmp-traces.sh` | diff a good boot's trace against a dark one, bit 10 masked |
| `rxdump-test.sh` | are punted frames F64-tagged? (needs portd stopped) |
| `et2-demux-test.sh` | does per-port RX demux work? guards against a false negative |
| `transit-test.sh` | does traffic transit, and what does the switch emit? |
| `a4-leaveout.sh` | clear ONE MOD step's Valid bit and read the wire |
| `a4-slice-sweep.sh` | which MOD slice performs a given edit (disable a whole slice) |

## Rules these scripts encode, each learned the hard way

- **A single boot measures nothing.** Et2 links on ~half of identical boots, so
  any "X makes it work" claim needs n per arm and a reported count. At that rate
  separating two arms needs ~32 boots each, so **design experiments to be
  falsified** — a hypothesis predicting a *zero* costs 5 boots, one predicting a
  shift costs a day.
- **A TAP's `carrier` is not a link signal.** portd's TAP reports carrier=1 as
  soon as it is up, whatever the lane is doing. Read `PORT_STATUS` at `0xe4000`.
- **`PORTD_DEBUG=N` dumps N frames.** With N=1 the one frame you see may be a
  runt, and generalising from it is how the punt-tag diagnosis went wrong.
- **Verify a save before perturbing, and health-check between tests.** The first
  `a4-slice-sweep.sh` guarded its 32-value capture with "non-empty", wrote zeros
  over a live table, and then reported five more slices as broken when it was
  the same wedge.
- **Restore is not recovery.** A restored table does not un-wedge a dataplane in
  flight. Budget a reboot.
- **`MOD_COMMAND_RAM` takes writes after boot** (unlike `PARSER_INIT_FIELDS`), so
  these perturbations are real until the next boot — which is also why a reboot
  always fixes them, since FULLSEQ reprograms the table.
