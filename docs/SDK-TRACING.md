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

**Next:** resolve the third call's target — it uses a different accessor and is the one write in
this function not yet attributed.

## Practical notes

- Symbols: `nm -D -S --defined-only` gives address **and size**, which is what you need to bound
  `objdump --start-address/--stop-address` to one function.
- `movl $0x80000,(%esp)` and `movl $0x2f32,0x18(%esp)` patterns are logging arguments (a category
  mask and a string offset), not register work. They are dense in this library and easy to mistake
  for data.
