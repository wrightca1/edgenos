# ONIE Installer Compatibility Issues (AS5610-52X)

Issues discovered during live ONIE installation on the Edgecore AS5610-52X.
This ONIE build is based on BusyBox v1.25.1 (2017-06-06).

---

## 1. Platform String Uses Underscores, Not Hyphens

**Problem**: `onie-sysinfo -p` returns `powerpc-accton_as5610_52x-r0` (underscores in
platform name), not `powerpc-accton-as5610-52x-r0` (hyphens).

**Impact**: Installer platform check fails immediately.

**Fix**: Match the exact string from `onie-sysinfo -p`:
```sh
PLATFORM="powerpc-accton_as5610_52x-r0"
```

---

## 2. `/sbin` and `/usr/sbin` Not in PATH

**Problem**: ONIE sets `PATH=/usr/bin:/bin` only. Tools like `fdisk`, `mke2fs`, `reboot`,
`halt`, `fw_setenv` live in `/sbin` or `/usr/sbin` and are not found.

**Impact**: `fdisk: not found`, `mkfs.ext2: not found`, `reboot: not found`.

**Fix**: Export full PATH at the top of the installer:
```sh
export PATH="/usr/sbin:/sbin:/usr/bin:/bin:$PATH"
```

**Note**: ONIE's own `exec_installer` script (line 469) also calls `reboot` without
the full path, causing a spurious "Failure" message even after successful install.

---

## 3. BusyBox `fdisk` Does Not Support Heredoc Input Reliably

**Problem**: Piping a heredoc (`<<'EOF' ... EOF`) to busybox `fdisk` can fail
silently or behave differently than util-linux `fdisk`.

**Impact**: Partitioning may not complete.

**Fix**: Use `printf` with `\n` separators piped to fdisk:
```sh
printf "o\nn\np\n1\n8192\n270273\n...\nw\n" | fdisk -u "$INSTALL_DEV" || true
```

---

## 4. `mkfs.ext2 -q` Flag Not Supported in BusyBox

**Problem**: BusyBox `mke2fs` does not support the `-q` (quiet) flag.

**Impact**: `mkfs.ext2 -q` fails.

**Fix**: Use `mke2fs` without `-q`:
```sh
mke2fs -L "LABEL" "${INSTALL_DEV}1"
```

---

## 5. `partprobe` Not Available

**Problem**: BusyBox does not include `partprobe`. After partitioning, the kernel
partition table may not be refreshed.

**Impact**: Subsequent `dd` to partitions may target stale device nodes.

**Fix**: Use `sync` + `sleep` instead:
```sh
sync
sleep 2
```

---

## 6. `fw_setenv` Prompts for Confirmation

**Problem**: ONIE's `fw_setenv` prompts "Proceed with update [N/y]?" and waits for
user input. In a non-interactive installer, this blocks or consumes stdin.

**Impact**: U-Boot environment variables not actually written; subsequent boot
goes back to ONIE instead of the installed NOS.

**Fix**: Pipe `y` to auto-confirm, and suppress errors:
```sh
echo y | fw_setenv cl.active 1 2>/dev/null || true
```

---

## 7. `ssh-keygen` Not Available in ONIE

**Problem**: ONIE's BusyBox does not include `ssh-keygen`.

**Impact**: Cannot generate SSH host keys during install.

**Fix**: Guard with `command -v` check. Generate keys on first boot instead:
```sh
if command -v ssh-keygen >/dev/null 2>&1; then
    ssh-keygen -t ed25519 -f "$mnt/ssh/ssh_host_ed25519_key" -N "" -q
fi
```

---

## 8. ONIE Reports "Failure" Even on Successful Install

**Problem**: ONIE's `exec_installer` calls `reboot` at the end without full path.
Since `/sbin` is not in PATH, the reboot fails and ONIE logs a "Failure" message,
even though the NOS install completed successfully.

**Impact**: Confusing error message. The switch does not automatically reboot.

**Workaround**: Manually reboot after install:
```sh
ssh root@<switch> /sbin/reboot
```

Or add to installer before `exit 0`:
```sh
/sbin/reboot 2>/dev/null || true
```

---

## 9. Available BusyBox Tools

For reference, the tools available in this ONIE build (BusyBox v1.25.1):

**Partitioning**: `fdisk`, `blkid` (no `parted`, `sfdisk`, `sgdisk`, `partprobe`)
**Filesystem**: `mke2fs`, `mkfs.ext2`, `mkfs.vfat` (no `mkfs.ext4`)
**File ops**: `dd`, `tar`, `mount`, `umount`, `cp`, `mv`, `rm`, `mkdir`, `sync`
**Network**: `wget`, `ip`, `ifconfig`, `ping`
**Other**: `fw_printenv`, `fw_setenv`, `onie-sysinfo`, `onie-nos-install`
**NOT available**: `parted`, `partprobe`, `ssh-keygen`, `curl`, `mkfs.ext4`, `reboot` (in PATH)

---

## 10. ONIE `exec_installer` Reboot Failure Resets `nos_bootcmd`

**Problem**: After a successful install, ONIE's `exec_installer` (line 469) calls
`reboot` without the full `/sbin/reboot` path. The reboot fails, the error handler
triggers, and `install_remain_sticky_arch()` runs again, resetting `nos_bootcmd=true`.
This means the NOS installer's `fw_setenv nos_bootcmd` gets overwritten.

**Impact**: Even though the install succeeds and `fw_setenv` writes correctly,
`nos_bootcmd` is reset to `true` before the switch actually reboots. On the next
boot, U-Boot sees `nos_bootcmd=true` and falls through to ONIE.

**Root cause**: ONIE's discover daemon loop re-runs `install_remain_sticky_arch()`
after the reboot failure.

**Fix**: The installer must ensure the reboot actually works. Add `/sbin` to PATH
in the installer script, or call `/sbin/reboot` explicitly. Alternatively, set
`nos_bootcmd` from U-Boot directly using `saveenv`.

---

## 11. U-Boot `bootargs` Must Be Set Explicitly

**Problem**: When booting a FIT image with `bootm`, U-Boot passes `bootargs` env
variable to the kernel. If `bootargs` is not set (or has stale values), the kernel
falls back to its built-in `CONFIG_CMDLINE`. If `CONFIG_CMDLINE` has the wrong
root device, the kernel panics silently.

**Impact**: Kernel appears stuck after loading - no serial output, no boot.

**Fix**: The U-Boot boot chain must set `bootargs` before `bootm`. The existing
`initargs` variable does this: `setenv bootargs quiet console=$consoledev,$baudrate $lbootargs`.
Ensure `lbootargs` contains the correct root device:
```
lbootargs=root=/dev/sda6 rootfstype=squashfs ro rootwait
```

The `flashboot` → `initargs` → `boot_active` chain handles this automatically
when `lbootargs` is set correctly.

---

## 12. FIT Image on Partition 5 Uses Raw `usbboot`, Not ext2

**Problem**: The AS5610-52X U-Boot uses `usbboot $loadaddr $usbdev:$partition`
to load from USB partitions. This is a raw block read, NOT `ext2load`.

**Impact**: Writing FIT to an ext2 filesystem on sda5 and using `ext2load` does
not match the existing boot chain. The correct approach is `dd` to raw partition
and use `usbboot`.

**Fix**: Write FIT image via `dd` to raw sda5/sda7 (as Cumulus does), and use
the existing `usbboot`-based `hw_boot` chain.

---

## 13. Debian jessie SSH Defaults Reject Root Password Login

**Problem**: Debian jessie's default `sshd_config` has `PermitRootLogin without-password`
and `PasswordAuthentication yes` but the root account in the squashfs may have a locked
password (from debootstrap). Even if `chpasswd` was run during rootfs build, the
squashfs is read-only and the overlay starts empty on first boot.

**Impact**: Cannot SSH with password after fresh install.

**Fix**: The rootfs build must:
1. Set root password via `chpasswd` before squashfs creation
2. Set `PermitRootLogin yes` in sshd_config before squashfs creation
3. Ensure these changes are baked into the squashfs, not relying on overlay

---

## Summary Checklist for ONIE-Compatible Installers

- [ ] Match platform string exactly from `onie-sysinfo -p` (underscores!)
- [ ] Set `PATH="/usr/sbin:/sbin:/usr/bin:/bin:$PATH"`
- [ ] Use `printf | fdisk` instead of heredoc
- [ ] Use `mke2fs` without `-q` flag
- [ ] Use `sync; sleep 2` instead of `partprobe`
- [ ] Pipe `echo y |` to `fw_setenv`
- [ ] Guard optional tools with `command -v`
- [ ] Don't rely on `reboot` being in PATH - use `/sbin/reboot`
- [ ] Test installer in ONIE shell first: `sh -x /path/to/install.sh`
