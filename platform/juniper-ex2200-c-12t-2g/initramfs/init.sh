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

# setsid + cttyhack gives the shell a real controlling terminal, so job
# control and Ctrl-C behave over the serial console.
exec setsid cttyhack /bin/sh
