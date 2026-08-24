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

## The clock, and why `apt` fails before you fix it

The board has an RTC (`f1010300.rtc`) but **no battery**, so it powers on at
the Unix epoch. Nothing in the boot path sets the time. `apt` then rejects
every repository with a message that does not obviously mean "wrong clock":

```
E: Release file for http://deb.debian.org/debian/dists/bookworm/InRelease
   is not valid yet (invalid for another 20645d 9h 46min 3s)
```

20,645 days is ~56 years — 2026 minus 1970. Set the clock before anything
that validates signatures:

```sh
date -u -s '2026-08-24 11:08:27'
```

`busybox hwclock -w` fails here with `can't open '/dev/misc/rtc'` — busybox
looks for the old path while the kernel exposes `/dev/rtc0`. Use
`hwclock -f /dev/rtc0 -w`, and note it is pointless without a battery anyway.

**Resolved.** Once the management subnet gained internet access,
`systemd-timesyncd` fixes this properly:

```
apt-get install --no-install-recommends systemd-timesyncd
```

```
systemd-timesyncd: Contacted time server 152.67.232.7:123 (0.debian.pool.ntp.org).
systemd-timesyncd: Initial clock synchronization to Mon 2026-08-24 13:09:41 UTC.
```

Box and build host then report an identical epoch. Note `timedatectl` prints
nothing here — it needs dbus, which `minbase` does not ship — so check
`journalctl -u systemd-timesyncd` instead.

The `date -u -s` workaround above is still what you need in the *initramfs*
rescue path, which has no NTP client.

## No internet on the management subnet

**This was true and is no longer.** Originally the box reached the lab host and
nothing else: ICMP, TCP/80, TCP/443 and UDP/53 to public addresses all timed
out, so it was not a DNS problem — there was no path. `traceroute` showed
packets dying at the gateway, then later at `10.101.1.1` once forwarding was
partly opened.

After the upstream fix the box has full internet:

```
# ping -c 3 8.8.8.8
3 packets transmitted, 3 received, 0% packet loss
rtt min/avg/max = 11.909/11.967/11.999 ms
```

`apt-get update` now works with no proxy at all.

**The workaround is kept, disabled, because it is worth knowing.** While the
subnet was isolated, the lab host ran **apt-cacher-ng** on 3142 and the box
pointed at it. The neat part: with an HTTP proxy the box needs **no DNS of its
own**, because apt sends the full URL and the proxy resolves it — so an empty
`/etc/resolv.conf` is fine. The config survives as
`/etc/apt/apt.conf.d/01proxy.disabled`; rename it to re-enable.

One gotcha found when DNS was finally configured: `debootstrap` copies the
**build host's** `/etc/resolv.conf` into the rootfs. Here that meant the
switch inherited a tailscale-generated file. Overwrite it.

## systemd needs kernel options `mvebu_v5_defconfig` does not set

`mvebu_v5_defconfig` is an embedded config and omits several things systemd
treats as mandatory. The failure is abrupt and gives no hint which option is
missing:

```
systemd[1]: Failed to mount API filesystems.
systemd[1]: Freezing execution.
```

That is systemd being unable to mount `/sys/fs/cgroup`. The kernel needs, at
minimum:

```
CONFIG_CGROUPS=y          # mandatory - without it systemd freezes as above
CONFIG_TMPFS_XATTR=y      # mandatory
CONFIG_TMPFS_POSIX_ACL=y
CONFIG_FHANDLE=y          # already set by the defconfig
CONFIG_MEMCG=y
CONFIG_CGROUP_SCHED=y
CONFIG_CGROUP_PIDS=y
CONFIG_AUTOFS_FS=y        # optional; silences "Failed to find module autofs4"
```

## Recovery once switch_root has happened

Note the asymmetry. Before `switch_root`, every failure lands in the busybox
rescue shell. **After** it, the initramfs is gone — so a rootfs that mounts but
whose init then fails (exactly the cgroups case above) leaves nothing to fall
back to.

SysRq still works, because it is handled in the kernel rather than userspace,
so `BREAK` + `b` recovers the box. That is the only route back, and it is why
`sysrq_always_enabled` stays in the boot arguments.

## Always `wipefs -a` before `mkfs`

This was got wrong here and the symptom points somewhere else entirely.

`mke2fs` writes its superblock at offset 1024 and does **not** clear the FAT
boot sector in the first 512 bytes. Formatting straight over the stick's
existing FAT32 left the partition advertising two filesystems at once:

```
# wipefs /dev/sdb1
DEVICE OFFSET TYPE UUID                                 LABEL
sdb1   0x52   vfat E3C5-4776                            SLINGUSB
sdb1   0x0    vfat E3C5-4776                            SLINGUSB
sdb1   0x1fe  vfat E3C5-4776                            SLINGUSB
sdb1   0x438  ext2 c34d2216-1191-4041-aee1-bd74224a6775 ex2200root
```

`blkid` then refuses to choose — `ambivalent result` — and returns nothing at
all. Everything that resolves a device through blkid breaks: `LABEL=` and
`UUID=` in fstab, and `/dev/disk/by-label/` and `by-uuid/` never appear. So
systemd reports

```
systemd-remount-fs[95]: mount: /: can't find LABEL=ex2200root
```

while `e2label /dev/sdb1` cheerfully prints `ex2200root`. The label is fine;
the *lookup* is poisoned by the stale superblock.

The fix is `wipefs -a /dev/sdb1` **before** `mkfs`. To repair it afterwards,
note `wipefs` refuses to touch a mounted device — so it has to be done from
the rescue initramfs with the filesystem unmounted. Busybox has no `wipefs`,
but zeroing the first 1024 bytes does the same job and is safe, because ext2
reserves that area for a boot sector and stores nothing there:

```sh
dd if=/dev/zero of=/dev/sdb1 bs=1024 count=1 conv=notrunc
```

Verified before and after at offset 510: `55 aa` became `00 00`, and the ext2
filesystem still mounted with the full tree intact.
