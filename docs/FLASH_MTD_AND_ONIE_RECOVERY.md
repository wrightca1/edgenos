# NOR Flash, MTD, and Reaching ONIE from the OS (AS5610-52X)

How EdgeNOS exposes the on-board NOR flash, how the U-Boot boot chain decides
between the NOS and ONIE, and how to drop into the **ONIE installer from a
running EdgeNOS** with `fw_setenv` — without serial console, and without any
risk to the bootloader or the ONIE recovery image.

All values below were verified read-only against the live ONIE `/proc/mtd` and
the live U-Boot environment on this exact board.

---

## 1. Flash hardware & MTD partition map

- **Device:** Spansion **S29GL064N** NOR, on the Freescale **eLBC** localbus
  (`localbus@ff705000`, chip-select 0 → physical `0xefc00000`), 4 MB mapped,
  bank/device-width 1, **64 KB (`0x10000`) uniform sectors**.
- Defined in `kernel/dts/as5610-52x.dts` under `nor@0,0` (`compatible = "cfi-flash"`).

| MTD | label | offset | size | sectors | access |
|-----|-------|--------|------|---------|--------|
| mtd0 | `onie` | 0x000000 | 0x360000 (3.375 MB) | 54 | **read-only** |
| mtd1 | `u-boot-env` | 0x360000 | 0x010000 (64 KB) | 1 | **read-write** |
| mtd2 | `board_eeprom` | 0x370000 | 0x010000 (64 KB) | 1 | read-only |
| mtd3 | `uboot` | 0x380000 | 0x080000 (512 KB) | 8 | read-only |

**Only `u-boot-env` (mtd1) is writable.** `onie`, `uboot`, and `board_eeprom`
carry `read-only;` in the DTS, so the kernel MTD layer rejects writes to them
(`/sys/class/mtd/mtdN/flags`: writable = `0xc00`, read-only = `0x800`). This is
the core safety guarantee: **the OS cannot corrupt the bootloader or the ONIE
recovery image** — the worst it can touch is the env, which is recoverable.

### Kernel requirement (the gotcha)

`/proc/mtd` was *empty* until we added one Kconfig symbol. The `of_flash`
driver that binds the DTS `cfi-flash` node is `CONFIG_MTD_PHYSMAP_OF`, which
**depends on `CONFIG_MTD_PHYSMAP`**. Without `PHYSMAP`, `olddefconfig` silently
drops `PHYSMAP_OF`, nothing probes the NOR, and no partitions appear. Required
in `config/kernel/as5610_defconfig`:

```
CONFIG_MTD=y
CONFIG_MTD_CFI=y
CONFIG_MTD_CFI_AMDSTD=y      # Spansion = AMD/STD command set
CONFIG_MTD_CFI_I1=y          # interleave 1  (DTS device-width=1)
CONFIG_MTD_MAP_BANK_WIDTH_1=y# bank-width 1
CONFIG_MTD_OF_PARTS=y        # parse partitions from the DTS
CONFIG_MTD_PHYSMAP=y         # <-- the dependency that was missing
CONFIG_MTD_PHYSMAP_OF=y      # of_flash: binds the "cfi-flash" node
```

`fw_printenv`/`fw_setenv` (u-boot-tools) read/write via `/etc/fw_env.config`:

```
# MTD device   Offset   Env size   Flash sector
/dev/mtd1       0x0      0x10000    0x10000
```

(Single, non-redundant env = one line. The env is `CONFIG_ENV_SIZE = 0x10000`,
sector `0x10000` — matches ONIE's own `fw_env.config` exactly.)

---

## 2. The U-Boot boot chain

From the live env (`fw_printenv`):

```
bootcmd            = run check_boot_reason; run nos_bootcmd; run onie_bootcmd
check_boot_reason  = if test -n $onie_boot_reason; then \
                       setenv onie_bootargs boot_reason=$onie_boot_reason; \
                       run onie_bootcmd; fi
nos_bootcmd        = usb start; usbiddev; setenv bootargs console=ttyS0,115200 cma=32M; \
                       usbboot 0x02000000 ${usbdev}:5 && bootm 0x02000000#accton_as5610_52x
onie_rescue        = setenv onie_boot_reason rescue   && boot
onie_uninstall     = setenv onie_boot_reason uninstall && boot
onie_update        = setenv onie_boot_reason update    && boot
```

The decision is a single variable: **if `onie_boot_reason` is set (non-empty),
U-Boot boots ONIE; if empty/unset, it boots the NOS** (`nos_bootcmd`, which
`usbboot`s the FIT from partition 5). The NOS installer (`installer/install.sh`)
*deletes* `onie_boot_reason` so the box boots EdgeNOS.

---

## 3. Reaching ONIE from a running EdgeNOS

No serial, no GRUB/U-Boot menu — just set the variable and reboot:

```sh
fw_setenv onie_boot_reason install     # or: rescue | uninstall | update
reboot
```

- `install`  → ONIE install mode (waits for / discovers an image to install)
- `rescue`   → ONIE rescue shell
- `uninstall`→ ONIE erases the NOS and returns to a clean ONIE
- `update`   → ONIE self-update

This only writes **mtd1** (the env). `uboot`/`onie` are read-only, so the
bootloader and recovery image are never at risk. After a successful
`onie-nos-install`, ONIE clears `onie_boot_reason`, so the box boots back into
the freshly installed NOS. (To abort an `install` and return to the existing
NOS without reinstalling, clear the var from ONIE/U-Boot:
`setenv onie_boot_reason; saveenv`.)

### From-OS install end-to-end
```sh
# stage an image somewhere ONIE can fetch it (USB, or a reachable HTTP/TFTP host),
# then from EdgeNOS:
fw_setenv onie_boot_reason install && reboot
# ... box boots ONIE install mode; run onie-nos-install <url|/path> there.
```

---

## 4. Verify (read-only — safe on a live box)

```sh
cat /proc/mtd                       # expect onie / u-boot-env / board_eeprom / uboot
for m in 0 1 2 3; do                # 0xc00 = writable, 0x800 = read-only
  echo "mtd$m $(cat /sys/class/mtd/mtd$m/name) flags=$(cat /sys/class/mtd/mtd$m/flags)"
done
fw_printenv | grep -E 'bootcmd|check_boot_reason|onie_boot_reason'   # must read cleanly
```

Expected: `mtd1 u-boot-env flags=0xc00` (writable), the other three `0x800`
(read-only), and `fw_printenv` returns a valid env (rc 0).

---

## 5. Build / ship notes

- The kernel + DTB + matched out-of-tree modules are rebuilt together by
  `scripts/build-kmodules.sh` (emits `output/kernel/{uImage,as5610-52x.dtb}` and
  `output/modules/*.ko`). `assemble-rootfs-from-base.sh` installs the
  kernel-matched modules over the stale base copies; `package-image.sh` packs
  the new uImage + DTB into the FIT.
- Changing the flash layout means editing **both** the DTS `nor@0,0` partitions
  **and** `fw_env.config` to stay consistent, then verifying read-only
  (`fw_printenv` must validate) before trusting any write.

*Authoritative cross-check:* booting the box into ONIE and reading its
`/proc/mtd` + `/etc/fw_env.config` gives the ground-truth flash layout for this
board — match the DTS to it.
