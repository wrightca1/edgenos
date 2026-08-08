# FM6000 replay tooling

Tools for understanding `fwd4.txt` — EOS's cold-boot register trace — and for replacing
it, block by block, with code of our own. Status and method: `docs/SELF-CONTAINED-PLAN.md`.

## Analysis

| tool | what it answers |
|---|---|
| `replay_classify.py` | what kind of write each one is (config / microcode / SPICO firmware / fill) |
| `replay_structure.py` | loop and redundancy structure — 77% of MMIO is one 336-iteration loop |
| `spico_extract.py` | reconstructs the SerDes SPICO image from a trace; proves the trace carries it **byte-identically** |
| `replay_bisect.sh` | cold-boots the switch with a block removed, to measure what is load-bearing |

## Generators

`gen_tableinit.py` emits the `fm6000_*init.c` block generators. `--survey` reports each block's
shape and suggests a mode; it does **not** tell you whether a block can be collapsed — that is a
semantic question, see the docstring.

| tool | generated C | block |
|---|---|---|
| `gen_tableinit.py` | `fm6000_ffuinit.c`, `fm6000_l2linit.c`, `fm6000_l2finit.c`, `fm6000_eplinit.c` | FFU, L2L, L2F+LBS, EPL |
| `gen_cminit.py` | `fm6000_cminit.c` | CM (rows + port→class map, per the public datasheet) |
| `gen_sweepinit.py` | `fm6000_sweepinit.c` | the 336-iteration sweep — **gated off, breaks forwarding** |
| `fm6000_sweepgen.py` | — | proves the sweep is a port-map walk; `verify` is byte-exact |

`fm6000_safinit.c` is hand-written: four patterns over 56 ports, small enough to read.

## Gotchas that have already cost time

- **`gen_after` must run last.** The helpers find the loop by grepping the anchor `001a0c00`, and
  that address is itself an L2F register — so the L2F filter deletes it. Anything after L2F splices
  at an arbitrary point, silently.
- **Filter by address list, not prefix,** for blocks mixing table and control. The FFU commit strobe
  at `0x3f0000` fires 59 times; collapsing it applies no rules and OSPF never comes up.
- **awk compares hex addresses inconsistently.** All-digit strings (`00381880`) compare numerically,
  hex-letter ones (`003b1000`) lexicographically. Force string context (`x ""`) on both sides.
- **Only cold boots test forwarding.** Re-running `fm6000-fullseq.sh` in place gives `et1 rx=0` for
  a good replay and a bad one alike.
- **Always run a stock-replay control** at the same cadence. The dataplane collapse (100% loss,
  adjacency dropping to 2 routes) is pre-existing and will otherwise be blamed on your change.
