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
