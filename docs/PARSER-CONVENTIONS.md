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

### ✅ `Halfword0Dest` IS a halfword index — the 64-channel space is real

The caveat above (that `Dest` might be a *byte* position, collapsing the space to 32 halfwords and
dissolving the upper-half puzzle) is **refuted by the shipped configuration**. Over EOS's parser
actions:

```
Halfword0Dest   166 writes   81 odd / 85 even
Halfword1Dest   159 writes  119 odd / 40 even
```

A 16-bit halfword landing at a *byte* position would be aligned, so `Dest` would be even. Roughly
half of `Halfword0Dest` and three quarters of `Halfword1Dest` are **odd**. Unaligned 16-bit writes
at arbitrary odd byte offsets is not a plausible reading; arbitrary indices into a 64-entry halfword
space is.

Also checked, in case the two halfwords were the halves of one 32-bit field: the delta
`Halfword1Dest - Halfword0Dest` is scattered (`+14` x24, `+1` x19, `-1` x16, `+7`, `+3`, `+2`,
`+11`, `-15`...), so the two halfwords of an action generally go to **unrelated channels**.

**Conclusion: there are 64 halfword channels, the FFU and MOD can each address only the lower 32,
EOS's parser writes as high as 39, and no consumer of channels 32-63 has been identified.** That is
a real gap, not an artefact of a bad assumption — which is what it needed to be checked for.

### ⚠ WEAKENED: `{0x20, 0xe0}` = `{DECREMENT, CHECKSUM}` fails a co-occurrence test

Earlier today I concluded from route-enrichment that `0x20` (op1/operand 0) and `0xe0` (op7/operand
0) are the two length-less routing edits, `DECREMENT` (TTL) and `CHECKSUM`. **A test I did not run
at the time contradicts the pairing.**

Routing an IPv4 frame needs *both* edits: decrement the TTL, then repair the header checksum for it.
Two commands that are that pair must appear **together** in any program that routes. Over EOS's 18
populated programs:

```
contain 0x20 : 5
contain 0xe0 : 5
contain BOTH : 1        expected under independence: 1.39
```

**They are independent, not paired.** One program of eighteen carries both, which is what pure
chance predicts. A TTL decrement without a checksum fixup is a broken IPv4 router; four programs
have `0x20` without `0xe0` and four have `0xe0` without `0x20`.

**What survives, and what does not:**

- ✅ **Both commands are genuinely route-enriched** — `0x20` 9/14 and `0xe0` 7/9 against a 17%
  baseline, p ≈ 1.2e-04. That was a test of each command separately and it stands.
- ⛔ **The identification of them as the `{DECREMENT, CHECKSUM}` pair does not.** Route-association
  made them *candidates*; I promoted that to an identification on the strength of "routing needs
  exactly two length-less edits", which is an argument about the datasheet, not about the data.
- The co-occurrence partners are also unhelpful: both appear most often beside `0x41` (op2/1), which
  is simply the most common command overall.

**So A4's hardware test is back to being genuinely open**, not a two-way tiebreak. What it has kept
is the *shortlist*: `0x20` and `0xe0` are the two most strongly route-bound commands with no length
operand, so they remain the first things to program on the transit rig — but the experiment must be
"what does this command do", not "which of these two is the TTL".

### The only program carrying both, for whoever runs that test

```
program 15, in stream order
  slot  5  c0   op6/0    jitter=0
  slot  6  20   op1/0    jitter=1
  slot  7  20   op1/0    jitter=1
  slot  8  e0   op7/0    jitter=1
  slot  9  e0   op7/0    jitter=1
  slot 10  98   op4/24   jitter=0
  ...
```

Two structural observations worth carrying into the bench test, neither yet explained:

- **Commands appear in consecutive runs of identical values** — `20 20`, `e0 e0`, `98 98`,
  `01 01 01`, `41 41 41`. A step is not written once.
- **`Jitter` is 1 on exactly `0x20` and `0xe0` here** and 0 on their neighbours (`c0`, `98`, `01`,
  `0b`), 7 on `0x41`. The field is 6 bits at `[13:8]` and its meaning is undocumented in our notes;
  it is the only field that separates the two candidates from everything around them.

### Our generated MOD is byte-identical to EOS's — 640 of 640 words

Read back from the live chip on alpha12, with `MOD generated by us` in the FULLSEQ log, against EOS's
own program in the replay:

```
checked 640 MOD_COMMAND_RAM words, 0 differ
live steps using 0x20: 14      using 0xe0: 9
```

Two things follow:

- **`fm6000_modinit.c` faithfully reproduces EOS's MOD program.** Not "works" — identical. That
  retires the hypothesis (raised 2026-08-15 when punted frames looked untagged) that our MOD
  generator had silently dropped a step EOS performed.
- **The A4 candidates are live on the chip**, 14 and 9 steps respectively, so the bench test needs no
  new programming to observe them — only to perturb one and watch the wire.

### A4's bench test, now that transit works

The transit rig gives a frame whose transformation is fully visible: TTL `64 -> 63` and a recomputed
checksum, captured on the peer's `swp6`. The test is leave-one-out:

1. pick a step using `0x20` (or `0xe0`) in the program that handles routed IPv4;
2. clear its `Valid` bit (`MOD_COMMAND_RAM` bit 14) — one register write;
3. send a frame through the hairpin and read the egress capture.

**TTL stops decrementing → that command is `DECREMENT`. Checksum goes stale → it is `CHECKSUM`.**
Neither changes → it is neither, and the shortlist was wrong.

⚠ Two cautions. **Which program handles our routed frame is not yet identified** — the MOD profile is
selected by a CAM match on frame attributes, and picking the wrong bank perturbs nothing and proves
nothing. And ⚠ **this writes to a live forwarding table**; `MOD_COMMAND_RAM` is not one of the tables
that refuse writes after boot (`PARSER_INIT_FIELDS` is), so the change will take and forwarding will
be affected until the next boot. Do it with the box already in a known state and a reboot budgeted.

### A4 bench test: first run, one valid result and a wedged box

**2026-08-15/16.** Transit works, so the test is finally runnable: clear a MOD step's `Valid` bit,
send a frame through the hairpin, and read TTL and checksum off the peer's `swp6`. Baseline is
`ttl 63` with a good checksum.

**Command-level leave-one-out found nothing.** Three hand-picked steps — `slice 8 slot 9` (`0x20`),
`slice 7 slot 7` (`0xe0`), `slice 10 slot 14` (`0x20`) — each cleared, each restored, TTL unchanged
at 63 every time. I had picked them by constraining only `DST_PORT_Tag` and the route flag; the CAM
key is 48 bits and the rest evidently excludes those entries. **Predicting which entry fires needs
the whole key, and guessing at it does not work.**

**Slice-level is the right search** — disable a whole slice and whatever would have fired cannot,
which is 20 tests instead of 302 and needs no shortlist. Valid results before the run went bad:

```
slice 0  ttl 63       no effect
slice 1  ttl 63       no effect
slice 2  ttl 63       no effect
slice 3  no frame     forwarding stops entirely
```

⚠ **Slice 3 agrees with an independent inference.** `mod_decode.py` reasoned from field widths and
value-bank contents that the operand-5 (length-6) commands `0x85`/`0x05` are the **MAC rewrite**, and
slices 3 and 4 are where those live. Remove the MAC rewrite and there is no valid frame to emit.
Two different methods, same conclusion.

### ⛔ Everything after slice 3 in that run is invalid

Slices 4-8 also reported "no frame". They were not five more essential slices — **the box wedged at
slice 3 and never recovered**, and every later reading was the same dead state. Confirmed by probing
with nothing perturbed: 0 frames captured.

The cause was the harness, not the chip: the bank save captured its 32 values through a nested shell
loop, the guard only checked that the result was non-empty, and a short capture then had the restore
write **zeros** over a live table. `0x159060` read `0x00000000` afterwards. A reboot fixed it —
FULLSEQ reprograms `MOD_COMMAND_RAM` from scratch, so the damage is transient by construction.

**Three rules, now implemented in `tools/a4-slice-sweep.sh`:**

1. **Verify the save** — exactly 32 values or skip the bank. "Non-empty" is not a check.
2. **Health-check between tests** with nothing perturbed. Six readings were taken after the box died.
3. **Restore is not recovery.** A restored table does not un-wedge a dataplane in flight. Treat a
   frame stopping as terminal, reboot, and `RESUME_FROM` the next slice.

⚠ The single-register `a4-leaveout.sh` was safe throughout — it saves one value, and all ten
registers it touched verified restored. The failure came from scaling the write without scaling the
check.

### A4 slice sweep, second run — and last night's "wedge" was my restore, not the chip

**2026-08-16.** With the fixed harness (verified 32-value save, health check between tests, wedge
treated as terminal):

```
slice 0   ttl 63     no effect
slice 1   ttl 63     no effect
slice 2   ttl 63     no effect
slice 3   no frame   RECOVERED
slice 4   no frame   RECOVERED
slice 5   ttl 63     no effect
slice 6   ttl 63     no effect
slice 7   no frame   wedged -- reboot, RESUME_FROM=8
```

★ **Slices 3 and 4 recovered this time.** Last night disabling slice 3 wedged the box permanently and
I recorded it as a property of the perturbation. It was not: **the permanent wedge was my restore
writing zeros over the bank**, because the save was guarded only by "non-empty". With a verified
save, disabling those slices is fully reversible. The harness fix removed a fault I had attributed
to the hardware.

**Three slices are load-bearing for emitting the frame at all: 3, 4 and 7.**

- 3 and 4 carry the operand-5 (length-6) commands `0x85`/`0x05` that `mod_decode.py` inferred to be
  the MAC rewrite from field widths and value-bank contents. Remove the MAC rewrite, get no valid
  frame. Two independent methods agreeing.
- ⚠ **Slice 7 carries `0xe0`** at slots 4, 5 and 7. If `0xe0` is essential to producing a valid frame
  at all, that constrains what it can be — a checksum fixup being *required* for a frame to appear on
  the wire is possible (a bad checksum could be dropped downstream) but is not the obvious reading.

**No slice has yet changed the TTL while continuing to emit**, which is the signature that names
`DECREMENT`. Slices 8-19 remain.

⚠ Note the cost model: each wedge needs a reboot, and each reboot needs Et2 to link (~50%) **and**
per-port RX to work (2 of 3 boots observed). So productive boots are perhaps 1 in 3.

## ★★★ A4: the TTL decrement is in MOD slice 14

**2026-08-16.** The slice sweep completed all 20 slices. Baseline is `ttl 63` (the peer sends 64;
the switch decrements once).

```
slice  0  1  2  5  6  8  9 10 18 19   ttl 63     no effect
slice  3  4  7 12 13 15 16 17         no frame   load-bearing for emission
slice 11                              ttl 65     <<< anomalous, see below
slice 14                              ttl 64     <<< THE DECREMENT
```

**Disabling slice 14 leaves the TTL at 64 — the value the peer sent, un-decremented — while the
frame still emits.** That is precisely A4's signature, and it is the first direct identification of
a MOD command's function on this chip.

⚠ **Slice 11 gives `ttl 65`, which no decrement can explain.** The peer sends 64 and nothing should
raise it. Candidates, untested: the IP header is shifted so `tcpdump` reads the wrong byte as TTL
(an insert/delete length change would do this); or slice 11 performs an edit whose removal
misaligns the header. **Do not record slice 11 as "increments the TTL"** — a misparse is the more
likely reading and it is cheap to settle by dumping the raw bytes rather than trusting tcpdump's
field decode.

### Why the sweep succeeded where command-guessing failed

Three hand-picked leave-one-out tests on `0x20`/`0xe0` steps changed nothing, because predicting
which CAM entry fires needs the frame's full 48-bit key and I was constraining two fields. Disabling
a whole slice needs no such prediction. **20 tests, no shortlist, and it did not depend on the
`{0x20, 0xe0}` hypothesis being right** — which matters, since the co-occurrence test had already
argued that hypothesis was wrong.

### Next

`tools/a4-narrow.sh 14` walks slice 14's valid entries with the single-register perturbation and
reports the **command byte** of the one that matters. That byte is A4's answer: the opcode for
`DECREMENT`. If it is neither `0x20` nor `0xe0`, the shortlist is falsified and the split hypothesis
needs re-deriving from a known-correct opcode.

## ★★★ A4 ANSWERED: the TTL decrement is command byte `0x05`, slice 14 slot 9

**2026-08-16.** Leave-one-out over every valid entry in slice 14, single-register perturbation, each
restored before the next:

```
slot 9  cmd 05 -> ttl 64    <<< the frame still emits, TTL NOT decremented
slot 6  cmd e0 -> ttl 63        no effect
slots 1,2,4,5,7,8,10,11,15,16,17-24 -> ttl 63   no effect
```

**Command byte `0x05` performs the TTL decrement**, and it is the first MOD command on this chip
whose function has been established by measurement rather than inference.

### ⛔ This falsifies the `{0x20, 0xe0}` shortlist

`0x05` is neither. And `0xe0` was tested **in the same slice, on the same frame** (slot 6) and did
nothing. The route-enrichment statistics that produced that shortlist were sound as far as they went
— `0x20` and `0xe0` really are route-associated at p ≈ 1.2e-04 — but route-association is not
function. Two commands can both appear mostly on routed frames without either being the TTL edit.

### ⚠ And it is in tension with the existing decode

`mod_decode.py` infers that the operand-5 commands `0x05`/`0x85` are the **MAC rewrite**, reasoning
that under `opcode[7:5]:operand[4:0]` an operand of 5 means length 6, which is the MAC length, and
that the value banks supply a literal system MAC. That reading is supported by slices 3 and 4 —
which carry `0x85`/`0x05` and whose removal stops the frame entirely, exactly as losing the MAC
rewrite should.

**But the same byte `0x05` in slice 14 slot 9 decrements the TTL**, which is a one-byte edit.

Both cannot be true of a self-contained opcode. The possibilities, none yet tested:

1. **The command byte is not self-describing.** What a step does depends on its **value operands**,
   which come from the *data* slices (17-31) via `MOD_VALUE_RAM`, not from the command slice. Then
   `0x05` names an operation shape — "replace 6 bytes" — and the operands decide *which* 6 bytes and
   with what. IPv4 offsets 8-13 are TTL, Protocol, Checksum and two bytes of source address, so a
   6-byte replace starting at offset 8 would cover the TTL and the checksum together.
2. The `opcode[7:5]:operand[4:0]` split is wrong, and `0x05` is a different opcode entirely.
3. Slices 3/4 stop the frame for some reason other than the MAC rewrite.

⚠ **Reading (1) would mean a MOD generator cannot be written from the command bytes alone** — it
needs the paired value operands, which is precisely the coupling `mod_decode.py` flagged when it
found that command and data live in different slices. That is a bigger constraint on A4's generator
than the opcode split ever was.

### The method that got here

Three hand-picked leave-one-out tests on shortlist candidates changed nothing, because predicting
which CAM entry fires needs the frame's full 48-bit key. **Disabling a whole slice needs no such
prediction**, and narrowing inside the guilty slice with single-register perturbations needs none
either — the entry that fires is the one whose removal changes the wire. 20 + 20 tests, no
hypothesis required, and the answer contradicted the hypothesis we had.

## ★★★ MOD is a POSITIONAL byte-edit stream — proved by disabling slice 11

**2026-08-16.** Slice 11's anomalous `ttl 65` is not a tcpdump misparse. Raw egress bytes:

```
baseline            4500 0054 e28f 4000 3f01 de2c 0a65 6521 0a66 0101
slice 11 disabled   4500 0054 faa9 3f00 4101 c512 0a65 6521 0a66 0101
                                       ^^^^ ^^
IP header offsets      0    2    4    6    8   10
```

- **Baseline**: byte 6-7 = `40 00` (flags, DF set), byte 8 = `3f` — TTL 63, correctly decremented.
- **Slice 11 disabled**: byte 6 = **`3f`** — the *flags* byte has been decremented from `0x40` —
  and byte 8 reads `41`.

**The edit landed two bytes early.** The chip wrote to the wrong offset; `tcpdump` reported it
faithfully. So `ttl 65` was never an increment, and the earlier caution against recording it as one
was right for the wrong reason.

### What this establishes

**The MOD program is a positional byte-edit stream, and position is carried by the preceding
commands.** Removing slice 11 removed something that advances the write position — a `SKIP`, or the
length contribution of an edit — and every later edit shifted by two bytes. That is exactly the
datasheet's model (§5.21.5: SKIP/INSERT/DELETE/REPLACE "consumed as a stream by a per-port unit")
now demonstrated on hardware rather than read.

### It also resolves the `0x05` contradiction

`0x05` decrements the TTL in slice 14 and looks like the MAC rewrite in slices 3/4. Under a
positional stream both are the same operation: **an edit of length 6 applied wherever the stream has
reached.** Under `opcode[7:5]:operand[4:0]`, operand 5 means length 6.

- In slices 3/4 the position is the L2 header, so a 6-byte edit is the **MAC**.
- In slice 14 the position is IP offset 8, where 6 bytes span **TTL, Protocol and Checksum** —
  which explains why disabling that one step stopped the decrement *and* left no `bad cksum`: the
  TTL and its checksum are repaired by the same edit.

The command byte therefore gives an operation **shape**, not a target. **A MOD generator must track
the write position across the whole stream**, which is a stronger requirement than decoding the
opcode split — and it is why command and value slices being separate matters so much.

⚠ Not yet established: which command supplies the two-byte advance that slice 11 contributes, and
whether the advance is a `SKIP` or the length of an edit. The same slice sweep answers it, applied
to the value banks.

## MOD_VALUE_RAM sweep: the TTL decrement is NOT operand-driven

**2026-08-16.** First perturbation of the operand side — 14 banks from data slices 17-30 plus slice
31 → bank 14. Zeroing a bank sets every operand's `Type` to 0, supplying no value bytes. Compared on
the full IP header hex with the IP ID and checksum masked (they change every packet; an exact match
reported bank 0 as "changed" when only those moved).

```
banks 0, 1, 4, 5, 6, 7, 8, 9    unchanged
banks 2, 3                      NO FRAME  (both recovered)
bank 10                         NO FRAME  (wedged -- RESUME_FROM=11)
banks 11-14                     not yet run
```

**No bank produced a partial change.** Every effect is all-or-nothing: either the frame is unaffected
or it does not emit. Nothing altered the header while leaving the frame valid.

### ⛔ A prediction of mine, recorded beforehand and refuted

I argued that since a decremented TTL is derived from the incoming frame, it must be channel-sourced,
and that banks **7 and 9** — the only banks with *no* constants, every operand `Type=2` (data from a
parser channel) — should therefore drive it. **Both are unchanged.**

The premise was the error: the TTL value does not come from `MOD_VALUE_RAM` at all.

### What survives, and it is the datasheet's own reading

`DECREMENT` is documented with **0 value bytes**. It *transforms* a byte already in the frame rather
than writing supplied content, so no operand bank can affect it — which is exactly what the sweep
shows. The banks that do matter (2, 3, 10) are all constant-bearing, consistent with supplying
literal content for rewrites the frame cannot be emitted without, and matching `mod_decode.py`'s
finding that banks 3 and 4 hold a literal system MAC.

### Why this helps A4's generator

It splits the problem along a clean line:

- **Transform-type commands** (`DECREMENT`, and by extension the checksum fixup) need **no operand
  modelling at all**. The command and the write position are sufficient.
- **Content-writing commands** (`INSERT`, `REPLACE`, the MAC rewrite) need the paired value bank.

So the coupling between command and data slices — which looked like it might make a generator
intractable — only binds for the second class. That is a materially smaller problem than "a generator
cannot be written from command bytes alone".

⚠ Banks 11-14 are untested. And bank 4 came back *unchanged* despite `mod_decode.py` associating
banks 3 and 4 with the MAC rewrite; the likely explanation is that bank 4 serves a frame path our
routed IPv4 unicast does not take, but that is untested.

### Value-bank sweep COMPLETE — all 15 banks, no partial changes anywhere

```
unchanged   banks 0, 1, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14      (12 banks)
NO FRAME    banks 2, 3, 10                                     (3 banks)
partial     none
```

**Not one bank altered the header while leaving the frame valid.** Every effect is all-or-nothing:
either the frame is unaffected, or it cannot be emitted. That includes bank 14, which is driven by
CAM slice 31 rather than the 17-30 range and was the most likely place for an exception to hide.

### The operand question is closed

`MOD_VALUE_RAM` supplies **content** for edits that write new bytes, and nothing else. Specifically:

- **Transform commands** — `DECREMENT`, and by extension the checksum fixup that rides with it —
  take their input from the frame and need **no operand data**. Zeroing every value bank in turn
  never once stopped the TTL decrementing. That is the datasheet's own reading (`DECREMENT`: 0 value
  bytes) confirmed by exhaustive measurement rather than inference.
- **Content commands** — `INSERT`, `REPLACE`, the MAC rewrite — need their paired bank, and removing
  it makes the frame unemittable rather than merely wrong. The three banks that matter (2, 3, 10)
  are all constant-bearing, consistent with `mod_decode.py` finding a literal system MAC in the
  value banks.

### What A4's generator therefore has to reproduce

```
transform commands   command byte + write position                   (no operands)
content commands     command byte + write position + operand bank
```

The command/data slice coupling that looked like it might make a generator intractable **binds only
for the second class**. Combined with the positional-stream result, the model a generator needs is:
maintain a write position across the stream, emit an operation shape per step, and attach operand
data only for the content-writing shapes.

⚠ Still untested: bank 4 came back unchanged despite `mod_decode.py` associating banks 3 **and** 4
with the MAC rewrite. The likely reading is that bank 4 serves a frame path our routed IPv4 unicast
does not take — the CAM selects per frame — but nobody has checked. It is the one loose thread in an
otherwise complete sweep.

### ⚠ `0x01` is a 2-byte SKIP — which puts the `0x05` result in doubt

**2026-08-16.** Narrowing slice 11: **slot 0, command byte `0x01`**, and its removal reproduces the
whole-slice signature exactly:

```
baseline            4500 0054 4000 3f01 0a65 6521
slot 0 (0x01) off   4500 0054 3f00 4101 0a65 6521
```

Under `opcode[7:5]:operand[4:0]`, `0x01` is opcode 0 operand 1, i.e. **count = operand + 1 = 2** — a
2-byte `SKIP`. That matches `mod_decode.py`'s guess that "SKIP almost certainly being 0", and it is
the first confirmation of the opcode-0 assignment.

⚠ **And it undermines the claim that `0x05` is `DECREMENT`.** If opcode 0 is `SKIP`, then `0x05` is
opcode 0 operand 5 = **`SKIP` 6**, not a transform. Removing a 6-byte skip would displace every
later edit by six bytes, and the TTL byte could then read 64 simply because the decrement landed
somewhere else entirely.

**`a4-narrow.sh` only grepped `ttl N`. It never dumped raw bytes**, so it cannot tell those apart:

```
"0x05 decrements the TTL"                     -> TTL 64 because no decrement happened
"0x05 skips 6, and removing it moved the edit" -> TTL 64 because the decrement hit another byte
```

Both produce `ttl 64`. The distinguishing evidence is whether **other header bytes moved**, which the
tool discarded.

**Re-test required**: disable slice 14 slot 9 and dump the raw header. If bytes elsewhere changed,
`0x05` is a skip and the A4 answer is wrong as recorded. If only the TTL differs and everything else
is byte-identical, the decrement claim stands.

This is the same instrument failure as reading `ttl 65` and inferring an increment: **a field decode
hides a shift.** Once known, it should have been applied to every earlier result taken with the same
tool — and it was not.

### ★ RE-TEST: `0x05` is neither a pure DECREMENT nor a SKIP

**2026-08-16.** Disabling slice 14 slot 9 (`0x1591c9`, command `0x05`) and reading **raw bytes**
rather than the parsed TTL:

```
baseline   4500 0054 b0f4 4000 3f01 0fc8 0a65 6521
disabled   4500 ff54 bc8d 4000 4001 042e 0a65 6521
                ^^                ^^
```

(bytes 4-5 are the IP ID and vary per packet; not comparable.)

**Two fields change, not one:**

- **byte 8: `3f` → `40`** — the TTL is no longer decremented.
- **byte 2: `00` → `ff`** — the IP **total-length** high byte is corrupted.

Source and destination addresses and the flags field are **unchanged**, so **nothing shifted**.

**That refutes both candidate readings:**

- ⛔ **Not a pure `DECREMENT`.** A transform of one byte cannot also change the length field. The
  A4 answer as recorded — "command byte `0x05` decrements the TTL" — is **too simple** and must be
  qualified: this step is *responsible for* the decrement, but it does more.
- ⛔ **Not `SKIP 6`.** A skip's removal displaces everything downstream; src and dst are byte-identical
  and the flags field is untouched. The `0x01` = `SKIP 2` result stands on its own evidence, but it
  does not generalise to `0x05` the way I feared.

**What it looks like instead:** a content edit that writes several header bytes, among them the
total-length high byte and the TTL. Under `opcode[7:5]:operand[4:0]` operand 5 means length 6, and a
6-byte write reaching both offset 2 and offset 8 is not contiguous — so either the operand is not a
byte count here, or the edit is not a single contiguous span.

⚠ **And it is in tension with the value-bank sweep.** If `0x05` writes content, that content should
come from `MOD_VALUE_RAM` — yet zeroing all 15 banks never once stopped the TTL decrementing. Either
the bytes come from somewhere else (a parser channel reaching the MOD engine by another path), or
the sweep's zeroing did not disable the operands the way it was assumed to.

**Do not record an opcode meaning for `0x05` yet.** Three readings have now been tried and refuted
(`DECREMENT`, `SKIP 6`, and before them the `{0x20, 0xe0}` pairing). What is established is narrower
and worth keeping: **slice 14 slot 9 is the step responsible for the TTL decrement**, found by
perturbation, and whatever it does it also writes the length field.

## ★★★ `0x05` IS `SKIP 6` — and opcode 0 = SKIP is now confirmed three ways

**2026-08-16.** The size test settles it. Slice 14 slot 9 (`0x1591c9`, command `0x05`), three payload
sizes, enabled and disabled:

```
            correct        ENABLED          DISABLED
-s 56       0054  ttl 3f   4500 0054 3f01   4500 ff54 4001
-s 400      01ac  ttl 3f   4500 01ac 3f01   4500 00ac 4001
-s 1000     0404  ttl 3f   4500 0404 3f01   4500 0304 4001
```

**With the step enabled**, length and TTL are both correct at every size — a working router.

**With it disabled**, the length *high byte is exactly one less* than it should be
(`00→ff`, `01→00`, `04→03`) and the TTL is **not** decremented.

**The decrement did not disappear — it moved.** From offset 8 (TTL) to offset 2 (length high byte).
`8 - 2 = 6`.

> **`0x05` is `SKIP 6`.** Opcode 0, operand 5, count = operand + 1 = 6 — the same encoding as
> `0x01` = `SKIP 2`. Removing the skip leaves the following single-byte `DECREMENT` landing six
> bytes earlier.

### Opcode 0 = SKIP, on three independent measurements

| evidence | result |
|---|---|
| `0x01` removed, slice 11 | later edit lands **2** bytes early |
| `0x05` removed, slice 14 | later edit lands **6** bytes early |
| count = operand + 1 | holds for both, and matches the datasheet's SKIP range |

### ⛔ Correcting the record, twice

1. **"Command byte `0x05` decrements the TTL" was wrong.** It is a skip; the decrement is a
   *different, later* step that this skip positions. The A4 entry recorded earlier today must be read
   as **"slice 14 slot 9 positions the decrement"**, not as identifying `DECREMENT`.
2. **My reason for dismissing `SKIP 6` was also wrong.** I argued that a skip's removal would displace
   everything downstream, and since src, dst and flags were byte-identical, it could not be a skip.
   **A `SKIP` before a single-byte transform displaces nothing** — it relocates one edit. Whole-frame
   displacement only happens for `INSERT`/`DELETE`, which change length. I tested for the wrong
   signature and drew a confident conclusion from its absence.

### What this leaves

- **`DECREMENT` is still unidentified.** It is a step *after* slice 14 slot 9 in the stream. The same
  perturbation method will find it: it is the entry whose removal stops the TTL changing **without**
  relocating the edit.
- The value-bank tension **dissolves**: a `SKIP` needs no operands, and the decrement is a transform
  which the datasheet gives 0 value bytes. Both agree with the sweep finding no bank affects the TTL.
- `mod_decode.py`'s reading of `0x05`/`0x85` as the **MAC rewrite** is now doubtful for the same
  reason my claim was — operand 5 means *skip 6*, not *write 6*. Slices 3/4 stopping the frame needs
  re-attributing to other commands in those slices.
