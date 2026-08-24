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
        if [ -x /mnt/root/sbin/init ]; then
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
echo

# setsid + cttyhack gives the shell a real controlling terminal, so job
# control and Ctrl-C behave over the serial console.
exec setsid cttyhack /bin/sh
