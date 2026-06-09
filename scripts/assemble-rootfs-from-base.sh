#!/bin/bash
# assemble-rootfs-from-base.sh — rebuild output/images/rootfs.sqsh from the proven
# base rootfs + the CURRENT overlay + the validated edged, WITHOUT rebuilding
# buildroot. Used when buildroot-from-scratch is blocked (e.g. rootless-docker
# user-namespace chown failures). The base userland is unchanged; only our
# overlay (services/config) and the edged daemon change.
#
# Runs entirely inside the edgenos-builder container (needs unsquashfs/mksquashfs).
set -e
SRC=/build/src
BASE="$SRC/output/images/rootfs.sqsh"
EDGED="$SRC/output/edged-rebuilt"
OVERLAY="$SRC/config/rootfs/overlay"
R=/tmp/rootfs-assembly

[ -f "$BASE" ]  || { echo "ERROR: base rootfs missing: $BASE"; exit 1; }
[ -f "$EDGED" ] && [ "$(stat -c%s "$EDGED")" -gt 15000000 ] || { echo "ERROR: valid edged (>15MB) missing"; exit 1; }

rm -rf "$R"
echo "==> unsquashing base..."
# -no-xattrs: the build fs can't store selinux xattrs; without this unsquashfs
# prints a warning AND returns exit 2, which `set -e` would treat as fatal.
unsquashfs -q -no-xattrs -d "$R" "$BASE" >/dev/null   # unsquashfs creates $R itself

echo "==> removing stale switchd..."
rm -f "$R/usr/sbin/switchd"
rm -f "$R/etc/systemd/system/switchd.service"
rm -f "$R/etc/systemd/system/multi-user.target.wants/switchd.service"

echo "==> retiring legacy thermal-mgmt (superseded by fan-controller)..."
# thermal-mgmt.sh targets a pre-platform_driver CPLD path (ff705000.localbus/
# ea000000.cpld/pwm1) that no longer exists, and its 0-248 PWM scale would
# corrupt the 5-bit CPLD register if it ran alongside fan-controller. Fan
# control + emergency shutdown now live solely in fan-controller.sh.
rm -f "$R/etc/systemd/system/thermal-mgmt.service"
rm -f "$R/etc/systemd/system/multi-user.target.wants/thermal-mgmt.service"
rm -f "$R/usr/sbin/thermal-mgmt.sh"

echo "==> applying current overlay (services, scripts, config)..."
cp -a "$OVERLAY/." "$R/"

echo "==> installing validated edged ($(stat -c%s "$EDGED") bytes)..."
install -D -m 755 "$EDGED" "$R/usr/sbin/edged"

echo "==> installing freshly-built platform kernel modules (matched to uImage)..."
# build-kmodules.sh rebuilds the kernel (uImage+dtb) AND the out-of-tree
# platform modules together, so output/modules/*.ko are vermagic- and
# CRC-matched to output/kernel/uImage. When the kernel is rebuilt (e.g. the MTD
# change), install ALL of them over the stale base copies so the loadable
# modules match the shipped kernel — bde is datapath-critical, so it must not
# be a stale build against a different kernel. (Built from the same repo source
# as the base, so no behaviour change — only kernel-match.) This also carries
# the accton_as5610_52x_cpld fan_pwm NULL-deref fix.
if [ -d "$SRC/output/modules" ] && ls "$SRC/output/modules/"*.ko >/dev/null 2>&1; then
    for ko in "$SRC/output/modules/"*.ko; do
        install -D -m 644 "$ko" "$R/lib/modules/extra/$(basename "$ko")"
        echo "   installed $(basename "$ko") ($(stat -c%s "$ko") bytes)"
    done
else
    echo "   note: no output/modules/*.ko — keeping base modules"
fi

echo "==> installing Quagga (zebra + ospfd) for OSPF control plane..."
for qb in zebra ospfd; do
    if [ -f "$SRC/output/${qb}-ppc" ]; then
        install -D -m 755 "$SRC/output/${qb}-ppc" "$R/usr/sbin/$qb"
        echo "   installed $qb ($(stat -c%s "$SRC/output/${qb}-ppc") bytes)"
    else
        echo "   WARN: output/${qb}-ppc missing — OSPF will not be available"
    fi
done

echo "==> enabling services..."
W="$R/etc/systemd/system/multi-user.target.wants"
mkdir -p "$W"
for svc in platform-init.service edged.service swp-l3.service \
           fan-controller.service sshd-keygen.service \
           zebra.service ospfd.service; do
    if [ -f "$R/etc/systemd/system/$svc" ]; then
        ln -sf "../$svc" "$W/$svc"; echo "   enabled $svc"
    else
        echo "   WARN: $svc not present in rootfs"
    fi
done

echo "==> re-squashing..."
rm -f "$SRC/output/images/rootfs.sqsh"
mksquashfs "$R" "$SRC/output/images/rootfs.sqsh" -comp xz -noappend -all-root
echo "==> rootfs.sqsh rebuilt:"
ls -lh "$SRC/output/images/rootfs.sqsh"
rm -rf "$R"
