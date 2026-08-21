# Open-source references for FocalPoint SerDes bring-up

**2026-08-20.** Asked while stuck on port 3: is any of this available openly, rather than only
by disassembling `libFocalpointSDK.so`? **Substantially, yes** — and one source is directly
applicable and cleanly licensed.

## 1. The FM5000/FM6000 datasheet — public

`331496-002` rev 3.4 (July 2017) is published by Intel. We already work from it; it is the
authority for the L3AR/L2AR/MOD/parser architecture, and it is what settled the `SetFlags`
semantics and the TCAM/slice/mux structure. It does **not** document the SerDes/SBus internals.

## 2. DPDK's `net/fm10k` switch management — the useful one

DPDK carries a switch-management driver for the **FM10000**, the FocalPoint successor, including
EPL SerDes and SPICO handling: `fm10k_sbus_read`/`fm10k_sbus_write`,
`fm10k_load_epl_spico_code`, `fm10k_epl_serdes_start_bringup`, `fm10k_epl_serdes_start_dfe_ical`
/ `_pcal`. Same SerDes family, same SBus + SPICO architecture as the FM6000.

⚠ **It is the successor part, not ours.** Treat it as a guide to the *shape* of the sequence and
to what the SPICO interrupt codes mean, not as drop-in values. FM6000 register numbers and
SPICO codes must still be confirmed against our own hardware.

**Licence matters here and is favourable.** DPDK is BSD-3-Clause and EdgeNOS is
GPL-2.0-or-later, which is compatible — so unlike `libFocalpointSDK.so`, this is a reference we
may actually adapt rather than only read. See `PROVENANCE.md`; nothing changes about the EOS
rule.

### ★ What it tells us that the disassembly did not

**Bring-up is driven by SPICO interrupts, not by direct SerDes register writes.**
`fm10k_epl_serdes_start_bringup()` runs, in order:

| step | mechanism |
|---|---|
| 1 | SPICO reset, register `0x00` |
| 2 | firmware CRC check, interrupt `0x3c` |
| 3 | bit-rate divider, interrupt `0x05` (`0x42` for 10G, `0xa5` for 25G) |
| 4 | PCSL width mode, via switch registers |
| 5 | TX data select, interrupt `0x02` arg `0x1ff` |
| 6 | TX equalisation, three cursors, interrupt `0x15` |
| 7 | **enable TX/RX, interrupt `0x01` arg `0x03`** |

DFE is interrupt `0x0a` (arg 1 = iCal, 2 = pCal) polled with interrupt `0x126`, timeouts 3000 ms
and 2000 ms.

**That `0x03` is the same operation as FM6000's `fm6000SetTxConfig`**, whose third
read-modify-write ends `or 0x2; or 0x1` — enable TX and RX. Two independent sources agreeing on
the same two bits is the strongest cross-check we have on this path.

### Tried immediately, and insufficient on its own

    fm6000_sbus irq 0x4a 0x01 0x03      # enable TX/RX on port 3's SerDes
    -> interrupt accepted (resp reg 0x02 = 0x13), Et3 unchanged, et1/et2 undisturbed

Expected: in the DPDK order that is step 7 of 7. The preceding steps — bit rate, TX data select,
TX equalisation — have not been issued.

## 3. What this changes

The SerDes work has been trying to reconstruct an *algorithm* from a *capture*
(`fm6000_lanelink`) and then from disassembly (`fm6000_serdes_enable`). The open driver says the
algorithm is largely a sequence of SPICO interrupts, which is why neither approach reached a
link: the capture records register traffic, and the enable path we disassembled is only part of
the story.

`fm6000_sbus irq <target-dev> <code> [arg]` already implements the interrupt mechanism against
the SPICO broadcast device with reg `0x03` naming the target, and it is safe in practice —
`fm6000_lanelink` issues 274 such ops for port 3 without disturbing et1 or et2.

**Next:** work the DPDK sequence step by step against device `0x4a`, confirming each FM6000
interrupt code on hardware before relying on it.

## Tried: the DPDK sequence against FM6000 — negative, and the failure is informative

`fm6000_serdes_enable -i <port>` implements `fm10k_epl_serdes_start_bringup()`'s order against
our own hardware, issuing each step as DPDK does — write the SerDes device's register `0x03`
with `(code << 16) | param`, then poll register `0x04` bits 16-17 until clear:

    int  05 arg 9042 -> r04=00000022 after 0 ms  (bit rate, 10G divider 0x42)
    int  02 arg 01ff -> r04=00000022 after 0 ms  (tx data select)
    int  15 arg 4004 / 0001 / 8005                (tx eq atten / pre / post)
    int  11 arg 0003 -> ...                       (PLL calibration)
    int  2b arg 0001 / int 13 arg 0300            (rx termination / polarity)
    int  01 arg 0003 -> r04=00000022 after 0 ms   (enable TX and RX)
    after: PORT_STATUS=00000015  SerXmit=0        -- still dark; et1/et2 undisturbed

**Every poll "succeeded" in 0 ms, and that is the tell.** `r04` reads `0x22`, and bits 16-17 of
`0x22` are clear, so the busy test can never fail — it is vacuous. Checked directly: dumping all
64 SerDes registers on device `0x4a` yields 37 distinct values and **every one is 8 bits wide**.

So **FM6000 SerDes SBus registers are 8-bit**, where FM10000's are 32-bit. DPDK's encoding —
`(code << 16) | param` into one register, a 2-bit busy field at bits 16-17 of another — cannot
survive that: the interrupt code would be shifted clean out of an 8-bit register.

⚠ This does **not** retire the interrupt idea. It retires the FM10000 *encoding*. The FM6000
plainly has a SPICO interrupt path — `fm6000_sbus irq` exists and `fm6000_lanelink` issues 274
broadcast-device ops for a single port. What we do not have is FM6000's own interrupt register
layout, and the 8-bit register width means it must differ structurally (a code and its parameter
cannot share one register).

Corroborating the width from the other direction: FM6000's `fm6000EnableSerDes` treats reg `0x03`
as an ordinary control register (its step 11 sets bit 0), which it could not be if reg `0x03`
were FM10000's interrupt-request register.

**Where that leaves the open-source angle.** DPDK remains valuable for the *shape* of bring-up —
that it is bit-rate, then data select, then equalisation, then PLL, then termination, polarity
and enable, in that order — and for confirming the `0x03` = enable-TX-and-RX operation against
`fm6000SetTxConfig`. It is not a source of FM6000 register encodings, and this attempt is the
evidence for that rather than an assumption.

## ★ Resolved: FM6000 bring-up does NOT use SPICO interrupts

Every call to `fm6000InterruptSpico`/`V2` in `libFocalpointSDK.so` was located (9 sites across
`0x360000-0x4a0000`) and mapped to its enclosing function:

| code | called from | what it is |
|---|---|---|
| `0x02` | static helper at `~0x476ffe` | the interrupt machinery itself |
| `0x04` | `fm6000LoadSpicoCode` | firmware load |
| `0x20` | `fm6000GetSerDesDfeStatus`, `fm6000GetSerDesEyeScore`, `...V2` | DFE / eye **status query** |
| `0x03`, `0x0f` | `fm6000SetSpicoState` | SPICO state control |

The fourth argument is a microsecond timeout (`0x7a120` = 500 ms, `0x30d40` = 200 ms,
`0xc350` = 50 ms).

**`fm6000EnableSerDes`, `fm6000SetTxConfig` and `fm6000StartSerDesDfeTuning` contain no
interrupt calls at all.** On the FM6000, SPICO interrupts are used only to load firmware, to
drive SPICO state, and to *read back* DFE and eye status. Lane bring-up is done with **direct
SBus register writes** — which is exactly what those three functions do.

### What that settles

- The FM10000 model — bring-up as a sequence of SPICO interrupts (`0x05` bit rate, `0x15` tx eq,
  `0x01` enable TX/RX) — **is specific to the successor part**. It does not describe the FM6000.
  DPDK remains useful for the *order* of operations and as corroboration that "enable TX and RX"
  is the value `0x03`, and no further.
- The register path is the right one after all: `fm6000_serdes_enable`'s ten known RMW steps plus
  both waits are the correct approach, and what is missing is genuinely just the four undecoded
  steps (3-6, values computed by SDK arithmetic) and `fm6000SetTxConfig`'s three writes.
- The interrupt encoding recovered in `EOS-SOURCES.md` is still correct and still worth having --
  `fm6000_sbus irq` is now usable, and code `0x20` gives a real DFE/eye-score readout, which is a
  diagnostic we did not previously have for a lane that will not train.

**So the next step is not more interrupt work.** It is finishing the register path: decode the
value arithmetic behind enable steps 3-6 (regs `0x00`, `0x1d`, `0x36`, `0x3b`) and the three
`SetTxConfig` writes (regs `0x3d`, `0x41`, `0x3e`), all of which are ordinary read-modify-writes
on the lane device we have already confirmed as `0x4a`.
