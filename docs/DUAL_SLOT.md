# Dual-Slot A/B Upgrades (AS5610-52X)

EdgeNOS installs into a **two-slot A/B layout** so you can upgrade in place,
boot the new image, and automatically fall back to the previous one if the new
image doesn't come up healthy — all without going through ONIE. This is modeled
on Cumulus Linux's scheme on the same hardware (the `cl.active` variable name is
deliberately compatible).

This document covers the disk layout, how U-Boot chooses a slot, the scripted
auto-rollback, the per-slot initramfs, and the `nos-upgrade` / `nos-slot-clone`
tools.

---

## 1. Disk layout

The install disk is the onboard USB flash, `/dev/sda`. The partition table is
created by `installer/install-dual-slot.sh`:

| Partition | FS / type | Size | Role |
|---|---|---|---|
| **sda1** | ext2 (`NOS-PERSIST`) | 128 MB | **Persist** — survives upgrades (SSH host keys, machine state) |
| sda2 | extended container | — | wraps the logical partitions below |
| **sda5** | raw | 32 MB | **Slot 1 kernel** (FIT) |
| **sda6** | raw squashfs | 128 MB | **Slot 1 rootfs** |
| **sda7** | raw | 32 MB | **Slot 2 kernel** (FIT) |
| **sda8** | raw squashfs | 128 MB | **Slot 2 rootfs** |
| **sda3** | ext2 (`NOS-RW`) | rest (~GBs) | **R/W overlay store**, holds per-slot `config1/` and `config2/` |

Two things worth calling out:

- **The two slots are fully independent** — each has its own kernel *and* its own
  rootfs. An upgrade or a bad image in one slot cannot corrupt the other.
- **Per-slot read/write state.** Both slots share the single `sda3` partition,
  but each gets its *own overlay subtree* (`/rw/config1` vs `/rw/config2`). Slot
  1's runtime changes never bleed into slot 2.

> **ONIE install caveat.** ONIE's BusyBox kernel only exposes ~3 logical
> partitions during install, so a **fresh ONIE install writes slot 1 only**.
> Slot 2 is populated afterward from the running NOS (`nos-upgrade`, or
> `nos-slot-clone` to mirror slot 1 → slot 2).

---

## 2. How U-Boot picks a slot

U-Boot on this board has **no `CONFIG_BOOTCOUNT_LIMIT`**, so slot selection and
rollback are implemented entirely in U-Boot **environment variables** (set by the
installer's `configure_uboot()`), using `setexpr`/`itest`/`saveenv`.

The key variables:

| Variable | Meaning |
|---|---|
| `cl.active` | `1` or `2` — the slot to boot. The one knob that selects a slot. |
| `boot_active` | `if test ${cl.active} = 1; then run set_active1; else run set_active2; fi` |
| `set_active1` / `set_active2` | set `bootargs` (`root=/dev/sda6` + `active=1`, or `sda8` + `active=2`) then run `hw_active1`/`hw_active2` |
| `hw_active1` / `hw_active2` | `usb start; usbiddev; usbboot 0x02000000 ${usbdev}:5` (slot 1, kernel from sda5) or `:7` (slot 2, sda7), then `bootm ...#accton_as5610_52x` |
| `boot_count` | incremented every boot attempt; reset to 0 once the NOS is confirmed healthy |
| `boot_limit` | rollback threshold (default **3**) |
| `nos_bootcmd` | the rollback engine (below) |
| `onie_boot_reason` | **must be absent/empty** — any value makes U-Boot boot ONIE |

### The boot + rollback sequence

`nos_bootcmd` runs on every boot:

```
boot_count = boot_count + 1
saveenv
if boot_count > boot_limit:
        cl.active = (the other slot)
        boot_count = 0
        saveenv
run boot_active        # -> set_activeN -> hw_activeN -> bootm
```

So each boot bumps a persistent counter *before* booting. A slot that boots
cleanly resets the counter back to 0 (see §4). A slot that keeps failing (kernel
panic, edged never starts, watchdog reboot) lets the counter climb; once it
passes `boot_limit`, U-Boot flips `cl.active` to the other slot and boots that
instead. If `nos_bootcmd` itself errors out, `bootcmd` falls through to
`onie_bootcmd` as the ultimate backstop.

---

## 3. The initramfs (`initramfs/nos-init.c`)

The FIT carries a tiny initramfs whose `/init` is a **freestanding PPC32
raw-syscall program** (no libc — Debian's powerpc glibc assumes a hardware FPU
the e500v2 lacks, so a glibc static binary faults). It:

1. mounts `/proc`, `/sys`, `/dev`;
2. reads `/proc/cmdline` and parses `root=/dev/sdaN` and `active=S` (defaults to
   slot 1 / `sda6`; derives slot 2 if the device ends in `8`);
3. mounts the slot's squashfs read-only (retries up to 30 s for USB to settle);
4. mounts `sda3` and sets up the **per-slot overlay**
   (`upperdir=/rw/configS/upper`, `lowerdir=<squashfs>`) on `/newroot`;
5. writes `/.edgenos-boot` recording which slot/root is live;
6. `switch_root` into `/newroot` and `exec /sbin/init` (systemd).

This is what makes each slot's writable state independent and tied to the slot
the kernel was told to boot.

---

## 4. Confirming a boot is healthy (`nos-boot-success`)

The rollback contract has a software half. `nos-boot-success.service`
(`After=edged.service`) runs `nos-boot-success.sh`, which waits up to 60 s for
the datapath daemon to actually be running (`pgrep -x edged`). **Only if `edged`
is confirmed up** does it run `fw_setenv boot_count 0`, marking the slot good.

The consequence: a slot that boots Linux but whose datapath never comes up is
*never* marked good, so `boot_count` keeps climbing across reboots and U-Boot
rolls back to the other slot. "Healthy" means "the switch is actually
switching," not merely "Linux booted."

---

## 5. Upgrading a slot — `nos-upgrade`

`nos-upgrade` (in `/usr/sbin`) installs a `.bin` image into the **inactive** slot
and optionally activates + reboots into it. The active (running) slot is never
touched, so a failed upgrade always leaves a known-good slot.

```
Usage: nos-upgrade [options] <image.bin>
  --slot A|B    Install to a specific slot (default: the inactive one)
  --activate    Switch cl.active to the upgraded slot (only after verify passes)
  --reboot      Reboot after install
  --force       Allow writing the ACTIVE slot (DANGEROUS)
```

Safety layers, each of which aborts **before** any write to disk or env:

1. refuses to write the active slot (unless `--force`);
2. checks the payload has the `__ARCHIVE__` marker and both members extract;
3. checks content magic — kernel is a FIT (`d00dfeed`), rootfs is squashfs
   (`hsqs`);
4. checks each member fits its slot partition;
5. **byte-exact sha256 read-back** of the kernel and rootfs after writing.

`--activate` only flips `cl.active` after the read-back verify passes.

### Worked example: upgrade and validate both slots

This is the exact sequence used to roll the port-LED image onto both slots.
**Always read `cl.active` back as a separate step before rebooting.**

```bash
# On the running box (say it's on slot 1):

# 1. copy the image over and install to the inactive slot (2) + activate
scp edgenos-as5610-52x-dualslot.bin root@<box>:/tmp/edgenos.bin
ssh root@<box> 'nos-upgrade --activate /tmp/edgenos.bin'
#    -> "installing to slot 2 (/dev/sda7 + /dev/sda8) ... verified ... Activating slot 2"

# 2. read cl.active BACK before rebooting (MTD env writes can be lost otherwise)
ssh root@<box> 'fw_printenv cl.active'        # must show cl.active=2

# 3. reboot into slot 2, then test: edged active, links up, OSPF Full, ping 0%
ssh root@<box> 'reboot'

# 4. once slot 2 is validated, repeat from slot 2 to flash slot 1:
ssh root@<box> 'nos-upgrade --activate /tmp/edgenos.bin'   # installs to slot 1
ssh root@<box> 'fw_printenv cl.active'                     # must show cl.active=1
ssh root@<box> 'reboot'                                    # boot + test slot 1
```

After a fresh boot, `swp-l3.service` applies the swp addresses and OSPF
re-converges on its own (~40 s). Note that a **manual** `systemctl restart edged`
does *not* re-run `swp-l3` (it recreates the swpN taps and drops their
addresses) — only reboots, or an explicit `systemctl restart swp-l3`, restore
them.

---

## 6. Mirroring slots — `nos-slot-clone`

`nos-slot-clone` copies the **active** slot's kernel + rootfs onto the inactive
slot, so both run identical software without re-flashing an image:

- reads the active squashfs block device directly (safe — it's mounted RO);
- copies **exactly the payload size** (FIT total size from its header; squashfs
  `bytes_used` from offset 40), not the whole partition;
- sha256 read-back verify per member;
- **never changes `cl.active`** — it only writes the inactive slot.

Use it after a verified upgrade if you'd rather mirror the running slot than
flash the `.bin` twice.

---

## 7. Quick reference: gotchas

- **Read `cl.active` back before every reboot.** A `saveenv` to the MTD U-Boot
  env immediately followed by a hard reset can be lost; confirm the value stuck.
- **`onie_boot_reason` must be empty.** If it's set to anything, U-Boot boots
  ONIE → install loop. The installer deletes it.
- **DHCP IP floats across reboots.** The mgmt port pulls DHCP; the address can
  change between boots (find by MAC if needed).
- **"Healthy" = edged up.** boot_count is only reset after edged is confirmed
  running; a datapath that won't start triggers auto-rollback by design.

---

## See also

- [`ONIE_IMAGE_BUILD.md`](ONIE_IMAGE_BUILD.md) — how to build the `.bin` you flash.
- [`../BOOT.md`](../BOOT.md) — the lower-level U-Boot/FIT boot chain.
- [`FLASH_MTD_AND_ONIE_RECOVERY.md`](FLASH_MTD_AND_ONIE_RECOVERY.md) — reaching ONIE from the running OS, NOR flash layout.
