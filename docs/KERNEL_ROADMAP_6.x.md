# Roadmap: EdgeNOS to the next Linux kernel (5.15 → 6.x LTS)

*Planning doc. Current: 5.15.209 LTS on Edgecore AS5610-52X
(NXP P2020 PowerPC, e500v2, 32-bit big-endian; Broadcom Trident+ ASIC).*

## The one thing to understand first

5.10 → 5.15 was a **rebuild** (vanilla source + our DTS + `olddefconfig`, zero
out-of-tree changes). **5.15 → 6.x is a port.** Two risks that were ~zero last
time become the whole job:

1. **Out-of-tree driver API drift** across the 5.x→6.x boundary (our 4 platform
   modules), and
2. **32-bit PowerPC (mpc85xx/e500) attrition in mainline** — a slowly sunsetting
   architecture. This is the strategic risk, and it grows the further we climb.

Everything *else* (delivery, rollback, install) is already solved and reused.

## Target selection

| Target | Support runway | Distance from 5.15 | Recommendation |
|---|---|---|---|
| **6.1.x LTS** (latest point release, ~6.1.175) | into ~2027 | one major step | **Do this first** — lowest drift, 85xx still solid |
| 6.6.x LTS (~6.6.142) | into ~2029 | larger | After 6.1 proves out |
| 6.12.x LTS (~6.12.93) | longest (~2030+) | largest | Endgame, only if 85xx still healthy there |

**Plan: ladder, don't leap.** Go 5.15 → **6.1** first. Each LTS step keeps API
drift digestible and gives a clean validation point. Re-evaluate 6.6/6.12 once
6.1 is in the field. (Verify the *current* point release at build time — these
numbers drift.)

## Risk register (what actually has to be done)

### R1 — 32-bit PowerPC mainline support  *(severity: HIGH, check FIRST)*
`mpc85xx`/`corenet`/e500 is lightly maintained and has been eroding. Before
investing in the port, the **first build** must confirm:
- `arch/powerpc` still configures + builds for our platform (CONFIG_PPC_85xx /
  the corenet/e500 bits) at the target version;
- our `as5610-52x.dts` still compiles with the target's `dtc` (deprecation →
  warnings → possibly errors);
- the e500v2 SPE handling is intact (kernel side; userland SPE is a separate,
  already-handled matter).
**Go/no-go gate:** if the platform is broken/removed at the target, that's a hard
stop for that version — fall back to the previous LTS or carry patches. This is
the gate that decides how far up the ladder we can go at all.

### R2 — Out-of-tree kernel modules  *(severity: MED-HIGH, the bulk of the work)*
The four platform modules and their 5.x→6.x exposure:
- **bde** (ASIC access: PCI, ioremap, mmap, DMA, interrupts) — *most exposed.*
  Watch DMA API, `ioremap`/`pgprot`, PCI helpers, IRQ.
- **cpld** (platform_driver + sysfs attrs) — `class_create()` dropped its module
  arg in 6.4; `device_attribute`/`dev_groups`, sysfs churn.
- **tmon** (thermal/hwmon) — hwmon registration API shifts.
- **retimer** (i2c + class) — i2c probe signature (`probe_new`/`probe`), class
  API, `del_timer` → `timer_delete` family.
**Approach:** build each against the new kernel, fix what the compiler flags,
iterate. The RE/logic is unchanged; this is API-shim work. Estimate the
**majority of the effort lives here.**

### R3 — defconfig migration  *(severity: LOW-MED)*
`olddefconfig` carries most settings, but more symbols rename/move across a major
jump. **Must re-verify after migration:** MTD/CFI/PHYSMAP (boot env), the BDE
dependencies, squashfs + overlayfs (root), namespaces/cgroup2, and core
networking. (Same sanity check we run today, widened.)

### R4 — toolchain  *(severity: LOW)*
Current cross-build uses Debian bookworm `powerpc-linux-gnu-gcc` (GCC 12), which
builds 6.1/6.6 fine. Confirm against 6.12's minimum GCC if we climb that far.

### R5 — boot chain / FIT  *(severity: LOW)*
PPC still builds `uImage`; our FIT (kernel.gz + dtb + initramfs) and U-Boot
`bootm` flow are version-agnostic. Expect no change; verify the FIT boots.

## Phased plan

**Phase 0 — Decide + scaffold.** Pick target (6.1.x). The build is already
parameterized: `KVER=6.1.x build-kmodules.sh`. No new infra.

**Phase 1 — Kernel + DTB trial build  *(this is the go/no-go)*.**
Vanilla 6.1.x + our DTS + `olddefconfig` → build `uImage` + `dtbs`. Resolve R1
(platform builds, DTS compiles) and R3 (config sanity). **If R1 fails, stop and
reassess the target.** If it builds, the rest is "just" engineering.

**Phase 2 — Out-of-tree modules (the work).** Build bde/cpld/tmon/retimer against
6.1; fix R2 API drift until all four compile, link, and carry correct vermagic.

**Phase 3 — Assemble image.** FIT (new kernel + dtb + existing initramfs) →
`assemble-rootfs-from-base.sh` swaps in the new versioned module tree →
`build-image.sh --dual-slot`. (Unchanged pipeline.)

**Phase 4 — Deploy to the INACTIVE slot + validate.** `nos-upgrade --slot <inactive>`
(byte-verified write), activate, reboot. Validate: all 4 modules load,
edged/data plane up, 52 ports, OSPF neighbors Full, ping upstream, fans
regulating. **Auto-rollback is the safety net** — a bad boot falls back on its
own; serial is the backstop.

**Phase 5 — Make it durable.** Once validated, `nos-slot-clone` to sync both
slots, and rebuild the **ONIE installer** so fresh installs land 6.1.

**Phase 6 — Strategic decision.** With 6.1 in the field, decide whether to climb
to 6.6/6.12 (re-running R1 at each step) or hold.

## Delivery is already solved (reused, not rebuilt)

The hard "ship it safely" half is done and proven from the 5.15 work:
A/B dual-slot, SHA-256 verified writes, **scripted auto-rollback** + boot-success
gating, and validated from-ONIE install. The kernel jump plugs straight into it.

## Effort estimate

- Phase 1 (trial build, R1/R3): **~1 session** — and it gates everything.
- Phase 2 (module port, R2): **the bulk** — a few sessions of compiler-driven
  fixups, bde being the long pole.
- Phases 3–5 (assemble/deploy/validate/clone/installer): **~1 session**, mostly
  reuse.
- Per additional ladder step (6.6, 6.12): repeat Phase 1–5, smaller each time
  *unless* R1 (PPC support) degrades.

## The strategic footnote

32-bit PowerPC is a sunsetting mainline architecture. There is a real future
kernel where `mpc85xx`/e500 breaks or is dropped — that's the eventual ceiling
for this box, and **R1 is the canary we check at every step.** The long-term
hedge is the platform that doesn't have this problem: the **AS4610 is ARM**, a
first-class, long-lived mainline arch. For the 5610 specifically, the realistic
goal is "stay on a supported LTS as far up the ladder as PPC support holds."

---
*Pipeline reference (all parameterized today):*
`KVER=<ver> scripts/build-kmodules.sh` → `scripts/assemble-rootfs-from-base.sh`
→ `scripts/package-image.sh` → `installer/build-image.sh --dual-slot` →
`nos-upgrade --slot <inactive> --activate --reboot` → validate → `nos-slot-clone`.
