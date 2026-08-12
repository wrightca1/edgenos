# Tracing register writes in libFocalpointSDK.so

**2026-08-12.** How to find which chip registers an SDK function writes. Written after a failed
attempt taught the wrong way to do it.

## ⚠ What does NOT work: grepping immediates

The obvious approach — disassemble a function, grep for large immediates, treat them as register
addresses — produces confident false results. In `fm6000DbgDumpPortMasks`:

```
42e9b7:  add  $0xfffffff8,%eax
42e9ba:  adc  $0xffffffff,%edx     <- 64-bit arithmetic
42e9c8:  add  $0x68000,%eax
```

`0x68000` reads like a register address inside the L2L block. It is a **struct offset**; the `adc`
beside it gives it away. Dumping `0x68000` on a live chip returned 4,096 words of zero.

Two more traps in the same family:

- `call 2318f9 <LBGPostamble@plt+0xe1>` appears at the top of nearly every function. It is the i386
  **PIC thunk** (`__x86.get_pc_thunk`), always followed by `add $imm,%ebx` to set up the GOT. Not a
  call worth following.
- Searching the whole `.text` for the L2F base immediates (`0x180000`/`0x1a0000`) returns two
  functions, neither of which programs L2F. Base addresses are usually *computed*, not literal.

## ★ What works: the CSR accessor is a function pointer in the switch struct

Register access goes through function pointers held at fixed offsets in the switch context struct.
The calling pattern is mechanical and greppable:

```
mov  -0x38(%ebp),%eax          ; switch context
mov  0x3cc54(%eax),%ecx        ; load the CSR accessor
movl $0x1f080,0x4(%esp)        ; REGISTER ADDRESS, as an argument
movl $<value>,0x8(%esp)        ; value
call *%ecx                     ; the write
```

Accessor offsets, by frequency across the library:

| offset | uses | role |
|---|---:|---|
| `0x3cc54` | 546 | write32 |
| `0x3cc58` | 393 | read32 |
| `0x3cc78` | 85 | |
| `0x3cc70` | 81 | |
| `0x3cc6c` | 71 | wider / burst accessor |
| `0x3cc74` | 70 | write64 (passes value as two halves) |
| `0x3cc5c` | 26 | |

**Method:** find `mov 0x3cc5[48]\(%e..\),%e..` in the function, then read forward to the `call *%reg`.
The register address is either an immediate argument or a value computed just above it (look for
`add $BASE,%eax` where BASE matches a `FM6000_*_BASE` from the register header).

Verified against a known answer: `fm6000CrmSetMemoryExt` writes `0x1f080`, `0x1f100/1`, `0x1f200`,
`0x1f180`, `0x1f000` — exactly the CRM Memory-Set descriptor phase 75 established by other means.

## First results from applying it

### VLAN membership never touches hardware

`fm6000SetVlanMembership`, `fm6000AddVlanPortList` and `fm6000CreateVlan` contain **zero** CSR
accessor uses, as does the internal helper at `0x49f967` they share. They manipulate software state
only; something else pushes it to the chip. That is a structural fact worth knowing before hunting
for "the register VLAN membership lives in" — there isn't one on that path.

### `fm6000UpdatePortMask` writes the LBS block

The function that *is* named for the job makes exactly three CSR calls:

```
42e160  mov 0x3cc54(%eax),%ecx ; add $0x14000,%eax ; call *%ecx     write32 -> LBS_BASE
42e344  mov 0x3cc54(%eax),%ecx ; add $0x14000,%eax ; call *%ecx     write32 -> LBS_BASE
42e486  mov 0x3cc6c(%eax),%edx ;                    call *%edx      wider accessor, target TBD
```

`0x14000` is `FM6000_LBS_BASE` — loopback suppression. Notably `LBS_CAM` was one of only four
per-port tables that differed between working Et1 (port 40, `0x0001fffe`) and port 41
(`0x0003fffc`) in the port-3 investigation.

### The third call writes L2F_TABLE_256 — and corrects my earlier dismissal

Following the arithmetic all the way through rather than stopping at the immediate:

```
mov  0x23e70(%eax),%eax        ; struct pointer
add  $0xb30,%eax
mov  0x4(%eax),%edx            ; 64-bit value
mov  (%eax),%eax
add  $0xfffffff8,%eax          ; minus 8
adc  $0xffffffff,%edx
shl  $0x8,%edx                 ; group << 8
lea  (%edx,%eax,1),%eax        ; + entry index
add  $0x68000,%eax             ; + base, IN ENTRY UNITS
shl  $0x2,%eax                 ; << 2  ->  0x68000 * 4 = 0x1A0000
movl $0x3,0x8(%esp)            ; count = 3 words
call *%edx                     ; multi-word write via 0x3cc6c
```

`0x1A0000` is **`L2F_TABLE_256`**, and `count = 3` is exactly `FM6000_L2F_TABLE_256_WIDTH`.

⚠ **This corrects the "0x68000 is a struct offset" conclusion above.** It *is* a register address —
the SDK addresses this table in **entry units** and shifts left by 2 to get words. Dumping
`0x68000` returned zeros because the table is at `0x1a0000`, not because the immediate was
meaningless. The `adc` I took as proof of struct arithmetic belongs to an unrelated 64-bit
subtraction three instructions earlier.

The lesson survives in a sharper form: **an immediate is only meaningful once you follow it to the
accessor call.** Neither "it's big so it's an address" nor "there's an adc so it isn't" is sound;
the deciding evidence is what reaches the `call *%reg`.

### So the SDK's port-mask update touches two tables

`fm6000UpdatePortMask` writes:

1. **`LBS_BASE` (`0x14000`)**, twice, via write32 — loopback suppression, per port
2. **`L2F_TABLE_256` (`0x1a0000`)**, one 3-word entry — the destination/port mask

That independently confirms what the port-3 work found empirically: the destination mask lives in
`L2F_TABLE_256` and is a 3-word atomic entry. It also says adding a port to a forwarding domain is
**two** operations, not one — the mask *and* loopback suppression.

⚠ And it flags an error in the port-3 experiments: `LBS_CAM` was set on port 41 by **copying port
40's value verbatim** (`0x0001fffe`). LBS is per-port — port 41's own value was `0x0003fffc`,
which is port 40's shifted left by one. Copying it was almost certainly wrong, and the SDK computing
it per port confirms the value is positional rather than global.

## Practical notes

- Symbols: `nm -D -S --defined-only` gives address **and size**, which is what you need to bound
  `objdump --start-address/--stop-address` to one function.
- `movl $0x80000,(%esp)` and `movl $0x2f32,0x18(%esp)` patterns are logging arguments (a category
  mask and a string offset), not register work. They are dense in this library and easy to mistake
  for data.


## The call path into the port mask

`fm6000UpdatePortMask` has six call sites. Three resolve to **`fm6000SetPortAttributeInt`**
(`+0x796`, `+0x9ed`, `+0x1c77`); three are in an unnamed local function around `0x3d1acd`.

The sequence immediately before each call is the informative part:

```
41935d:  call fmBitArrayToPortMask@plt   ; port LIST -> port MASK
419362:  mov  %eax,-0x1c(%ebp)
419365:  cmpl $0x0,-0x1c(%ebp)           ; error check
419369:  jne  <error>
41937c:  call fm6000UpdatePortMask@plt
```

So the SDK's route to a hardware port mask is: **set a port attribute whose value is a port list**,
which `fmBitArrayToPortMask` converts to a mask, which `fm6000UpdatePortMask` then writes into
`L2F_TABLE_256` (3-word entry) and `LBS_BASE`.

That is the API-level answer to "how does a port get added to a forwarding domain": not through
`fm6000SetVlanMembership` (which touches no hardware), but through a **port attribute** carrying a
port list.

**Next:** `fm6000SetPortAttributeInt` is ~68 KB and dispatches on the attribute ID through a jump
table that is not in its first 0x400 bytes. Locating that table gives the specific attribute IDs
for these three cases — i.e. exactly which attribute to set. That is the next step, and it is
bounded: find the `jmp *table(,%reg,4)`, read the table, map the three call-site offsets back to
their case indices.


## ★ The answer: `FM_PORT_MASK_WIDE`

Read the attribute dispatch table to name the three cases that write the port mask.

**The dispatch**, at `0x419287`:

```
cmpl $0x85,0x18(%ebp)              ; attribute ID, valid 0..0x85
ja   <default>
mov  0x18(%ebp),%eax
shl  $0x2,%eax
mov  -0xb5e38(%eax,%ebx,1),%eax    ; table[attr], PIC-relative offsets
add  %ebx,%eax
jmp  *%eax
```

PIC base is `0x418bf7 + 0x1eb615 = 0x60420c`, so the table sits at `0x54e3d4` and each entry is an
offset from that base. Mapping the three `fm6000UpdatePortMask` call sites back to their cases:

| call site | attribute | name |
|---|---|---|
| `+0x796` | `0x16` (22) | **`FM_PORT_MASK_WIDE`** |
| `+0x9ed` | `0x5a` (90) | `FM_PORT_INTERNAL` |
| `+0x1c77` | `0x59` (89) | `FM_PORT_LOOPBACK_SUPPRESSION` |

**The attribute name table** is a 28-byte record array at `0x408d40` (file offset), each record
holding the attribute ID followed by a pointer to its `FM_PORT_*` string. 81 records parse cleanly,
and the low IDs check out against the published FocalPoint ordering — `MIN_FRAME_SIZE=0`,
`MAX_FRAME_SIZE=1`, `LEARNING=6`, `TAGGING=7`, `SPEED=20`.

### What this means

**`FM_PORT_MASK_WIDE` is the forwarding-domain membership.** Setting it on a port takes a port
*list*, converts it with `fmBitArrayToPortMask`, and writes a 3-word entry into `L2F_TABLE_256`.
That is the operation the entire port-3 investigation was looking for and never found, because it
looked for it under "VLAN membership" — which on this SDK writes nothing.

It also matches the shape of the masks EOS holds: `{0,41}` and `{0,3,20,40,41}` are exactly
"which ports may this source port forward to", not flood groups as I had assumed.

⚠ And `FM_PORT_LOOPBACK_SUPPRESSION` being a *separate attribute* writing `LBS_BASE` confirms
these are two independent operations — and that copying port 40's `LBS_CAM` value onto port 41 in
the port-3 experiments was wrong, since the SDK derives it per port.

### Next

For Et1 (port 40) to forward to port 41, **port 40's `FM_PORT_MASK_WIDE` must include bit 41**. The
remaining unknown is the index computation into `L2F_TABLE_256`:

```
((value_at[ptr+0xb30] - 8) << 8) + entry_index + 0x68000, then << 2
```

The `<<8` group term comes from a switch-struct field, so which `L2F` entry corresponds to which
source port still has to be pinned — most cheaply by reading `L2F` on EOS with both ports up and
correlating, which is a dump we already have.


## `fm6000InitPort`: what EOS does, in order, to bring a port up

Traced end to end. Stripping logging, the whole function is:

```
fmPortMaskEnableAll(mask, n)      x2      build an all-ports bitmask in software
memset / fmLoadDynamicLoadLibrary
fm6000GetPortMacCount
fm6000SetPortAttribute(... 0x9f ...)      Arista extension, unnamed
fm6000SetPortAttribute(... 0x07 ...)      FM_PORT_TAGGING
fm6000SetPortAttribute(... 0x08 ...)      FM_PORT_TAGGING2
fm6000SetPortAttribute(... 0x30 ...)      FM_PORT_SWPRI_SOURCE
fm6000SetPortAttribute(... 0x8b ...)      Arista extension, unnamed
fm6000SetPortAttribute(... 0x8c ...)      Arista extension, unnamed
```

`fmPortMaskEnableAll` is a pure software helper — it fills a 3-word (96-bit) mask with the low
`n` bits set, looping words 0..2. It produces the port *list* that `fmBitArrayToPortMask` later
converts.

### Two things this settles

**`fm6000InitPort` does not set `FM_PORT_MASK_WIDE`.** Port bring-up and forwarding-mask membership
are separate operations in this SDK; the mask is set by whoever owns the forwarding domain, not by
port init. So "bring the port up" was never going to make it forward, which is consistent with
everything the port-3 work observed.

**Three of the six attributes are outside the stock FocalPoint set.** The name table at
`0x408d40` holds 81 records topping out at `0x85` (`FM_PORT_PARSE_L3_QinQinQ`), and
`fm6000SetPortAttributeInt`'s dispatch bounds at the same `0x85`. IDs `0x8b`, `0x8c` and `0x9f` are
therefore **Arista extensions** handled before or outside that dispatch — three attributes EOS sets
on every port that have no public name and no entry in the table.

### Next thread

Those three unnamed attributes are the most interesting thing found so far: they are set by EOS on
every port, they are not part of the documented API, and they are not in the dispatch table that
handles everything else. Finding where `fm6000SetPortAttribute` (the outer wrapper, distinct from
`...Int`) routes IDs above `0x85` would name them — and they are the only remaining candidates for
per-port state EOS configures that we have never replicated.


## The three unnamed attributes: resolved, and they are not the answer

`fm6000SetPortAttribute` bounds its own dispatch at `attr - 0x21 <= 0x5a` (attributes `0x21`-`0x7b`)
and sends everything else down a default path that ends at `fm6000SetPortAttributeInt`, whose
dispatch bounds at `0x85`. So where do `0x8b`, `0x8c` and `0x9f` go? `SetPortAttributeInt`'s
default path answers it:

```
call fmLoadDynamicLoadLibrary@plt   ; load a plugin
cmpl $0x0,-0x1c(%ebp)               ; check the handle
mov  0x18(%ebp),%eax                ; the attribute id
call *%edx                          ; dispatch INTO the loaded library
```

**Attributes above `0x85` are handled by a dynamically loaded library**, named in the SDK's strings
as `libFocalPoint_AWM_switch.so` and present in the EOS rootfs. It exports `fm6000SetUcPortAttribute`
and carries its own name table (28-byte records, same layout as the main one).

The two ID spaces **dovetail rather than collide** — the extension defines `FM_PORT_DEF_VLAN = 0x09`,
filling a gap the main table leaves — so the two libraries split one enum.

Resolving `fm6000InitPort`'s three unknowns there:

```
0x8b (139) = FM_PORT_PARSE_PAUSE
0x8c (140) = FM_PORT_PARSE_CBP_PAUSE
0x9f (159) = not among the 35 records parsed
```

### The honest conclusion

**They are pause-frame parsing, not forwarding state.** The thread was worth pulling — it was the
last unexamined thing in the port bring-up path — but it does not contain the missing piece. The
full picture of `fm6000InitPort` is now: build an all-ports software mask, set VLAN tagging, switch
priority source, and pause parsing. Nothing that grants a port membership of a forwarding domain.

Combined with the earlier finding that `fm6000InitPort` never sets `FM_PORT_MASK_WIDE`, the
conclusion is consistent and worth stating plainly: **in this SDK, bringing a port up and making it
forward are entirely separate concerns, and nothing in the port-init path touches the second.**

The extension library is nonetheless a find in its own right. `FM_PORT_DEF_VLAN`,
`FM_PORT_SWITCHING_REFLECT`, `FM_PORT_ROUTING_REFLECT` and `FM_PORT_MLAG_PEER_LINK` all live there,
none appear in the main SDK, and `fm6000SetUcVlanAttribute` / `fm6000SetUcSwitchAttribute` are the
VLAN and switch equivalents — which is where the forwarding-domain configuration most plausibly
lives, and which no part of this project has looked at before.


## Port bring-up, mapped completely — and it never touches the forwarding mask

`fm6000UpdatePortMask` has exactly six call sites. All six are now attributed:

| call site | reached from | trigger |
|---|---|---|
| `+0x796` | `fm6000SetPortAttributeInt` | `FM_PORT_MASK_WIDE` |
| `+0x9ed` | `fm6000SetPortAttributeInt` | `FM_PORT_INTERNAL` |
| `+0x1c77` | `fm6000SetPortAttributeInt` | `FM_PORT_LOOPBACK_SUPPRESSION` |
| `0x3d1acd` | static helper | LAG membership |
| `0x3d1ddc` | static helper | LAG membership |
| `0x3d1e5e` | static helper | LAG membership |

The three static call sites sit in unnamed helpers in the `0x3d1xxx` region whose only callers are
`fm6000AddPortToLag`, `fm6000DeletePortFromLag` and `fm6000SetPortLAGConfig`. They call
`fmIsCardinalPort`, `fmGetLAGMemberPorts` and `fm6000UpdatePortMask` under `fmCaptureLock` /
`fmReleaseLock` — a recompute-the-masks routine for link aggregation.

### The conclusion

**A port mask is written on exactly two occasions: an explicit attribute set, or a LAG membership
change. Nothing else.** In particular:

- `fm6000InitPort` does not write one (traced end to end: an all-ports software mask, VLAN tagging,
  switch priority source, pause parsing).
- **There is no link-state path.** No handler recomputes masks when a port comes up. The presence
  of `fmPortMaskLogicalToLinkUpMask` suggested one might exist; it does not — that helper filters a
  mask by link state for a caller that already has one, it is not a trigger.

So "bring the port up and it joins the forwarding domain" is not how this SDK works, and no amount
of port-level configuration will make port 41 forward. Membership is **explicit configuration
performed by whatever owns the VLAN**, and in EOS that is an agent calling into
`libFocalPoint_AWM_switch.so`.

That closes the port bring-up line of enquiry with a definite answer rather than another
"necessary but not sufficient". The remaining target is `fm6000SetUcVlanAttribute` in the extension
library — the VLAN-side equivalent, in the binary this project has never opened.
