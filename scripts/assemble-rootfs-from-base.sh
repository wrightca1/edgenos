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

echo "==> installing kernel modules (matched to uImage)..."
# build-kmodules.sh rebuilds the kernel (uImage+dtb), the out-of-tree platform
# modules, AND a full versioned module tree (in-tree =m modules + modules.dep)
# under output/kernel/modules/lib/modules/<KVER>. On a kernel BUMP the rootfs's
# old /lib/modules/<oldver> must be replaced or every =m module (bridge, 8021q,
# nf_tables, ...) fails vermagic and modprobe breaks (no modules.dep for the new
# kernel). So: if a fresh versioned tree exists, drop the stale version dir(s)
# and install the new one; otherwise just refresh our out-of-tree .ko.
MODTREE="$SRC/output/kernel/modules/lib/modules"
if [ -d "$MODTREE" ] && ls -d "$MODTREE"/*-edgenos >/dev/null 2>&1; then
    NEWVER=$(basename "$(ls -d "$MODTREE"/*-edgenos | head -1)")
    echo "   replacing versioned module tree -> $NEWVER (dropping stale $(ls "$R/lib/modules" 2>/dev/null | grep -E '\-edgenos$' | tr '\n' ' '))"
    rm -rf "$R"/lib/modules/*-edgenos
    cp -a "$MODTREE/$NEWVER" "$R/lib/modules/$NEWVER"
    # depmod over the installed tree so modules.dep is correct for the target
    depmod -b "$R" "$NEWVER" 2>/dev/null || true
fi
# Always refresh the flat /lib/modules/extra that platform-init insmods by path.
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
           zebra.service ospfd.service nos-boot-success.service; do
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
