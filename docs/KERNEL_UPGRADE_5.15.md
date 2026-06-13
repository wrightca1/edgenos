# Upgrading a White-Box Switch to a Supported Linux Kernel (5.10 → 5.15 LTS)

*Technical summary — source material for a write-up. Audience: Linux engineers,
not switch/ASIC specialists.*

## TL;DR

EdgeNOS is a from-scratch, open Network Operating System running on a
**10-year-old Edgecore AS5610-52X** switch (52× 10/40G ports). The switch ships
as bare metal; the OS is ours. This work took its Linux kernel from
**5.10.258 to 5.15.209 (the next LTS)** and delivered it **in the field with
zero downtime risk** — A/B slots, byte-verified writes, and automatic rollback —
on a 32-bit PowerPC box the original vendor long ago stopped supporting.

The headline: keeping abandoned enterprise hardware on a **current, security-
maintained kernel** turned out to be a clean, low-drama mainline upgrade — not
the forklift everyone assumes.

## Why this matters

A switch is a Linux box with an unusual NIC (a switching ASIC). Like any Linux
box, the kernel underneath it ages out: LTS branches reach end-of-life, security
fixes stop, and you're left running something you can no longer patch.

Most white-box / "disaggregated" switches inherit whatever kernel the vendor
froze years ago. When the vendor walks away, so does your kernel maintenance.
Owning the OS means you can do what you'd do on any server: **move to a kernel
that's still getting fixes.**

## Starting point

- **Hardware:** Edgecore AS5610-52X — NXP/Freescale **P2020 PowerPC** (e500v2
  core, 32-bit, big-endian), Broadcom Trident+ switching ASIC.
- **Kernel:** 5.10.258 — and notably already at the **very tip of the 5.10 LTS
  branch** (5.10.258 was the newest 5.10 point release available). The 5.10
  series is heading toward end-of-life; there was nowhere left to go *within*
  5.10. The only way to stay current was to jump to the next LTS.
- **Target:** **5.15.209** — the latest point release of the 5.15 LTS branch,
  with years more support runway.

## The upgrade itself — easier than feared

The fear with a PowerPC embedded box is that a kernel jump means a pile of
out-of-tree patches and broken drivers. It didn't:

1. **No kernel patches needed.** The board builds from a **vanilla kernel.org
   tarball** plus our device tree and config. The P2020 / e500v2 platform is
   well-supported in mainline, so there was nothing to forward-port.
2. **The config migrated cleanly.** `make olddefconfig` carried our settings
   forward and preserved every option that matters for this board — including
   the NOR-flash stack (MTD / CFI / PHYSMAP) that the bootloader environment
   depends on.
3. **Out-of-tree drivers compiled with zero drift.** EdgeNOS has four small
   platform kernel modules (ASIC register access, CPLD, thermal, retimer). All
   four built and linked against 5.15 **with no source changes**, and came out
   with the correct module vermagic so they load on the new kernel.

In short, the 5.10 → 5.15 step for this platform was a **rebuild, not a port.**
The whole kernel + module build is one parameterized command.

## Shipping it safely (the part that's actually hard)

A datacenter switch can't be casually rebooted into an unproven kernel. The
risky move isn't *building* the kernel — it's *cutting over* to it. So the
upgrade rides on an **A/B dual-slot** scheme, the same idea phones and modern
servers use:

- **Two independent system slots.** The new kernel + root filesystem are written
  to the **inactive** slot while the box keeps running on the active one. The
  running system is never touched.
- **Verified writes.** Every image written to flash is **read back and SHA-256
  compared** before it's allowed to become bootable — no silent corruption.
- **One-flag cutover and rollback.** A single bootloader variable selects the
  active slot. Flip it, reboot, you're on the new kernel. Flip it back, you're
  on the old one.
- **Automatic rollback.** The bootloader counts boot attempts; a slot is only
  marked "good" after the data plane actually comes up. If the new kernel ever
  failed to boot cleanly, the box **falls back to the known-good slot on its
  own** — no console, no truck roll.

Net effect: upgrading the kernel became a routine, reversible operation instead
of a heart-in-throat event.

## A few real gotchas (for the engineers)

Honest notes from the trenches, the kind that don't show up in tutorials:

- **Never overwrite the root filesystem you're currently running from.** The
  root is a read-only squashfs mounted from a flash partition; rewriting that
  partition under the live mount makes the kernel read garbage for any not-yet-
  cached block (`Input/output error` on random binaries). Write the *other*
  slot. Obvious in hindsight; alarming in the moment.
- **Mind the userland mismatch.** The bootloader-env tool (`fw_setenv`) is one
  binary dispatched by its name; the image had only the read alias, so writes
  failed until the write alias was added. Small, classic, easy to miss.
- **Busybox is not coreutils.** The from-scratch installer's image-validation
  checks worked on the full Linux box but broke in the minimalist installer
  environment, where `od` lacks the GNU flags. Byte-comparison with `dd` + `cmp`
  is the portable answer.

## Result

The box now runs **5.15.209**, validated end-to-end:

- vermagic-matched platform modules all loaded;
- data plane fully up (all 52 ports, the userspace forwarding daemon healthy);
- routing converged (OSPF neighbors Full, traffic forwarding, ping to the
  upstream router clean);
- thermal/fan control regulating; no regressions.

Both A/B slots carry the new kernel, auto-rollback is armed, and a fresh
install-from-scratch was validated to produce the same result.

## The takeaway

"This hardware is end-of-life" usually means *the vendor* is done with it — not
that the silicon is. Because the operating system is open and Linux-native, a
decade-old switch was moved onto a **current, supported, security-maintained
kernel** with a clean mainline rebuild and a safe, reversible rollout.

That's the quiet superpower of open networking: a switch is just a Linux box,
and Linux boxes get to keep up with Linux.

---
*Platform: Edgecore AS5610-52X (P2020 PowerPC e500v2, Broadcom Trident+).
Kernel: 5.10.258 → 5.15.209 LTS, delivered via A/B dual-slot with verified
writes and automatic rollback.*
