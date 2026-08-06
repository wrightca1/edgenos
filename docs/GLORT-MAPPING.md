# GLORT ↔ port mapping — why it isn't stable, and how to fix it

**2026-08-06.** Short answer: the hardware mapping *is* deterministic. What is unstable is that we
**inherited EOS's dynamic GLORT allocation** instead of choosing our own.

---

## Where the mapping lives

`PARSER_INIT_FIELDS[phys_port]` at **`0x108200 + 4*port`**, holding `(SGLORT << 16) | logical_id`.
This is the parser's per-port initial state: it stamps the source GLORT onto every frame entering
that physical port.

Extracted from the replay:

| phys port | address | value | SGLORT | logical id |
|---|---|---|---|---|
| **40** (Et1, EPL14 ch0) | `0x1082a0` | `0x03ef0101` | **`0x03ef`** | 257 |
| **20** (Et2, EPL16 ch0) | `0x108250` | `0x03ee0102` | **`0x03ee`** | 258 |
| 21 | `0x108254` | `0x00010104` | `0x0001` | 260 |
| 22 | `0x108258` | `0x00010106` | `0x0001` | 262 |
| …all other ports | | `0x0001….` | `0x0001` | various |

Only the two ports EOS had *configured up* got a real GLORT. Every other port keeps the default
`0x0001`. That is the whole story.

## Why it's unstable

`0x03ee` and `0x03ef` are **arbitrary numbers EOS allocated from its own GLORT pool at
configuration time.** They are not a function of the port. Configure the ports in a different order,
add or remove a port, or change features, and EOS hands out different values. Our replay froze one
particular allocation, so the F64 tag we build (`w1 = 0x03ef`) is only correct for *that* capture —
hence "read it from the trace, don't assume it."

Note the *logical id* is systematic (`0x101` for port 40, `0x102` for port 20, then `0x103`,
`0x104`…). It is the **GLORT specifically** that is arbitrary.

## The hardware side is deterministic

`GLORT_CAM` at `0x0e000` is a plain ternary CAM, `[31:16]` = Key, `[15:0]` = KeyInvert (match 1
where Key=1/Inv=0, 0 where Key=0/Inv=1, don't-care where both=1):

```
0x0e005 = 0x30ffcfff   ->  matches 0x30xx   (low byte don't-care)
0x0e006 = 0x31ffceff   ->  matches 0x31xx
0x0e007 = 0x32ffcdff   ->  matches 0x32xx
...
0x0e032 = 0x5dffa2ff   ->  matches 0x5dxx
```

Entry *i* matches GLORT block `0x30xx + i` — one 256-entry block per port, entirely predictable.
`GLORT_RAM` at `0x0e800 + 2*i` holds the corresponding DMask base.

So nothing in the silicon is unstable. We simply adopted someone else's numbering.

## How to make it stable

Own the allocation. Define GLORT as a pure function of the port and program every table that
references it:

```c
#define FM6000_GLORT_BASE   0x0300u
#define GLORT_FOR_PORT(p)   (FM6000_GLORT_BASE + (p))   /* deterministic, reproducible */
```

Then write, from our own code rather than the replay:

| table | address | contents |
|---|---|---|
| `PARSER_INIT_FIELDS` | `0x108200 + 4*port` | `(GLORT_FOR_PORT(port) << 16) \| logical_id` |
| GLORT-indexed L2L tables | `0x036000 + glort`, `0x037000 + glort` | seen at `0x0373ee`/`0x0363ee` |
| `0x032000`/`0x034000` maps | `+ 2*glort` | seen at `0x0327dc`/`0x0347dc` = `0x03ee` |
| `NEXTHOP` | `0x160000 +` | entries embed the GLORT (`0x03ef80a2`) |
| `GLORT_CAM` / `GLORT_RAM` | `0x0e000` / `0x0e800` | ternary key per port block |

and have `fm6000_txinline` / `fm6000_l3` build the F64 tag from `GLORT_FOR_PORT()` instead of a
hard-coded `0x03ef`.

**This is a subset of the "generate config instead of replaying EOS" work** in `PROVENANCE.md §4`
— and a good first slice of it, because the tables are small, the CAM format is understood, and it
is independently testable: program our own GLORT for Et1, send a tagged frame, confirm it still
egresses on Et1.
