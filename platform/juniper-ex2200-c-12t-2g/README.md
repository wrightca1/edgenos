# platform/juniper-ex2200-c-12t-2g

Juniper EX2200-C-12T-2G: 12x1G copper + 2 combo uplinks (RJ45 or SFP),
Marvell Kirkwood 88F6281 host, Marvell Prestera DX (Cheetah-3) switch ASIC.

**Status: planned.** Nothing here has been compiled or booted yet.

## What exists

| File | State |
|---|---|
| `board.yml` | board manifest, populated from hardware |
| `dts/kirkwood-ex2200-c-12t-2g.dts` | **first draft, not compiled, not booted** |
| `config/`, `services/` | empty, awaiting phase 1 |

## Where the hardware knowledge lives

Everything here derives from the `junos-ex2200-static-analysis` repo, which
reverse-engineered the running Junos 12.3R12-S14 on this exact unit. Start with
its `docs/PORT-CHECKLIST.md` - it carries every recovered constant in the order
a port needs them.

Load-bearing findings:

| Topic | Finding |
|---|---|
| Confirmed memory map, live boot | 16 |
| SysPLD register table, reset map | 22 |
| Board watchdog (disarmed at power-on) | 26 |
| `chassism` board-support surface, dual-bank flash | 29 |
| `/dev/mpfe` kernel contract | 30 |
| CPSS platform contract (15-slot vtable) | 33, 34 |
| SFP combo uplinks | 36 |
| PHY layer and QSGMII lane grouping | 37 |
| **ASIC port map** | 38 |
| Boot flash regions, what needs backup | 45 |

## Bring-up, in short

1. Build a kernel with `mvebu_v5_defconfig` plus this DTS.
2. Interrupt autoboot (any character in a silent 1 s window; prompt `=>`).
3. `setenv ipaddr`/`serverip` **in RAM**, `tftpboot`, `bootelf`. No flash write.
4. Expect: console, `eth0` (the SoC MAC), USB root. **No switching.**

Power-cycling undoes anything at this stage.

## The dataplane

Not started, and it is the whole project. No open driver fits this Prestera
generation, and CPSS is licence-gated. The intended shape is a `cpssEnabler`-
derived kernel shim (GPLv2) with the platform layer above it implementing the
15-slot vtable recovered in finding 33.
