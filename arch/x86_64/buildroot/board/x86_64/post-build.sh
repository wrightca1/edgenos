#!/bin/sh
# EdgeNOS x86_64 base: post-build fixups on TARGET_DIR (runs before the rootfs image
# is made). Keep this minimal and generic: anything board/platform-specific belongs in
# the platform-svc package, not the base.
set -e
TARGET="$1"
BOARD_DIR="$(dirname "$0")"

# sshd: allow root login (lab/virtual default; the on-box password-change framework
# the maintainer plans will tighten this later).
if [ -f "$TARGET/etc/ssh/sshd_config" ]; then
    sed -i 's/^#\?PermitRootLogin .*/PermitRootLogin yes/' "$TARGET/etc/ssh/sshd_config"
    grep -q '^PermitRootLogin' "$TARGET/etc/ssh/sshd_config" || echo 'PermitRootLogin yes' >> "$TARGET/etc/ssh/sshd_config"
fi

# Where the boot partition and the persistence partition get mounted by the initramfs.
mkdir -p "$TARGET/boot" "$TARGET/mnt/persist" "$TARGET/etc/edgenos" "$TARGET/opt/edgenos" "$TARGET/etc/quagga" "$TARGET/var/lib/edgenos"

# No Buildroot-default eth0 DHCP (we name and address ports in platform-svc / networkd).
rm -f "$TARGET/etc/systemd/network/80-dhcp.network" "$TARGET/usr/lib/systemd/network/80-dhcp.network" 2>/dev/null || true

# Drop the stock predictable-name policy: EdgeNOS names ports itself (ma1, ge0..N).
mkdir -p "$TARGET/etc/systemd/network"
ln -sf /dev/null "$TARGET/etc/systemd/network/99-default.link"

# /etc/os-release and /etc/edgenos/version.json are stamped by imgbuild per platform;
# leave a marker so a raw base boot is recognisable.
echo 'EDGENOS_BASE=x86_64' > "$TARGET/etc/edgenos/base"

# machine-id must be generated per box (lives on the overlay), not baked into the base.
rm -f "$TARGET/etc/machine-id"; : > "$TARGET/etc/machine-id"
