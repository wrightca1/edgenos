# Parser conventions: what the rest of the chip expects

`gen_parser.py` can emit valid CAM+RAM words — verified by round-tripping all 2,117 of EOS's
entries bit-identically. That is not enough to author a parser. A program also has to agree with
L2AR, L3AR and FFU about *conventions*: what the inter-slice state means, and which FIELDS channel
carries which extracted header. Those choices are implicit in EOS's program, and
`fm6000_parserinit.c` currently satisfies them only by being a copy of it.

This is what has been derived so far. Everything here is measured from the decode.

## ★ The 64-bit key splits state from frame data

Datasheet 5.5.1 says each slice keys on the next 4-byte frame word plus the previous slice's
32-bit state. The split is:

```
key[63:32]  STATE       4 x STATE8 bytes, transformed by StateOp0..3
key[31:0]   FRAME_DATA  the slice's 4-byte parsing window
```

Confirmed on slice 4 entry 3, the IPv4 rule: `care = 0x00ff00ff_ffffffff` — the frame half fully
cared with `0x00010800` (EtherType 0x0800 in the low halfword), the state half partially cared
at `0x3a`.

Care masks bear the split out. On the **state** half the dominant mask is `0x000000ff` (586 of
2,117 entries), i.e. most rules test only STATE8[0] — exactly the per-byte model Table 5-3
describes. On the **frame** half the dominant mask is `0x00000000` (1,265 entries): **most rules
do not look at frame data at all** and are pure state transitions.

## ★ The state machine, traced

`parser_decode.py --states` walks the machine from state 0 at slice 0, applying each rule's
StateOp0..3 to compute the next state. Transitions whose result depends on frame bytes the rule
does not pin (`StateOp` 2 and 3 read FRAME_DATA) are **dropped rather than guessed**, so a trace
reaches fewer states than exist — 26 states and 137 transitions, against 60–79 states per slice
in the raw match data. What it recovers is exact; it is simply partial.

### The four state bytes have distinct roles

| byte | rules constraining it | distinct values | reading |
|---|---:|---:|---|
| **STATE8[0]** | **2,114 (100%)** | 63 | the primary parse state — every single rule keys on it |
| STATE8[1] | 533 (25%) | 15 | auxiliary context |
| STATE8[2] | 679 (32%) | 12 | auxiliary context |
| STATE8[3] | 1,016 (48%) | 14 | auxiliary context / flags |

That every rule constrains `STATE8[0]` and only some constrain the rest is the structural fact a
generated program has to respect: byte 0 is the state variable, bytes 1–3 qualify it.

### Protocol transitions are visible

Tag stacking reads out directly, e.g.

```
slice3 0x00007f10 --VLAN C-tag--> slice4 0x0000f110 --VLAN C-tag--> slice5 0x0000f1ff
```

with `STATE8[1]` carrying the tag-depth context (`0x7f` → `0xf1` → …) while `STATE8[0]` stays
`0x10`.

### ★ What the STATE8[0] values mean

`--state-map` labels each state by what its rules extract: a state whose rules deposit the DMAC is
a state parsing the DMAC. Only rules pinning `STATE8[0]` exactly are counted, so attribution is
unambiguous. 55 of the 63 values are pinned exactly by at least one rule, and they read as a walk
through the packet:

| state | slices | extracts | reading |
|---:|---|---|---|
| `0x01` | 5–15 | L2_DMAC[47:32], [31:16] | first Ethernet word |
| `0x02` | 1–16 | L2_DMAC[15:0], L2_SMAC[47:32] | second Ethernet word |
| `0x11` | 15–20 | L2_TYPE (matches IPv6, FCoE) | EtherType position |
| `0x20` | 4–20 | L3_FLOW/L3_PRI, L3_LENGTH | IP header start |
| `0x30` | 4–20 | L3_FLOW/L3_PRI, L3_FLOW[15:0] | IPv6 flow label |
| `0x22` | 6–22 | L3_TTL / L3_PROT | IP TTL/protocol |
| `0x31` | 5–21 | L3_LENGTH, L3_TTL/L3_PROT | IP header body |
| `0x23` | 7–23 | L3_SIP/DIP[31:16], [15:0] | IP addresses |
| `0x40` | 9–27 | L4_SRC, L4_DST | L4 ports |
| `0x50` | 9–27 | L3_TTL/L3_PROT, L4_SRC | deepest L3/L4 |
| `0x3a` | 4–23 | — (matches **IPv4**) | IPv4 EtherType decision |

A state is not a single packet offset — most span many slices, because the same logical position is
reached at different depths depending on how many tags precede it. That is the whole point of an
unrolled parser, and it is why a generated program must emit the same rule at every slice where the
state is reachable, not once.

⚠ Not every state is labelled. Several high-traffic ones (`0x24` with 160 rules, `0x39`, `0x36`)
extract only into unmapped generic channels (`ch22`, `ch23`, `ch30`, `ch31`), which Table 5-5 does
not name. Those are FIELD16-class channels used for whatever the program wants; their meaning is a
choice EOS made, not a hardware fact.

## The state machine is legible

Distinct state values matched, per slice:

| slice | states | shape |
|---|---:|---|
| 0 | 1 | only `0x0` — the entry state |
| 1 | 8 | `0x2`–`0xb` |
| 2 | 5 | |
| 3–8 | 27–43 | fan-out as tags and headers accumulate |
| 9–23 | 60–79 | the wide middle |
| 24–27 | 33–34 | a distinct late set (`0x2b50`, `0x2c50`, `0x3250`, `0x3350`, `0x3c50`) |

Slice 0 keying only on state `0x0` is the anchor: **parsing starts from state zero**, and any
program we author has to as well.

## ★ The FIELDS binding is a datasheet fact, not a convention to reverse

The thing this document was written to say was missing — which channel carries what — is fixed in
hardware and documented. **Table 5-5, Parser Fixed Mapping:**

| channel | carries | channel | carries |
|---:|---|---:|---|
| 0 | ISL_FTYPE/VTYPE/PRI/USER | 15 | **L2_TYPE (EtherType)** |
| 1 | L2_VID1 (+L2_VPRI1) | 16/17 | L3_FLOW, L3_PRI |
| 2 | L2_VID2 (+L2_VPRI2) | 18 | L3_LENGTH |
| 3 | ISL_SGLORT | 19 | L3_TTL / L3_PROT |
| 4 | ISL_DGLORT | 20,21,36–39,32,33 | L3_SIP / L3_DIP |
| **5,6,7** | **L2_DMAC[15:0],[31:16],[47:32]** | 24,25 | L4_SRC / L4_DST |
| **12,13,14** | **L2_SMAC** | 8–11,26,27,40–42 | FIELD16{A..I} generic |

So a generated parser does not have to guess where to put the DMAC. It has to put it in channels
5/6/7 because the hardware reads it there.

## ⚠⚠ THE ACTION LAYOUT IS SETTLED — and both earlier answers here were wrong

The register header defines every PARSER_RAM field's exact bit position
(`FM6000_PARSER_RAM_l_/h_/b_*`). That is authoritative and ends the guessing:

```
SetFlags       0-37    Byte0-3Enable   54-57   StateOp2/Value2   80-89
Halfword0Dest 38-43    Halfword0/1Add  58-59   StateOp3/Value3   90-99
Halfword1Dest 44-49    StateOp0/Value0 60-69   StateFrameRot   100-101
Halfword0Rot  50-51    StateOp1/Value1 70-79   LegalPadding    102-103
Halfword1Rot  52-53                            TerminateAllowed    104
                                               Terminate           105
                                               ShiftNextSlice  106-108
```

**Two wrong answers preceded it, and both were reached honestly:**

1. *Table 5-3 order packed LSB-first.* Put `Halfword0Dest` at bit 80 → channels
   `{0,1,2,3,4,5,8,60,62}`, never the 5/6/7 and 12/13/14 that Table 5-5 fixes for DMAC/SMAC, and
   60/62 do not exist in a table stopping at 43. Refuted.
2. *Scanning offsets for a 6-bit field hitting those channels.* Gave a unique hit at **bit 45**
   with a compelling slice progression — DMAC high-to-low, then SMAC, then EtherType, deeper each
   slice. **Also wrong.** Bit 38 was in the candidate list and was discarded for hitting 6 of 7
   documented channels instead of 7 of 7.

The lesson worth keeping: a semantic scan over 2,117 samples can produce a unique, plausible and
wrong answer. Check the register header before inferring a layout.

### What the authoritative layout shows

| | |
|---|---|
| entries writing FIELDS | **1,455 of 2,117** (the broken layout said 131) |
| `Terminate` | **used — 344 entries** (the broken layout said never) |
| `TerminateAllowed` | 518 entries |
| `LegalPadding`, `StateOp3` | always 0 |
| channels written | all within the documented 0–43 range |

Channels seen at `Halfword0Dest`: L2_DMAC[15:0] and [47:32], L2_SMAC[31:16], L2_TYPE, L3_TTL/PROT,
L3_LENGTH, L3_FLOW, L3_SIP/DIP, L4_SRC, L2_VID1, ISL_SGLORT and the generic FIELD16 channels.

### Why the round-trip test caught none of this

`gen_parser --verify` reproduces all 2,117 entries bit-identically and still passes with the wrong
offsets. **A decode→encode round-trip proves only that the packing is self-consistent.** Shifted
field boundaries re-encode to exactly the same bits. It validates an encoder; it can never validate
an interpretation. Only an external fact can, and here that was Table 5-5.

The earlier corroboration — "every field lands inside its documented range" — was weak evidence
treated as confirmation. Several of these fields are narrow enough that a wrong offset still yields
plausible-looking values.

Both wrong layouts round-tripped 2,117/2,117 entries perfectly. The layout is now taken from the
register header rather than inferred, so `action_render()` no longer marks anything untrusted.

## What is still needed before authoring

| convention | state |
|---|---|
| key split (state / frame) | **derived** |
| entry state = `0x0` | **derived** |
| state-machine shape per slice | **derived** (shape, not meaning) |
| FIELDS channels written | **solved** — 1,455 entries, all channels in range |
| which FIELDS channel carries which header | **solved** — Table 5-5, fixed in hardware |
| full action field layout | **solved** — exact bit positions from the register header |
| *meaning* of each state value | not derived — which state means "seen one VLAN tag", etc. |
| slice budget per protocol | not derived |

The FIELDS binding — which this document previously called the binding constraint — turned out not
to be a constraint at all: it is fixed in hardware and documented in Table 5-5. Worth recording why
the earlier reasoning was wrong. L3AR does not read FIELDS directly; its inputs (Table 5-30) are
mapper-derived IDs — `L2_DMAC_ID3`, `L3_DIP_ID3`, `L2_TYPE_ID2`. The parser writes FIELDS, the
MAPPER associates those into small IDs, and L3AR keys on the IDs. So the parser-side contract is
Table 5-5 and nothing more.

What now gates authorship is narrower and more tractable:

1. **The meaning of state values** — which state encodes "one VLAN tag seen", etc. This is the
   last parser-side unknown.
2. **The MAPPER's configuration**, since it decides how FIELDS become the IDs L3AR matches on.

Both the encoding and the output contract are now settled, so authoring a parser is no longer
blocked on format questions.

## ★ Hardware validation (2026-08-09)

The first piece of this work to meet silicon. `parser_program.py` generated a CAM entry, and it was
written to the real FM6000 on the 7150 (`0000:02:00.0`) and read back:

```
slice 0, entry 127 (0x1001fc-0x1001ff)
  baseline    00000000 00000000 00000000 00000000
  written     7ffff7ff ffffffeb 7fff0800 ffff0014   <- gen_parser.encode_cam() output
  read back   7ffff7ff ffffffeb 7fff0800 ffff0014   <- byte-identical
  restored    00000000 00000000 00000000 00000000
  PIN 0x1c021 0x00000208 before and after
```

Chosen for zero risk: entry 127 already read all-zero, which is the *never-match* encoding, and the
written entry carried a never-match bit at 31 (outside its care mask) so it could not fire whatever
the traffic. The slot was restored afterwards.

**What this proves:** the `PARSER_CAM(slice, entry, word)` address arithmetic is right against real
hardware, the `words[1:0]=KeyInvert / words[3:2]=Key` ordering is right, and our encoder's bit
patterns survive a write/read round trip through the chip. None of that could be established by
self-consistency, however many round-trips passed.

**What it does not prove:** anything about behaviour. The entry was deliberately unable to match.
Whether a generated parser actually parses is a different question needing L3 extraction, a cold
boot, and a stock-replay control at the same cadence.

## ★ Our parser ran on the chip (2026-08-09/10)

`parser_program.py` emitted a program, `--splice` put it into a copy of the replay, and the FM6000
loaded it. Read back from silicon at three separate slices — 0 (DMAC extraction), 3 (EtherType
dispatch, both VLAN entries) and 7 (IPv4 SIP/DIP) — **every word matched what the generator
produced.** The bring-up sequence reached `FULLSEQ DONE`, `PIN=0x00000208`, and Et1 trained to
`0xcc0` with `pcsRx=1`.

### ⚠ The first attempt was an invalid test, and the reason matters

A full cold boot with the spliced replay appeared to succeed: sequence completed, Et1 up. Reading
the chip showed it was running **EOS's parser, not ours**.

The boot script's own generator chain contains `gen_list_early fm6000_parserinit PARSER`, and
`fm6000_parserinit.c` is precisely the file carrying EOS's transcribed tables. The splice removed
EOS's parser writes from the replay; the generator then put them straight back. Nothing in the log
hinted at it — the run looked like a pass.

**Anything that appears to validate a replacement must be checked by reading the hardware, not by
whether the sequence completed.** The valid run needed `chmod -x /usr/bin/fm6000_parserinit` so the
chain skips PARSER entirely, confirmed by `PARSER` being absent from the log.

### What this establishes, and what it does not

| | |
|---|---|
| our program loads onto real silicon, bit-exact | **yes**, three slices verified by readback |
| the chip survives it | **yes** — sequence completed, PIN healthy, no wedge |
| Et1 trains and holds link | **yes**, `0xcc0`/`pcsRx=1`, same as the control |
| **frames are parsed correctly** | **NOT TESTED** |
| forwarding, routing, ping | **NOT TESTED** |

The valid run was an **in-place re-run, not a cold boot**. `SELF-CONTAINED-PLAN.md` is explicit
that in-place re-runs are good for link-level checks and useless for forwarding — both a modified
replay and the unmodified control give `et1 rx=0` that way. Neither run had `et1`/`et2` Linux
interfaces at all, so no traffic test was possible in either.

Et2 read `0x815`/rx=0 on the run with our parser and `0x8c0`/rx=1 on the cold-boot control. That is
inside the documented pre-existing copper flap (`ET2-COPPER-LINK.md`; the docs' own three-boot
series shows `0x8c0`, `0x815`, `0x8c0`), and the parser cannot affect SerDes training, which is
EPL's job and untouched here. It is not attributed to our parser — but it is not cleanly ruled out
either, because the two runs differed in more than one variable.

**So: the program is real, resident and non-fatal. Whether it parses correctly is still unknown.**
That needs a cold boot from an image built without `fm6000_parserinit`, since the RAM rootfs
discards `chmod` on reboot.

## ★★ A/B on hardware: EOS's parser forwards, ours does not

The decisive experiment, and it is clean: same box, same boot, same dataplane,
`fm6000load` swapping only the parser program between measurements.

| parser | kernel routes | ping to neighbour |
|---|---:|---|
| **EOS's** | **35** | **4/4, 0% loss**, adjacency in <4s |
| **ours** | 2 | 0/4, 100% loss |

`et1` rx counts grow under both, so frames reach the CPU either way — our parse
is not fatally wrong, it is wrong in a way that stops downstream classifying.

### Two earlier claims this overturns

**In-place runs CAN test forwarding.** `SELF-CONTAINED-PLAN.md` says they cannot
(both modified and control give `et1 rx=0`), and this document repeated it. That
holds when `fm6000-fullseq.sh` is re-run over a live dataplane; it does not hold
here, because `portd` was started fresh on this boot. EOS's parser reached 35
routes and full ping in an in-place state.

**The multicast-DIP flag was not the cause.** Table 5-6 bit 19 (`L3_Mcst` from
the DIP) was genuinely missing and is a real fix — OSPF hellos to 224.0.0.5 are
multicast by address, not just by MAC — but adding it changed nothing. Routes
stayed at 2. A plausible mechanism is not a diagnosis.

### What the loop is worth

Loading a parser with `fm6000load` and measuring takes about a minute, against
roughly ten for a reboot cycle:

```
fm6000load 0000:02:00.0 <clear+program>   # ~17k writes, seconds
ip route | wc -l ; ping -c4 10.101.101.25 # verdict in <10s
```

Clearing slices 0-15 to all-zero first is required: all-zero is the never-match
encoding, so it disables stale entries from a previous load that would otherwise
survive at higher indices and win under last-match-wins.

### ★ The FFU scenario key, read off the chip

`MAPPER_SCENARIO_FLAGS_CFG` (0x123e00, four words; one 6-bit selector per byte —
an 8-bit stride, per the register header, not the 6 a naive reading gives) says
exactly which 16 ACTION_FLAGS feed the FFU's SCENARIO_CAM on this box:

```
0 Unbound0     2 ISL_Type0    3 ISL_Type1   4 ISL_FType0
8 IsIPv4       9 IsIPv6      13 TTL_Expired 14 HeadFrag
16 L3_Options 19 L3_Mcst(DIP) 27,28 HdrOffsets[3:4]
40, 42, 43 — mapper-set, beyond the parser's 38-bit SetFlags
```

**This retires an earlier wrong conclusion.** This document previously argued the
ISL flags were "correctly left clear" because our ports carry no ISL tag. The
scenario key matches on `ISL_Type0/1` and `ISL_FType0` regardless, and the
datasheet says FType is "specified by the ISL tag **or assigned**". Leaving them
zero changes the scenario value, which selects different FFU rules.

`TTL_Expired` and `HeadFrag` were also missing and are also in the key.

### ⚠ All of that was implemented, and forwarding still fails

```
t=30s  routes=34  ping 100% loss     (OSPF routes still aging out)
t=60s  routes=2   ping 100% loss
t=120s routes=2   ping 100% loss
```

Three hypotheses have now been tested on hardware and none fixed it:

| hypothesis | grounding | result |
|---|---|---|
| missing `L3_Mcst(DIP)` bit 19 | Table 5-6 + OSPF is 224.0.0.5 | no change |
| parser off by one slice | EOS's earliest DMAC write | refuted before testing — EOS writes DMAC at slice 0 too |
| missing scenario-key flags | **read off the chip** | no change |

Each was better grounded than the last, and the third was measured rather than
inferred. That pattern says the remaining defect is unlikely to be found by
proposing mechanisms.

### What to do instead

Stop guessing and bisect, which the fast loop now makes practical (~1 minute per
trial against ~10 for a reboot). EOS's parser works and ours does not, so the
difference is findable by construction: start from EOS's program and replace it
toward ours a piece at a time, or instrument the chip — the FFU exposes scenario
and rule-hit state, and reading what the silicon computes for a live frame would
show where classification diverges rather than leaving it to be reasoned about.

### ★★ ROOT CAUSE: CPU-injected frames carry an 8-byte F64 ISL tag

Found by bisection plus the asymmetry the earlier runs kept showing.

Three measurements decomposed it:

| parser | routes | ping |
|---|---:|---|
| EOS, all 28 slices | 35 | **works** |
| EOS, truncated to slices 0–15 | **35** | fails |
| ours, slices 0–15 | 2 | fails |

Truncated EOS forms a full adjacency, so **slice depth is not what blocks OSPF**
— our program fails at something EOS's does within the same slices.

⚠ **The ping column is not a reliable metric, and one earlier reading of it here
was wrong.** `SELF-CONTAINED-PLAN.md` documents a pre-existing defect: ping
collapses to 100% loss within about three minutes *on the stock replay as well
as on every generated variant*, while management stays clean. Confirmed again at
the end of this session — EOS's parser restored, routes=34, ARP resolved
(`lladdr 80:a2:35:81:ca:b4`), and ping still 0/4.

So "EOS truncated to 0–15 fails ping" does **not** establish that depth is
needed for forwarding. That was a second defect I inferred from a metric that
degrades on its own. **Route count is the reliable signal** — 35 versus 2 is
robust and reproducible across every trial; ping is not.

The decisive clue was that `et1` rx kept growing under our parser while no
adjacency ever formed: we were *receiving* fine and failing to *transmit*.
`asic/fm6000/fm6000_txinline.c` says why:

```
DMAC        SMAC        <---- F64 tag, 8 bytes ---->        ethertype
```

CPU-injected frames carry an 8-byte F64 ISL tag **between the SMAC and the
EtherType**. Our program assumes DMAC + SMAC + EtherType, so at slice 3 it reads
the tag's first bytes as an EtherType, matches nothing, and falls through to the
terminate rule. Every frame the switch itself sends is mis-parsed and dropped —
the neighbour never hears our hellos, so no adjacency, and ARP never resolves,
so no ping.

Frames arriving from the wire are untagged and parse correctly, which is exactly
why `rx` grew and made the parse look half-working.

This also explains two things that were puzzling earlier:

- why `ISL_Type0/1` and `ISL_FType0` are in the FFU scenario key at all
- why EOS dedicates FIELDS channels 3 and 4 to `ISL_SGLORT` / `ISL_DGLORT`

and it retires the reasoning in commit 162bc63 completely: the ISL flags were
never "correctly left clear because our ports carry no ISL tag". Our ports carry
no ISL tag *on ingress from the wire*. On the inject path they always do.

### What the fix requires

The parser needs an ISL-tagged path: recognise the F64 tag after the SMAC,
extract `ISL_SGLORT`/`ISL_DGLORT` into channels 3/4, set `ISL_RX_Tagged` and the
Type/FType flags, and resume the EtherType dispatch 8 bytes later. That is a
second entry path into the existing dispatch, not a rewrite — the states after
the EtherType are unchanged.

### The open question

Not yet diagnosed. One structural difference stands out for a next look: EOS's
slice 0 classifies the DMAC into four different next-states (`0x03`, `0x04`,
`0x06`, `0x07`) by content, where ours has a single path and only distinguishes
the I/G bit. If downstream expects the DMAC class encoded in the state, our
uniform path would lose it — but that is a hypothesis, and the last one was
wrong.

## ★★ COLD BOOT VALIDATED (2026-08-10)

Built a SWI with `fm6000_parserinit` **removed from the initrd**, spliced our parser into the
replay, set `boot-config`, cold booted.

```
image     edgenos-ourparser.swi   md5 0764934006fccc73145e681cd7ce902c
          /usr/bin/fm6000_parserinit: No such file or directory
boot log  no PARSER generator ran
replay    373,345 writes (EOS's 18,032 parser writes out, our 1,568 in)
parser    0x100c01 = 0x94ffffeb   ours, resident after a cold boot
Et1       0xcc0, pcsRx=1          FULLSEQ DONE

routes    34   34   34    across three minutes
ARP       resolved
rx        52 -> 90                fresh boot counters
```

**EOS's parser program exists nowhere in this configuration** — not in the image, not in the
replay — and the switch forwards. `fm6000_parserinit.c` can now be deleted rather than bypassed.

Method: a SWI is a zip containing a gzip cpio initrd, so removing a binary and repacking takes
about a minute and needs none of the build toolchain. Repack with `zip -X -0` (stored), matching
how the EOS image is built.

## Scope: what the 7150S datasheet says we can leave out

The product datasheet (`arista-reverse-engineering/docs/datasheets/7150S_Datasheet.pdf`)
explains most of the parser complexity we deliberately skipped, and confirms none of it is an
EdgeNOS requirement:

| 7150S feature | what it accounts for in EOS's parser |
|---|---|
| wire-speed **VXLAN / NVGRE** gateway | the deep slices 16–27, and the `…DefaultDglortVxlan` L2AR rules |
| line-rate **NAT / MNAT** | L4 port extraction (ch24/25) — NAT needs the ports |
| **IEEE 1588 PTP**, boundary + transparent clock | the `0x88f7` entries |
| **FCoE** | the `0x8906` entries |
| **LANZ**, sFlow, per-packet timestamping | generic FIELD16 channel usage |

EOS's 2,117 rules cover a product that does all of this. EdgeNOS needs Ethernet, VLAN, IPv4,
IPv6 and ARP, which is why 196 rules suffice — the gap is features, not fidelity.

⚠ It does **not** explain the open ARP diff, and one reading of it here was wrong. EOS writes
`0x0000` into ch22/23 on the ARP path because it keeps parsing into the ARP body, where the
padding is zero — not because it deliberately zeroes the DIP channels to make `DIP_V4InV6`
well-defined. Our program terminates at the ARP dispatch instead. A design difference, not a
defect, and outside the scenario key.

## Reproducing

```
python3 asic/fm6000/tools/parser_decode.py --image <fm6000Microcode.raw> --slice 4
```


---

## The F64 frame type: bit 15 of tag word 0 — and we do not implement it

**2026-08-11.** Decoded while trying to make a CPU-injected frame egress port 3. The chain runs
backwards from L3AR and lands in the parser.

### What the frame type is

L3AR's two ISL rules discriminate on a single ACTION_FLAGS bit:

```
r28 SpecialDelivery     ACTION_FLAGS bit 0 must be 1
r5  IslF64FtypeNormal   bits 3 and 10 set, bit 0 must be 0
```

EOS's parser sets that bit in exactly one place, and three sibling entries make the encoding
unambiguous:

| entry | halfword0 | care | SetFlags | meaning |
|---|---|---|---|---|
| s3e27 | `0x8000` | `0x8000` | `[0, 3]` | **SPECIAL** + ISL_TYPE1 |
| s3e28 | `0x0000` | `0x8000` | `[3, 4, 6]` | ISL_TYPE1 + FTYPE0 + VLAN1_TAGGED |
| s3e29 | `0x0000` | `0xc000` | `[3]` | ISL_TYPE1 only |

**Bit 15 of the F64 tag's first halfword is the frame type.** Set → special delivery; clear →
the normal ISL paths. It fits the semantics: `SpecialDelivery` keeps `StrictDestGlort` (AF33), which
is what makes the tag's DGLORT authoritative instead of subject to a lookup.

`fm6000_portd` and `fm6000_txinline` both send word 0 = `0x0100` — EOS's captured *normal* tag —
so every frame EdgeNOS has ever injected has been NORMAL. `fm6000_l2.h`'s instruction to "inject
F64 ftype=SPECIAL dglort=0xFF00" has never actually been followed.

### ⚠ And our parser cannot act on it

Measured on the live chip running our parser:

```
our parser: 3,584 actions, 0 set ACTION_FLAGS bit 0
```

`parser_program.py` contains no rule for it — no mention of SPECIAL, ftype, or `0x8000`. So
injecting `word0 = 0x8100` changes nothing: the flag is never set, `SpecialDelivery` never matches,
and the DGLORT is never treated as authoritative. Confirmed by experiment — NORMAL and SPECIAL
tags produced identical (absent) egress.

This is a genuine gap in our parser rather than a misconfiguration, and it is well bounded: one
rule at `S_ETYPE` under the CPU-port state, matching halfword0 bit 15, setting SetFlags bits 0 and
3. EOS needs exactly one entry for it.


## The full set of F64 frame types — there are exactly three

Enumerating every EOS parser entry at slice 3 that constrains the tag halfword, with the full key
rather than just `halfword0`:

| entry | hw0 15:14 | hw1[11:0] | SetFlags | meaning |
|---|---|---|---|---|
| e27 | `1x` | — | `[0, 3]` | **SPECIAL delivery** + ISL_TYPE1 |
| e28 | `0x` | — | `[3, 4, 6]` | ISL_TYPE1 + ISL_FTYPE0 + VLAN1_TAGGED |
| e29 | `00` | `== 0` | `[3]` | ISL_TYPE1 only |

All three require `state1` bit 0 and `state = 0x10` (the EtherType state) — that is what separates
them from the ethertype dispatch sharing the same state, and it is why last-match-wins does not
let them swallow ordinary IPv4.

★ **Two bits, not one.** Bit 15 selects special delivery. **Bit 14 also matters**: e29 requires
both top bits clear *and* `hw1[11:0] == 0` — a DGLORT whose low 12 bits are zero. It is the most
specific of the three, which is how it wins over e28 for that case despite the overlap.

So the tag's frame-type space is small and now fully enumerated: special, normal-tagged, and a
bare ISL case with no destination in the low DGLORT bits.

### What our injected frames actually hit

`portd`/`txinline` send `word0 = 0x0100`, `word1 = 0xff00` or `0x03ed`:

```
hw0 bits 15:14 = 00     -> not e27
hw1[11:0]      = 0xf00 / 0x3ed, non-zero  -> not e29
                                          -> e28, SetFlags [3, 4, 6]
```

Every frame EdgeNOS injects lands on **e28, the ordinary tagged path** — never e27 and never e29.
Setting `word0 = 0x8100` moves it to e27, which our parser can now express (added 2026-08-11), but
that alone did not produce egress on an unconfigured port.

### For the ethertype side, for completeness

e3–e26 dispatch the untagged ethertypes at the same state: `0x0800` IPv4, `0x86dd` IPv6, `0x0806`
ARP, `0x8100`/`0x88a8` VLAN (with tag-depth variants), `0x8808` PAUSE, `0x88f7` PTP, `0x8906` FCoE,
`0x8926`, plus a `0xbeef` and `0xffff` case in other states.

## 2026-08-15: MOD command split — hypothesis A survives a discriminating test, and two opcodes get named

Static analysis only, against EOS's own 302 valid MOD steps in the replay. No hardware; the box was
busy measuring something else. `mod_decode.py` records the split as "a hypothesis with evidence, not
a settled fact" — this narrows it.

### The rival reading is now the worse one

The datasheet says *every command carries a flag for whether it contributes to the checksum
accumulator*, which makes a flag bit inside the Command byte plausible and gives a serious
alternative to the working hypothesis:

```
A   opcode[7:5] : operand[4:0]                (mod_decode.py's hypothesis)
B   csum[7] : opcode[6:4] : operand[3:0]      (a flag bit, 4-bit operand = length 1..16)
```

B is attractive: 8 of EOS's 47 distinct bytes appear as both `x` and `x|0x80`, and 4 operand bits
match INSERT/DELETE/REPLACE's documented 1..16 exactly. It still loses.

Grouping the 47 bytes both ways, **A partitions them with no parity exceptions** — opcode 2 is 13
of 13 odd (even lengths, as the halfword-channel argument requires), opcodes 6 and 7 all even. **B
needs two exceptions**, and they are exactly `0xc0` and `0xd0` — the two bytes A assigns to a
different opcode entirely. An explanation that needs exceptions precisely where its rival has none
is the weaker explanation.

### Two opcodes named, by route-enrichment

51 of the 302 steps have a CAM that *requires* `MOD_FLAGS[14]` — the route flag, absolute key bit 38.
That is a 16.9% baseline, so a command that is route-specific will be enriched against it. Exact
binomial tail, commands with n≥4:

| cmd | route/total | under A | p |
|---|---|---|---|
| `0x20` | 9/14 | op1, operand 0 | 9.8e-05 |
| `0x85` | 7/9 | op4, operand 5 | 1.0e-04 |
| `0xe0` | 7/9 | op7, operand 0 | 1.0e-04 |
| `0xd0` | 6/16 | op6, operand 16 | 4.0e-02 |

`0x85` falling out as route-bound is a **free consistency check**: `mod_decode.py` reached the same
conclusion from the CAM by hand, and it reappears here from a test that knew nothing about it.

The new result is the other two. **`0x20` and `0xe0` are strongly route-bound and both carry
operand 0** — no length. Routing requires exactly two edits that have no length operand: `DECREMENT`
(the TTL, 0 value bytes) and `CHECKSUM` (the ones-complement fixup, 2 value bytes). So:

> **`{0x20, 0xe0}` = `{DECREMENT, CHECKSUM}`, i.e. opcodes 1 and 7.**

⚠ **Which is which is NOT established.** Both are operand 0 and both are route-bound; nothing here
separates them.

### What this does to A4

A4's hardware test was "program one MOD step with a candidate command and look at what comes out",
over a space of 47 observed bytes. **It is now a two-way question**: program `0x20`, program `0xe0`,
and see which decrements the TTL and which repairs the checksum. That is one capture on the transit
rig, and `tools/transit-test.sh` already produces exactly the byte-level egress view it needs.

### ⛔ One test I ran and had to throw away

I tried to settle it by arity — count each step's value operands with a non-zero Type and match
against the documented value-byte counts. It produced mixed counts (0, 2, 3, 4) for nearly every
command, which is not a signal but a mistake: I indexed `MOD_VALUE_RAM` at the same `(profile,
step)` as the command, and command slices and data slices are **not** the same slices —
`mod_decode.py` says so in its own header, and I did not read it before writing the test.

Recorded because the arity test is still the right idea: done with the correct slice mapping it
would separate `0x20` from `0xe0` on the bench, with no hardware at all. Someone should do it
properly.

### Correction, and why the arity test cannot be done statically after all

**Two corrections to the section above, neither of which changes its conclusion.**

**1. The CAM slice mapping was wrong for banks 17-19.** `MOD_COMMAND_RAM` banks 16..19 are all
driven by **CAM slice 16** (`mod_decode.command_bank()`); slices 17..30 are *data* slices. My first
enrichment run paired banks 17-19 with CAM slices 17-19, i.e. with data slices. Redone correctly the
baseline moves from 51/302 to 52/302 and the result stands:

```
0x20   9/14   op1/0    p = 1.15e-04
0x85   7/9    op4/5    p = 1.17e-04
0xe0   7/9    op7/0    p = 1.17e-04
```

Only 3 steps were affected, but the check was worth running — the same class of index-pairing error
is what `value_bank()`'s own comment records as having survived a passing `--verify` round-trip.

**2. ⛔ "Someone should do the arity test properly" was wrong — it cannot be done statically.**

I suggested counting each step's value operands and matching the documented value-byte counts. That
is not merely a matter of fixing the index. Commands and values come from **different CAM slices**:
commands from slices 0..16, values from data slices 17..31 via banks 0..14. Each slice contributes
at most one entry *per frame*, so a command and the operands it consumes are associated
**positionally in a per-frame stream**, not statically as a `(profile, step)` pair. There is no
static mapping from a command word to "its" value words to count.

It could still be done statically for a *specific* scenario — fix a key, work out which entry each
slice yields, reconstruct both streams, and test whether `sum(length(cmd))` matches the value bytes
available under each candidate split. That is a real experiment and a much larger one than it looked.

**So A4's decisive test remains hardware**, exactly as `mod_decode.py` said. What has changed is its
size: not "try candidate commands" across 47 bytes, but *"is `0x20` the TTL decrement and `0xe0` the
checksum fixup, or the other way round?"* — one transit capture, two candidates.

## 2026-08-15: FFU ByteMux — the parsimony test fails, but the widths say something

### ⛔ The test I expected to discriminate does not

B1's open question is whether `ByteMux` is (a) a direct halfword-channel index, or (b) a byte
address (`channel = v//2`, byte `v%2`). The obvious test: EOS's parser writes only some channels, so
whichever reading points at channels the parser never writes is the wrong one.

Ran it against every value EOS actually programs — 17 distinct `ByteMux` values across
`FFU_SLICE_SCENARIO_CFG`, and the 33 halfword channels its parser writes:

```
channels written  : 0-3, 5-7, 12-25, 28-39            (33 of 64, max 39)
ByteMux values    : 0 1 2 3 8 17 18 21 24 32 33 34 35 40 53 58 60

reading (a)  v is the channel     -> 5 values hit unwritten channels:  8 40 53 58 60
reading (b)  channel = v//2       -> 5 values hit unwritten channels:  8 17 18 21 53
```

**Exactly five misses each.** The test does not discriminate, and `FEATURE-COMPLETE-CHECKLIST.md`'s
"the shipped configurations refute neither" stands unchanged. Recorded so nobody runs it twice.

### ★ What the field widths do settle

Two facts from the header that were not previously written down:

```
PARSER_RAM  Halfword0Dest [43:38]   6 bits  -> 64 halfword channels, not 32
            Halfword1Dest [49:44]   6 bits
FFU_SLICE_SCENARIO_CFG
            ByteMux_0..3            6 bits each
            Top4Mux                 5 bits
MOD VALUE_RAM  Val*_DataSelect      5 bits  -> reaches channels 0-31 only
```

**The channel space is 64 halfwords.** And each `ByteMux` contributes **8 bits** to the 38-bit key
(4x8 + 6 = 38, the arithmetic already in this file), i.e. it names a *byte*. Six bits naming a byte
covers 64 bytes = **32 halfword channels**. A byte address into the full 64-halfword space would
need 7 bits and there are only 6.

So, whichever way the mapping goes:

> **The FFU can only reach half the channel space.** Under (b) it sees bytes of channels 0-31;
> under (a) it sees halfwords 0-63 but then contributes 8 bits of a 16-bit halfword with nothing
> naming which byte — which the key arithmetic does not allow. **(b) is forced by the key width**,
> independently of the parsimony test that failed above.

⚠ And that raises a new question worth more than the original one: **EOS's parser writes channels
32-39, which neither the FFU (6-bit byte address, tops out at channel 31) nor MOD (5-bit
`DataSelect`, tops out at 31) can address.** Something else consumes the upper channels, or they are
written for an effect other than being selected. Finding that consumer is likely to name the upper
channels for free — the same way naming a channel once named it everywhere for `L3_SIP`/`L4_SRC`.

### ⛔ The MAPPER is not the consumer of the upper channels

Searched the header for every 5- and 6-bit selector-shaped field, looking for a block whose selector
is wide enough to reach halfword channels 32-63. One candidate stood out —
`MAPPER_SCENARIO_FLAGS_CFG`, sixteen 6-bit `FlagN_MuxSelect` fields — and EOS programs three of them
above 32:

```
Flag0..15 MuxSelect = 0 16 4 0 28 3 14 27 8 9 13 40 19 42 43 2
values >= 32: 40, 42, 43
```

Encouraging for about a minute, and then wrong: **that is a different bus.** `FlagN_MuxSelect`
selects a *flag* source, not a byte of halfword-channel data — the parser's own `SetFlags` field is
38 bits wide, a flag space, entirely separate from the 16-bit data channels that `Halfword0Dest` and
`Halfword1Dest` write. Selecting flag 40 says nothing about halfword channel 40.

**So the open question survives the search, and is sharper for it:**

- The FFU reaches channels **0-31 only** — `ByteMux` is a 6-bit byte address, and `Top4Mux`'s 5 bits
  likewise cover 32 halfwords. Both halves of the FFU (`FFU_SLICE_SCENARIO_CFG` and
  `FFU_BST_SCENARIO_CFG1`) have the identical field widths.
- MOD reaches **0-31 only** (5-bit `DataSelect`).
- EOS's parser writes up to **channel 39**.
- No block found in the header has a selector that reads halfword channels 32-63.

Either those channels feed a consumer whose selector is not shaped like the ones searched for, or
the destination space is not what it appears — e.g. if `Halfword0Dest` is a *byte* position rather
than a halfword index, "channel 39" would be byte 39, halfword 19, and everything fits inside 32
halfwords with no upper half at all. `Halfword0Rot` (a rotate, at halfword granularity) is weak
evidence against that reading, and it is untested either way.

**Do not build on "there are 64 halfword channels" until this is settled.** It follows from the
6-bit `Dest` field, and it is the assumption that makes the upper half mysterious in the first place.
