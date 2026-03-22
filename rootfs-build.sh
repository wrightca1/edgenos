#!/bin/bash
# Build Debian jessie (powerpc) rootfs and pack as squashfs for ONIE installer.
# Debian 8 jessie is the last Debian release with a powerpc (PPC32 BE) port.
#
# Usage:
#   ./build.sh
#   REPO_ROOT=/path BUILD_DIR=/path/build KERNEL_VERSION=5.10.0 ./build.sh
#
# Requires (on x86 build host):
#   debootstrap  +  qemu-user-static  (apt: debootstrap qemu-user-static)
#   squashfs-tools                    (apt: squashfs-tools)
#   OR: docker  (will auto-run inside debian:bookworm + install deps)
#
# Output: sysroot.squash.xz in onie-installer/ (or $ROOTFS_OUT)

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"
STAGING="${ROOTFS_STAGING:-$SCRIPT_DIR/staging}"
OUT_DIR="${ROOTFS_OUT_DIR:-$REPO_ROOT/onie-installer}"
OUT_FILE="${ROOTFS_OUT:-$OUT_DIR/sysroot.squash.xz}"
KERNEL_VERSION="${KERNEL_VERSION:-5.10.0-nos}"
BUILD_ARTIFACTS="${BUILD_ARTIFACTS:-1}"

# Debian jessie archive (EOL; use archive.debian.org)
JESSIE_MIRROR="${JESSIE_MIRROR:-http://archive.debian.org/debian}"

log() { echo "[rootfs/build] $1"; }

IN_DOCKER="${IN_DOCKER:-0}"

# --- Docker fallback ---
# debootstrap + qemu-user-static are needed to build a PPC32 rootfs on x86.
# Re-run inside debian:bookworm which can install both without issues.
# --privileged is required for binfmt_misc registration (qemu-ppc-static).
if [ "$IN_DOCKER" != "1" ] && ! command -v debootstrap &>/dev/null; then
	if ! command -v docker &>/dev/null; then
		echo "ERROR: need debootstrap (+ qemu-user-static) or Docker"
		echo "  On Debian/Ubuntu: apt-get install debootstrap qemu-user-static squashfs-tools"
		exit 1
	fi
	log "debootstrap not found; falling back to Docker (debian:bookworm)..."
	exec docker run --rm --privileged \
		-e IN_DOCKER=1 \
		-e KERNEL_VERSION="${KERNEL_VERSION}" \
		-e BUILD_ARTIFACTS="${BUILD_ARTIFACTS}" \
		-e JESSIE_MIRROR="${JESSIE_MIRROR}" \
		-v "$REPO_ROOT:/work" \
		-w /work \
		debian:bookworm \
		bash -c "
			set -e
			export DEBIAN_FRONTEND=noninteractive
			apt-get update -qq
			apt-get install -y -qq --no-install-recommends \
				debootstrap qemu-user-static binfmt-support \
				squashfs-tools xz-utils ca-certificates >/dev/null
			update-binfmts --enable 2>/dev/null || true
			exec rootfs/build.sh
		"
fi

# --- Prerequisites (native or inside Docker) ---
for cmd in debootstrap mksquashfs; do
	if ! command -v "$cmd" &>/dev/null; then
		echo "Missing: $cmd"; exit 1
	fi
done

# Find qemu-ppc-static for second-stage chroot
QEMU_STATIC=""
for q in /usr/bin/qemu-ppc-static /usr/bin/qemu-powerpc-static; do
	[ -x "$q" ] && QEMU_STATIC="$q" && break
done
if [ -z "$QEMU_STATIC" ]; then
	echo "ERROR: qemu-ppc-static not found"
	echo "  Install: apt-get install qemu-user-static"
	exit 1
fi

# --- Stage 1: debootstrap (extract; no PPC32 execution yet) ---
rm -rf "$STAGING"
mkdir -p "$STAGING"
log "Stage 1: debootstrap --arch=powerpc jessie from ${JESSIE_MIRROR}..."
debootstrap --arch=powerpc --foreign --no-check-gpg \
	jessie "$STAGING" "$JESSIE_MIRROR"

# Copy qemu-ppc-static so PPC32 binaries can run inside the chroot
cp "$QEMU_STATIC" "$STAGING/usr/bin/"

# --- Stage 2: debootstrap second stage (runs PPC32 post-install scripts) ---
log "Stage 2: debootstrap second stage (via $(basename "$QEMU_STATIC"))..."
DEBIAN_FRONTEND=noninteractive chroot "$STAGING" /debootstrap/debootstrap --second-stage

# --- Configure apt sources ---
# archive.debian.org jessie is EOL; Release file dates are in the past.
# Disable valid-until check so apt doesn't refuse to use the archive.
cat > "$STAGING/etc/apt/sources.list" <<EOF
# Debian jessie (EOL) -- only archive.debian.org/debian main works for powerpc.
# jessie-updates and jessie/updates (security) are 404 for powerpc on the archive.
deb ${JESSIE_MIRROR} jessie main
EOF
mkdir -p "$STAGING/etc/apt/apt.conf.d"
cat > "$STAGING/etc/apt/apt.conf.d/99ignore-release-date" <<EOF
Acquire::Check-Valid-Until "false";
Acquire::AllowInsecureRepositories "true";
EOF

# --- Chroot bind mounts (required for apt post-install scripts + DNS) ---
# Copy resolv.conf so the chroot can resolve archive.debian.org
cp /etc/resolv.conf "$STAGING/etc/resolv.conf"
# Bind mount /proc, /dev, /sys so post-install scripts work
mount -t proc proc "$STAGING/proc" 2>/dev/null || true
mount -o bind /dev "$STAGING/dev" 2>/dev/null || true
mount -o bind /sys "$STAGING/sys" 2>/dev/null || true
# Prevent services (dbus, sshd, etc.) from starting during apt-get install.
# Without this, dbus starts inside the chroot and keeps /proc busy so umount fails.
mkdir -p "$STAGING/usr/sbin"
printf '#!/bin/sh\nexit 101\n' > "$STAGING/usr/sbin/policy-rc.d"
chmod +x "$STAGING/usr/sbin/policy-rc.d"
# Ensure cleanup on exit
STAGING_CLEANUP="$STAGING"
chroot_cleanup() {
	# Kill any processes that may have started inside the chroot (e.g. dbus)
	# Use chroot PID namespace: find processes with /proc/PID/root pointing to staging
	for pid in $(ls /proc/ 2>/dev/null | grep -E '^[0-9]+$'); do
		root=$(readlink /proc/$pid/root 2>/dev/null || true)
		[ "$root" = "$STAGING_CLEANUP" ] && kill -9 "$pid" 2>/dev/null || true
	done
	# Lazy unmount (-l) detaches immediately even if mount is busy
	umount -l "$STAGING_CLEANUP/proc" 2>/dev/null || true
	umount -l "$STAGING_CLEANUP/dev" 2>/dev/null || true
	umount -l "$STAGING_CLEANUP/sys" 2>/dev/null || true
}
trap chroot_cleanup EXIT

# --- Install packages ---
log "Updating apt and installing packages..."
DEBIAN_FRONTEND=noninteractive chroot "$STAGING" apt-get update -q || true
DEBIAN_FRONTEND=noninteractive chroot "$STAGING" apt-get install -y -q \
	--no-install-recommends \
	--allow-unauthenticated \
	iproute2 isc-dhcp-client openssh-server ethtool tcpdump \
	i2c-tools pciutils u-boot-tools \
	python3-minimal ca-certificates \
	lldpd net-tools curl || true

# FRR is not available in Debian jessie repos (too new); install separately so failure
# doesn't block other packages. Cross-compile or use a third-party .deb if needed.
DEBIAN_FRONTEND=noninteractive chroot "$STAGING" apt-get install -y -q \
	--no-install-recommends \
	--allow-unauthenticated \
	frr 2>/dev/null || log "WARN: frr not available in jessie repos (expected)"

# Install systemd explicitly (default on jessie but ensure it's present)
DEBIAN_FRONTEND=noninteractive chroot "$STAGING" apt-get install -y -q \
	--no-install-recommends \
	--allow-unauthenticated \
	systemd systemd-sysv dbus || true

# --- Our artifacts from build server or local build ---
if [ "$BUILD_ARTIFACTS" = "1" ]; then
	BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
	if [ -f "$BUILD_DIR/sdk/libbcm56846.so" ]; then
		log "Installing libbcm56846.so, nos-switchd, BDE modules..."
		mkdir -p "$STAGING/usr/lib" "$STAGING/usr/sbin" "$STAGING/lib/modules/$KERNEL_VERSION"
		cp -f "$BUILD_DIR/sdk/libbcm56846.so" "$STAGING/usr/lib/"
		[ -f "$BUILD_DIR/switchd/nos-switchd" ] && \
			cp -f "$BUILD_DIR/switchd/nos-switchd" "$STAGING/usr/sbin/" && \
			chmod +x "$STAGING/usr/sbin/nos-switchd"
		for ko in "$REPO_ROOT/bde/nos_kernel_bde.ko" "$REPO_ROOT/bde/nos_user_bde.ko" \
		         "$REPO_ROOT/platform/drivers/accton_as5610_cpld.ko"; do
			[ -f "$ko" ] && cp -f "$ko" "$STAGING/lib/modules/$KERNEL_VERSION/"
		done
		# Install I2C mux module (PCA9548/PCA9546 mux expansion: 2 buses -> 70 buses)
		if [ -n "$KERNEL_SRC" ]; then
			I2C_MUX_KO="$KERNEL_SRC/drivers/i2c/muxes/i2c-mux-pca954x.ko"
			[ -f "$I2C_MUX_KO" ] && cp -f "$I2C_MUX_KO" "$STAGING/lib/modules/$KERNEL_VERSION/"
		fi
		# Install MTD/CFI modules for U-Boot env access (boot_count reset via fw_setenv)
		if [ -n "$KERNEL_SRC" ] && [ -d "$KERNEL_SRC/drivers/mtd" ]; then
			log "Installing MTD/CFI kernel modules..."
			mkdir -p "$STAGING/lib/modules/$KERNEL_VERSION/kernel/drivers/mtd/chips"
			mkdir -p "$STAGING/lib/modules/$KERNEL_VERSION/kernel/drivers/mtd/maps"
			mkdir -p "$STAGING/lib/modules/$KERNEL_VERSION/kernel/drivers/mtd/parsers"
			for ko in \
				"$KERNEL_SRC/drivers/mtd/mtd.ko" \
				"$KERNEL_SRC/drivers/mtd/chips/chipreg.ko" \
				"$KERNEL_SRC/drivers/mtd/chips/gen_probe.ko" \
				"$KERNEL_SRC/drivers/mtd/chips/cfi_util.ko" \
				"$KERNEL_SRC/drivers/mtd/chips/cfi_probe.ko" \
				"$KERNEL_SRC/drivers/mtd/chips/cfi_cmdset_0002.ko" \
				"$KERNEL_SRC/drivers/mtd/chips/cfi_cmdset_0001.ko"; do
				[ -f "$ko" ] && cp -f "$ko" "$STAGING/lib/modules/$KERNEL_VERSION/kernel/drivers/mtd/chips/"
			done
			for ko in "$KERNEL_SRC/drivers/mtd/maps/physmap.ko"; do
				[ -f "$ko" ] && cp -f "$ko" "$STAGING/lib/modules/$KERNEL_VERSION/kernel/drivers/mtd/maps/"
			done
			for ko in "$KERNEL_SRC/drivers/mtd/parsers/ofpart.ko"; do
				[ -f "$ko" ] && cp -f "$ko" "$STAGING/lib/modules/$KERNEL_VERSION/kernel/drivers/mtd/parsers/"
			done
		fi
		depmod -b "$STAGING" "$KERNEL_VERSION" 2>/dev/null || true
		chroot "$STAGING" ldconfig 2>/dev/null || true
	else
		log "BUILD_DIR ($BUILD_DIR) has no libbcm56846.so; skipping artifacts."
	fi
	# platform-mgrd (PPC32)
	if command -v powerpc-linux-gnu-gcc &>/dev/null; then
		log "Building and installing platform-mgrd..."
		make -C "$REPO_ROOT/platform/platform-mgrd" CROSS_COMPILE=powerpc-linux-gnu- clean all 2>/dev/null || true
		make -C "$REPO_ROOT/platform/platform-mgrd" DESTDIR="$STAGING" install 2>/dev/null || true
	fi
fi

# --- Default NOS config ---
mkdir -p "$STAGING/etc/nos"
[ -f "$REPO_ROOT/etc/nos/config.bcm" ] && cp -f "$REPO_ROOT/etc/nos/config.bcm" "$STAGING/etc/nos/"
[ -f "$REPO_ROOT/etc/nos/rc.soc" ] && cp -f "$REPO_ROOT/etc/nos/rc.soc" "$STAGING/etc/nos/"
[ -f "$REPO_ROOT/etc/nos/rc.datapath_0" ] && cp -f "$REPO_ROOT/etc/nos/rc.datapath_0" "$STAGING/etc/nos/"
if [ ! -f "$STAGING/etc/nos/ports.conf" ]; then
	echo "# open-nos-as5610 default: 52 ports" > "$STAGING/etc/nos/ports.conf"
	echo "swp1=10G" >> "$STAGING/etc/nos/ports.conf"
fi

# --- Overlay files ---
OVERLAY="$SCRIPT_DIR/overlay"
if [ -d "$OVERLAY" ]; then
	log "Applying overlay..."
	cp -a "$OVERLAY"/. "$STAGING/"
	# Fix ownership: overlay files may be owned by build user (uid 1000)
	# SSH requires authorized_keys owned by root; fix all overlay files
	chown -R 0:0 "$STAGING/root/.ssh" 2>/dev/null || true
fi

# --- Enable systemd services ---
WANTS="$STAGING/etc/systemd/system/multi-user.target.wants"
mkdir -p "$WANTS"
for svc in nos-bde-modules nos-switchd platform-mgrd nos-boot-success; do
	if [ -f "$STAGING/etc/systemd/system/${svc}.service" ]; then
		ln -sfT "/etc/systemd/system/${svc}.service" "$WANTS/${svc}.service"
		log "Enabled systemd service: $svc"
	fi
done

# Required mount-points referenced in fstab
mkdir -p "$STAGING/mnt/persist"

# Hostname
echo "as5610" > "$STAGING/etc/hostname"

# Allow root SSH login with password (for initial setup; restrict in production)
if [ -f "$STAGING/etc/ssh/sshd_config" ]; then
	sed -i 's/^#*PermitRootLogin.*/PermitRootLogin yes/' "$STAGING/etc/ssh/sshd_config" || true
	sed -i 's/^#*PasswordAuthentication.*/PasswordAuthentication yes/' "$STAGING/etc/ssh/sshd_config" || true
fi

# Default root password: as5610
echo 'root:as5610' | chroot "$STAGING" chpasswd 2>/dev/null || true

# Remove qemu-static and policy-rc.d from rootfs before packing
rm -f "$STAGING/usr/bin/qemu-ppc-static" "$STAGING/usr/bin/qemu-powerpc-static" 2>/dev/null || true
rm -f "$STAGING/usr/sbin/policy-rc.d" 2>/dev/null || true

# Unmount bind mounts before squashing (kills chroot processes first)
chroot_cleanup
trap - EXIT

log "Packing squashfs to $OUT_FILE..."
mkdir -p "$(dirname "$OUT_FILE")"
mksquashfs "$STAGING" "$OUT_FILE" -comp xz -noappend -no-duplicates -no-xattrs
log "Done: $OUT_FILE"
