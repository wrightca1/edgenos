# Shipping a working NOS with no vendor code — what is actually required

The goal is a switch that boots, links and forwards using only code we wrote, with
no vendor file on flash and no vendor values in our source. This is the measured
distance to that, and the gates in the way.

## Where the vendor content actually is

Three separate places, and only one of them has moved this session.

### 1. On the operator's flash — 6,727 writes (121 KB)

Down from 283,339 (5.1 MB). The box boots and forwards with no `fwd4.txt`,
`fwd5.txt` or `bringup.txt`, at 92.3% of the bring-up stream produced by our own
generators, Et2 5/5. What is left is **the sequence**, not padding: only 8 of those
writes are back-to-back duplicates, so nothing is removable by deduplication
(`docs/RESIDUAL-CANDIDATES.md`).

Plus `ucode_l2.raw` (546 KB) and `ucode_tail.raw` (165 KB), which are readable
tables rather than firmware, and the 12 KB SPICO blob, which only copper DAC needs.

### 2. In our source, as transcription — 52,702 pairs in 3 files

    fm6000_l2arseq.c   29,110    L2 action resolution
    fm6000_eplseq.c    22,051    port/SerDes link training
    fm6000_mgmt2pre.c   1,541    a 129-iteration CRM command loop

### 3. In our source, as the vendor's microcode verbatim — 8,570 pairs in 19 files

This is the number that gates redistribution, and **it has not moved at all.**

    fm6000_l2arseq.c      RELOCATED   2,747
    fm6000_modinit.c      TABLE       2,604
    fm6000_l2arinit.c     TABLE       1,128
    fm6000_l3arslice2.c   AUTHORED      342
    fm6000_l3artables.c   AUTHORED      301
    fm6000_statsarinit.c  TABLE         296
    fm6000_l3arslice3.c   AUTHORED      257
    fm6000_mapperinit.c   TABLE         180
    fm6000_l3arslice4.c   AUTHORED      166
    fm6000_smalltables.c  AUTHORED      157
    + 9 smaller files                   592

## ⚠ "AUTHORED" does not mean "no vendor bytes"

Seven files graded **AUTHORED** carry 1,298 microcode pairs between them. That
grade means the *structure* was recovered and the rules follow from stated intent —
it does not mean the *values* were derived. `l3arslice1` authors six rules where
EOS ships thirty-two, and the profile-table contents those rules select are still
the vendor's numbers.

Only four authored files carry none: `fm6000_erl.c`, `fm6000_cmrest.c`,
`fm6000_esched.c`, `fm6000_parserfields.c`.

**So the provenance audit and the microcode overlap measure different things, and
the goal needs both at zero.** A file can be honestly authored and still be
unshippable.

## What clearing each block requires

| block | pairs | what is needed | blocked on |
|---|---:|---|---|
| `l2arseq` | 29,110 | author 404 L2AR rules from intent | **the key format** |
| `eplseq` | 22,051 | drive link training ourselves | the training protocol |
| `modinit` | 3,855 | author the rewrite engine's tables | what each MOD command means |
| `l2arinit` | 4,606 | same block as `l2arseq` | same |
| FFU (residual) | 3,545 | author 127 classifier rules | **the key format** |
| `mgmt2pre` | 1,541 | drive the CRM engine ourselves | what the 129 operations do |
| `l3arslice*` | 1,038 | derive the profile values | what each profile means |

### The two key formats are the main gate

**L2AR.** 404 rules, 8 slices × 64, a 384-bit ternary key. The action half is fully
named by the SDK. The key half is **not packet fields** — all 404 rules were
searched for this switch's router MAC, port GLORTs and EtherTypes and matched
none — it is lookup metadata, concentrated in the low 12 bytes. Correlating key
bits against actions gives nothing: no bit predicts an action, and the 61 rules that
trap to CPU test scattered bits with no pattern. Naming those 96 bits needs the L2
lookup's outputs and the parser's 38 `SetFlags` traced, and only about a third of
those flags are named so far (bits 8/9 an L3 type code, 15 VLAN-tag-present, 26 ARP,
34–36 PAUSE).

**FFU.** 127 rules. `FFU_SLICE_SCENARIO_CFG` selects key sources with
`ByteMux_0..3`, and of the 16 selectors used, three fall outside the r1..r42 range
the parser writes — so the selector space is not just parser registers and the
mapping is unknown.

Without those two formats, 404 + 127 rules can be read as bit patterns but not as
policy, and authoring them would be transcription with a loop around it.

## The honest assessment

The runtime dependency is 40× smaller and the box demonstrably works without the
vendor replay. The repository is not shippable and nothing this session changed
that.

What remains is a **research programme, not a cleanup**: two undocumented key
formats, a link-training protocol, and an indirect-engine command set, each of which
gates a block that cannot be honestly authored until it is understood. The parser
work in `docs/PARSER-PROGRAM.md` shows the shape of it — a state machine fully
decoded and walked — and that took a session on its own for one block.

The pattern that has worked, in order of leverage:

1. **Read the SDK for names.** `sdk_fieldmap.py` named every field of the L2AR
   action word and the parser instruction; that is what made both readable.
2. **Read the write sequence, not the bytes.** Convergence means author the end
   state (`ESCHED` 444→12, parser seed 970→110); oscillation means an engine
   running (`L2L_SWEEPER` 1,280→0).
3. **Anchor on values you know.** The parser fell to it — the port GLORTs and the
   IEEE multicast MACs were the fixed points that made everything else decode.
4. **Check what `fullreplay` does with an address before authoring it.** SBus is a
   handshake, not three writes, and that cost 5 boots of a dead port to learn.
