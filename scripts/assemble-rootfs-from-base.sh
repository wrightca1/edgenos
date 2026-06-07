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
unsquashfs -q -d "$R" "$BASE" >/dev/null   # unsquashfs creates $R itself

echo "==> removing stale switchd..."
rm -f "$R/usr/sbin/switchd"
rm -f "$R/etc/systemd/system/switchd.service"
rm -f "$R/etc/systemd/system/multi-user.target.wants/switchd.service"

echo "==> applying current overlay (services, scripts, config)..."
cp -a "$OVERLAY/." "$R/"

echo "==> installing validated edged ($(stat -c%s "$EDGED") bytes)..."
install -D -m 755 "$EDGED" "$R/usr/sbin/edged"

echo "==> enabling services..."
W="$R/etc/systemd/system/multi-user.target.wants"
mkdir -p "$W"
for svc in platform-init.service edged.service swp-l3.service \
           fan-controller.service sshd-keygen.service; do
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
