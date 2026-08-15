# FM6000 SerDes SPICO firmware — reverse engineering notes

**Status 2026-08-06.** The *delivery protocol* is fully decoded and we can reconstruct the firmware
image from any register trace. The *instruction set* is not yet recovered.

Tool: `asic/fm6000/tools/spico_extract.py` (ours; bundles no firmware).

---

## 1. What SPICO is

A small microcontroller embedded in the FM6000 SerDes. Its firmware runs the SerDes housekeeping —
notably RX equaliser adaptation. It is Intel's code, shipped inside `libFocalpointSDK.so`, and it is
the one blob we cannot redistribute. It is **not required** for a 10GBASE-SR link (proven by
bisect), which is why it is not in the runtime path today.

## 2. Delivery protocol — fully decoded

The IMEM upload is a plain SBus transaction sequence on receiver `0xFD`. Per word:

```
reg 0x04 <- addr[15:8]
reg 0x05 <- addr[7:0]
reg 0x07 <- data[7:0]
reg 0x06 <- data[9:8] | 0xC     bit3 = IMEM write enable, bit2 = strobe
reg 0x06 <- data[9:8] | 0x8     strobe released
```

> **The IMEM word is 10 bits wide, not 16.**

Register `0x06` carries only `data[9:8]` in bits `[1:0]`. Confirmed empirically: across the entire
upload, every value written to `0x06` lies in `{0,8,9,a,b,c,d,e,f}` — the data field never exceeds
`0x3`. The stock image is **6000 words at 0x0000–0x176f**, contiguous, every word ≤ `0x3ff`.

Surrounding sequence: `0xFD0C <- 3` (reset), `<- 1` (enable), `0xFD06 <- 8` (IMEM write enable),
upload, `0xFD06 <- 0`, `0xFD0C <- 8` (run), then `0xF004` (a *direct* JSS CSR, not an SBus
transaction) Reset=0/Enable=1. Liveness is interrupt 2 → 1; interrupt 4 is a CRC self-check.

**Verified:** reconstructing the image from EOS's own trace yields a file **byte-identical** to the
SDK blob (md5 `0ba4fbcc057d052801c484288849d1d8`). That validates both the decode and our loader.

## 3. Image characteristics

| property | value |
|---|---|
| size | 6000 words × 10 bits |
| distinct word values | 461 of 1024 |
| entropy | 7.10 bits/word |
| most common | `0x3c7` (5.6%), `0x044` (5.5%), `0x000` (5.2%), `0x347` (4.5%) |

**Not fixed-width-multi-word.** Per-position entropy at strides 1–5 is flat (6.7–7.1), so
instructions are not simply k×10-bit groups.

**Evidence for variable length.** The image opens with an obvious unrolled loop:

```
0000: 146 000 004 000 044 047 000 347
0008: 037 036 362 036 001 009 005 04e
0010: 000 3fa 362 036 002 009 003 04f
0018: 362 036 003 009 003 04e 362 036
0020: 004 009 003 04e 362 036 005 009
```

`362 036 <n> 009 …` repeats with `n` incrementing 001,002,003,004,005 — an immediate operand in the
instruction stream. Successor-entropy confirms mixed behaviour: `0x343` is almost always followed by
`0x005` (H=1.08, a fixed idiom), `0x3c0`→`0x001` (H=1.95), while `0x347` has H=6.29 (operand
follows).

**Open:** opcode field boundaries, register file, the interrupt-handler dispatch table, and which
routine performs RX adaptation.

## 4. Practical note — RE is not on the critical path

Two separate goals, very different costs:

- **Make copper links work** — does *not* need this RE. The firmware can simply be loaded, exactly
  like the FM6000 microcode, on the established "bring-your-own from a licensed EOS" model. **And
  it turns out not to help anyway** — see below.
- **Replace SPICO so the platform is fully distributable** — needs full ISA recovery plus writing a
  SerDes control program. Large project, and the last blob after the microcode generator.

**Tested and refuted (2026-08-06):** loading SPICO cold does *not* fix Et2. The load succeeds —
alive check passes, CRC self-check OK, SPICO running — and Et2 stays at `PORT_STATUS=0x815`,
`pcsRx=0`. So SPICO is not what Et2 is missing, and this further strengthens the case that SPICO is
not required at all on this platform.

## 5. Next steps

1. Recover the ISA: identify the opcode split, then hand-disassemble the entry sequence and the
   interrupt dispatch (interrupts 2 and 4 have known semantics — alive and CRC — which gives two
   anchors into the code).
2. Look for a jump/dispatch table near the image start; `0x146` at address 0 is a plausible entry
   vector.
3. Only then consider writing our own SerDes control program.

Priority is low relative to the microcode generator: SPICO is not required for the working datapath,
and it does not fix copper.

## 2026-08-15: ⚠ "SPICO IS REQUIRED for 10GBASE-CR" does not survive the base-rate measurement

`fm6000-fullseq.sh` carries this at the top of the file, and checklist C1/C2 rest on it:

```
*** SPICO IS REQUIRED for 10GBASE-CR (DAC/copper). ***
An earlier bisect concluded it was unnecessary -- that was WRONG, because it
only ever checked Et1 (10GBASE-SR fibre). With the firmware stripped:
  Et1 (SR)  links fine   -> PORT_STATUS=0x8c0, pcsRx=1
  Et2 (CR)  does NOT     -> PORT_STATUS=0x815, pcsRx=0
With fwd4.txt unmodified, BOTH link at 0x8c0/pcsRx=1.
```

**The last line is measurably false.** Ten controlled boots, `fwd4.txt` unmodified in the relevant
sense — and it demonstrably contains the firmware, 30,479 SBus writes to the SPICO broadcast device
`0xfd` of 62,482 total — gave **Et2 up on about half of them**, sitting at exactly `0x0815` on the
rest. See `PORT3-BRINGUP.md` for the run.

So the inference collapses:

- "SPICO stripped → Et2 reads `0x815`" is **one boot per condition** on a port that reads `0x815`
  roughly 44% of the time *with* the firmware present.
- That observation is therefore consistent with an unlucky boot and establishes nothing.
- The earlier bisect that concluded SPICO was unnecessary was overturned on this evidence. **The
  overturning is what is now in doubt** — not necessarily the original conclusion.

⚠ **This is not a claim that SPICO is unnecessary for copper.** It is a claim that the evidence for
"required" does not survive contact with the base rate. Checklist C1 already half-noticed this
("Et2 is intermittent with *and* without SPICO") but read the intermittency as a property of copper
rather than of the boot.

### The experiment that settles it, and it is cheap

The general case is hopeless — separating two rates near 50% needs ~31 boots per arm. **But this
hypothesis predicts an extreme, not a shift**: if SPICO is genuinely required for CR, Et2 must come
up **never** without it. Testing a predicted zero is far cheaper than testing a difference:

```
P(0 up in k boots | the measured ~56% rate) = 0.44^k
  k=3  -> 0.085      not enough
  k=5  -> 0.016      significant
  k=7  -> 0.003      comfortable
```

**Five boots with the SPICO writes stripped, Et2 sampled for three minutes each.** One single
success refutes "required" outright. Five failures support it at p≈0.016. That is about an hour on
`tools/et2-baserate.sh` with a stripped replay, against the 6.5 hours per arm a general comparison
would need.

⚠ Run it against the **same** replay with only the `0xfd` writes removed — not against an older
stripped image — or the arm changes for other reasons too.

### Why this matters to scope

C2 is *"if copper needs it — our own equaliser loop over SBus. Large, unscoped."* If SPICO turns out
not to be required for CR, **C2 disappears** and "zero proprietary files" stops being a fibre-only
claim. That is the single largest piece of remaining work riding on a conclusion drawn from one boot.
