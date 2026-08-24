# Persistent root filesystem — Debian armel on USB

Replaces the initramfs-only setup with a real, persistent rootfs so the box
does not need the lab host on every boot, and so packages (FRR) can be
installed normally.

## Why bookworm

**Debian dropped `armel` after bookworm (12).** The Feroceon 88FR131 is
ARMv5TE soft-float, which is exactly what `armel` targets — so bookworm is the
last Debian release that supports this CPU. `armhf` requires ARMv7 with a
hardware FPU and will not run here.

## Why the second stage runs on the switch

The usual route is `debootstrap --foreign` on the build host, then
`--second-stage` under `chroot` with `qemu-arm-static` and a `binfmt_misc`
handler. **That does not work on this build host** — it is a Proxmox
container, `/proc/sys/fs/binfmt_misc` is read-only, and registering the ARM
handler fails with `Permission denied`.

The workaround is better anyway: the EX2200-C *is* an ARMv5TE machine, so it
runs its own second stage natively. No emulation, and the result is validated
on the real target rather than under qemu.

```
host:   debootstrap --foreign --arch=armel --variant=minbase bookworm rootfs
host:   tar czf rootfs-stage1.tar.gz -C rootfs .        # 188 MB -> 83 MB
host:   serve it over HTTP
box:    mke2fs -F -i 32768 -L ex2200root /dev/sdb1
box:    mount -t ext2 /dev/sdb1 /mnt/root
box:    wget -O - http://<host>:8080/rootfs-stage1.tar.gz | tar xzf - -C /mnt/root
box:    chroot /mnt/root /debootstrap/debootstrap --second-stage
```

## Which disk — this matters

There are **two** USB block devices, and picking the wrong one destroys the
Junos install that is still our fallback:

| device | model | size | what |
|---|---|---|---|
| `sda` | `ST ST72682` | 1 GB | **Junos root — do not touch.** Four partitions, the standard Junos layout |
| `sdb` | `SanDisk Cruzer Glide` | 29 GB | the spare stick, our target |

Confirm by model before every destructive command:

```sh
cat /sys/block/sdb/device/vendor /sys/block/sdb/device/model
```

## Why ext2 and not ext4

Busybox only builds `mke2fs`/`mkfs.ext2` — no journal support — and there is
no `mkfs.ext4` on the box during bring-up. The kernel has `CONFIG_EXT2_FS=y`
so it mounts natively.

The tradeoff is real: **no journal means an unclean power-off needs `fsck`.**
Acceptable for a lab bring-up, and worth revisiting once `e2fsprogs` is
installed from the rootfs itself — at which point the filesystem can be
rebuilt as ext4 properly.

## Booting from it — via the initramfs, not `root=`

The obvious route is `root=/dev/sdb1 rootfstype=ext2 rootwait` in `bootargs`.
**Don't.** Plain `rootwait` waits *indefinitely* for the device
(`Documentation/admin-guide/kernel-parameters.txt`), so a stick that is absent,
dead or unplugged hangs the boot with no automatic way out.

Instead the initramfs stays primary and `/init` hands over only if the rootfs
is actually usable:

```sh
for i in 1 2 3 4 5 6 7 8 9 10; do [ -b /dev/sdb1 ] && break; sleep 1; done
if mount -t ext2 /dev/sdb1 /mnt/root 2>/dev/null && [ -x /mnt/root/sbin/init ]; then
        exec switch_root /mnt/root /sbin/init
fi
echo "persistent root unusable - staying in the rescue shell"
```

The bounded wait covers asynchronous USB enumeration, and **any** failure —
missing device, bad filesystem, no `/sbin/init` — lands in the busybox rescue
shell instead of hanging. The rescue path needs no disk at all, so it survives
whatever happened to the stick.

## Console tooling note

`bin/ex-linux-cmd.py` waits for the shell **prompt**, not for the console to
go quiet. A quiet-based wait is wrong for anything long and silent:
`wget | tar` prints nothing while it runs, so the console falls silent
immediately, the next command is typed into a still-busy shell, and all the
output interleaves. That cost two confusing captures before it was fixed.
