#!/bin/busybox sh
# EdgeNOS bring-up userspace, Juniper EX2200-C.
# Static busybox, so there is no libc in the image and nothing to go wrong
# with a dynamic loader on a board we are still learning.

/bin/busybox --install -s /bin
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

mount -t proc     proc     /proc
mount -t sysfs    sysfs    /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs    tmpfs    /tmp

echo
echo "======================================================="
echo "  EdgeNOS bring-up userspace -- Juniper EX2200-C"
echo "  $(uname -srm)"
echo "======================================================="
echo

echo "-- interfaces"
ip -o addr show 2>/dev/null | sed 's/^/   /'
echo
echo "-- link"
for i in /sys/class/net/*; do
    n=$(basename "$i")
    [ "$n" = "lo" ] && continue
    printf "   %-8s carrier=%s speed=%s duplex=%s\n" "$n" \
        "$(cat $i/carrier 2>/dev/null)" \
        "$(cat $i/speed 2>/dev/null)" \
        "$(cat $i/duplex 2>/dev/null)"
done
echo
echo "-- memory"
grep -E "^(MemTotal|MemFree)" /proc/meminfo | sed 's/^/   /'
echo
echo "Type 'poweroff -f' or 'reboot -f' to leave. SysRq (BREAK+b) also works."
echo

# Hand over to the persistent rootfs if it is genuinely usable.
#
# Deliberately NOT done with root=/dev/sdb1 in bootargs: plain `rootwait`
# waits indefinitely, so an absent or dead stick hangs the boot with no way
# out. Here every failure path falls through to the rescue shell below, which
# needs no disk at all.
ROOTDEV=/dev/sdb1
echo "-- persistent root"
i=0
while [ $i -lt 10 ]; do
    [ -b "$ROOTDEV" ] && break          # USB enumerates asynchronously
    i=$((i + 1)); sleep 1
done

if [ -b "$ROOTDEV" ]; then
    mkdir -p /mnt/root
    if mount -t ext2 "$ROOTDEV" /mnt/root 2>/dev/null; then
        # Careful with this test. Debian is usr-merged and /sbin/init is a
        # symlink to the ABSOLUTE path /lib/systemd/systemd, which resolves
        # against the *initramfs* root while we are still here - so a plain
        # `[ -x /mnt/root/sbin/init ]` follows it into the wrong filesystem
        # and reports missing. (switch_root itself is fine; it resolves
        # /sbin/init after pivoting.) Check the real binary via the relative
        # /lib -> usr/lib link, and keep -h as the catch-all for a symlink we
        # cannot follow from here.
        if [ -x /mnt/root/lib/systemd/systemd ] ||
           [ -x /mnt/root/usr/lib/systemd/systemd ] ||
           [ -x /mnt/root/sbin/init ] || [ -h /mnt/root/sbin/init ]; then
            echo "   $ROOTDEV ok -- switching root"
            mount --move /dev  /mnt/root/dev  2>/dev/null
            mount --move /proc /mnt/root/proc 2>/dev/null
            mount --move /sys  /mnt/root/sys  2>/dev/null
            exec switch_root /mnt/root /sbin/init
        fi
        echo "   $ROOTDEV mounted but has no /sbin/init"
        umount /mnt/root
    else
        echo "   $ROOTDEV will not mount"
    fi
else
    echo "   $ROOTDEV never appeared"
fi
echo "   staying in the rescue shell"

# Configure eth0 for the rescue case only. This deliberately sits AFTER the
# switch_root attempt: ip= was removed from bootargs because a kernel-applied
# address makes the rootfs's ifup fail with "Address already assigned", and
# doing it before the handover here would recreate exactly that problem.
ip link set eth0 up 2>/dev/null
ip addr add 10.101.104.2/29 dev eth0 2>/dev/null
ip route add default via 10.101.104.1 2>/dev/null
echo "   rescue networking: $(ip -4 -o addr show eth0 2>/dev/null | awk '{print $4}')"
echo

# setsid + cttyhack gives the shell a real controlling terminal, so job
# control and Ctrl-C behave over the serial console.
exec setsid cttyhack /bin/sh
