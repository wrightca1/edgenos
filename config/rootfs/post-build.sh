#!/bin/bash
# post-build.sh - Buildroot post-build script
# Called with $1 = target rootfs directory

TARGET_DIR="$1"

# Set root password to 'as5610'
HASH='$6$52x8izoNf.9aB3Vd$azJoPieNNwYutepMslp9J.32/wB0pGCdd5lxeiz9J8jhoBdqwllvIvNIvyGYnCWfYuVZ4LBP9970NCzaymfsI/'
sed -i "s|^root:[^:]*:|root:${HASH}:|" "${TARGET_DIR}/etc/shadow"

# Ensure root account is not locked
sed -i 's|^root:!:|root:'"${HASH}"':|' "${TARGET_DIR}/etc/shadow"

# Allow root login via SSH (for initial setup)
if [ -f "${TARGET_DIR}/etc/ssh/sshd_config" ]; then
    sed -i 's/^#PermitRootLogin.*/PermitRootLogin yes/' "${TARGET_DIR}/etc/ssh/sshd_config"
    # If not present, add it
    grep -q "^PermitRootLogin" "${TARGET_DIR}/etc/ssh/sshd_config" || \
        echo "PermitRootLogin yes" >> "${TARGET_DIR}/etc/ssh/sshd_config"
fi

# Enable systemd services
SYSD="${TARGET_DIR}/etc/systemd/system"
WANTS="${SYSD}/multi-user.target.wants"
mkdir -p "$WANTS"

# Enable sshd
[ -f "${TARGET_DIR}/usr/lib/systemd/system/sshd.service" ] && \
    ln -sf /usr/lib/systemd/system/sshd.service "$WANTS/sshd.service" 2>/dev/null

# Enable SSH keygen
[ -f "${SYSD}/sshd-keygen.service" ] && \
    ln -sf /etc/systemd/system/sshd-keygen.service "$WANTS/sshd-keygen.service" 2>/dev/null

# Enable systemd-networkd for DHCP
ln -sf /usr/lib/systemd/system/systemd-networkd.service "$WANTS/systemd-networkd.service" 2>/dev/null
ln -sf /usr/lib/systemd/system/systemd-resolved.service "$WANTS/systemd-resolved.service" 2>/dev/null

# Enable platform-init and switchd
[ -f "${SYSD}/platform-init.service" ] && \
    ln -sf /etc/systemd/system/platform-init.service "$WANTS/platform-init.service" 2>/dev/null
[ -f "${SYSD}/switchd.service" ] && \
    ln -sf /etc/systemd/system/switchd.service "$WANTS/switchd.service" 2>/dev/null
[ -f "${SYSD}/thermal-mgmt.service" ] && \
    ln -sf /etc/systemd/system/thermal-mgmt.service "$WANTS/thermal-mgmt.service" 2>/dev/null

# Set hostname
echo "edgenos" > "${TARGET_DIR}/etc/hostname"
