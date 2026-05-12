#!/bin/bash
# build-all.sh - Complete EdgeNOS build
# Builds: kernel, initramfs, Debian rootfs, FIT image, ONIE installer
# Must run inside Docker (privileged, for debootstrap/chroot)
#
# Usage:
#   docker run --rm --privileged --network=host \
#     -v $(pwd)/output:/build/output \
#     -v $(pwd):/src:ro \
#     debian:bookworm /src/scripts/build-all.sh

set -e

SRCDIR="${SRCDIR:-/src}"
OUTDIR="${OUTDIR:-/build/output}"
KVER="5.10.224"
KSRC="/build/linux-${KVER}"
JESSIE_MIRROR="http://archive.debian.org/debian"
JOBS=$(nproc)

log() { echo "==> $*"; }

# ── Install build deps ──────────────────────────────────────
install_deps() {
    log "Installing build dependencies..."
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends \
        build-essential gcc g++ \
        gcc-powerpc-linux-gnu g++-powerpc-linux-gnu binutils-powerpc-linux-gnu \
        bc bison flex libssl-dev libelf-dev \
        u-boot-tools device-tree-compiler \
        debootstrap qemu-user-static binfmt-support \
        squashfs-tools xz-utils cpio \
        wget ca-certificates git make file \
        >/dev/null
    update-binfmts --enable 2>/dev/null || true
}

# ── Build kernel ─────────────────────────────────────────────
build_kernel() {
    log "Building kernel ${KVER}..."
    mkdir -p /build

    if [ ! -d "$KSRC" ]; then
        log "Downloading kernel..."
        wget -q -O /build/linux-${KVER}.tar.xz \
            "https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-${KVER}.tar.xz"
        tar -xf /build/linux-${KVER}.tar.xz -C /build/
        rm /build/linux-${KVER}.tar.xz
    fi

    # Install DTS
    cp "$SRCDIR/kernel/dts/as5610-52x.dts" \
       "$KSRC/arch/powerpc/boot/dts/as5610-52x.dts"
    grep -q "as5610-52x" "$KSRC/arch/powerpc/boot/dts/Makefile" || \
        echo 'dtb-$(CONFIG_PPC_85xx) += as5610-52x.dtb' >> \
        "$KSRC/arch/powerpc/boot/dts/Makefile"

    # Apply kernel patches
    for p in "$SRCDIR/kernel/patches/"*.patch; do
        [ -f "$p" ] || continue
        log "Applying $(basename "$p")..."
        (cd "$KSRC" && patch -p1 < "$p") || true
    done

    cp "$SRCDIR/config/kernel/as5610_defconfig" "$KSRC/.config"
    make -C "$KSRC" ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- olddefconfig
    make -C "$KSRC" ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- -j${JOBS} uImage dtbs modules

    mkdir -p "$OUTDIR/kernel"
    cp "$KSRC/arch/powerpc/boot/uImage" "$OUTDIR/kernel/"
    cp "$KSRC/arch/powerpc/boot/dts/as5610-52x.dtb" "$OUTDIR/kernel/"

    log "Kernel: $OUTDIR/kernel/uImage"
}

# ── Build out-of-tree platform kernel modules ───────────────
#
# These are linked against the kernel we just built. Order does not
# matter (no cross-module dependencies); each invocation is a one-shot
# make -C $KSRC M=<dir> modules.
build_platform_modules() {
    log "Building out-of-tree kernel modules (bde, tmon, cpld, retimer)..."

    # Copy the source into the build area so we can write .ko alongside
    # without dirtying the read-only /src bind mount. /src is mounted ro
    # but the build needs to drop .ko/.o files into the module dir.
    local MODSTAGE="/build/modules-src"
    rm -rf "$MODSTAGE"
    mkdir -p "$MODSTAGE"
    cp -a "$SRCDIR/asic/bde"        "$MODSTAGE/bde"
    cp -a "$SRCDIR/asic/tmon"       "$MODSTAGE/tmon"
    cp -a "$SRCDIR/platform/cpld"   "$MODSTAGE/cpld"
    cp -a "$SRCDIR/platform/retimer" "$MODSTAGE/retimer"

    local KMAKE="make -C $KSRC ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu-"

    for mod in bde tmon cpld retimer; do
        log "  → $mod"
        if ! $KMAKE M="$MODSTAGE/$mod" modules; then
            log "ERROR: kernel module $mod failed to build"
            exit 1
        fi
    done

    # Stage the .ko's where build_rootfs() will copy them from.
    mkdir -p "$OUTDIR/modules"
    find "$MODSTAGE" -name '*.ko' -exec cp {} "$OUTDIR/modules/" \;
    log "Built $(ls "$OUTDIR/modules/" | wc -l) kernel module(s)"
}

# ── Build edged userspace daemon (cross-compile) ────────────
#
# The Makefile's `make edged` chain expects to write into /src/asic/...
# which is bind-mounted read-only here. Stage the source into /build
# first, build openmdk (CDK/BMD/PHY libs), then build edged itself.
build_edged() {
    log "Building edged daemon (cross-compile to powerpc)..."
    local EDGEDSRC="/build/edged-src"
    rm -rf "$EDGEDSRC"
    cp -a "$SRCDIR" "$EDGEDSRC"

    # OpenMDK libs first (writes into asic/openmdk/.../*.a; needed by edged)
    if [ -d "$EDGEDSRC/asic/openmdk" ]; then
        log "  → OpenMDK (CDK/BMD/PHY)"
        if ! "$EDGEDSRC/scripts/build-sdk.sh" 2>&1 | tail -20; then
            log "ERROR: OpenMDK build failed"
            return 1
        fi
    else
        log "WARN: asic/openmdk not present, skipping edged build"
        return 0
    fi

    log "  → edged"
    if ! make -C "$EDGEDSRC/asic/edged" \
        CROSS_COMPILE=powerpc-linux-gnu- \
        OPENMDK="$EDGEDSRC/asic/openmdk" \
        TOPDIR="$EDGEDSRC" \
        SDK_BLDDIR="$EDGEDSRC/output/sdk" 2>&1 | tail -25; then
        log "ERROR: edged build failed"
        return 1
    fi

    mkdir -p "$OUTDIR/edged"
    if [ -f "$EDGEDSRC/asic/edged/edged" ]; then
        cp "$EDGEDSRC/asic/edged/edged" "$OUTDIR/edged/edged"
        log "edged: $(ls -lh "$OUTDIR/edged/edged" | awk '{print $5}')"
    else
        log "ERROR: edged binary not produced"
        return 1
    fi
}

# ── Build initramfs ──────────────────────────────────────────
build_initramfs() {
    log "Building initramfs..."

    local INITROOT="/build/initramfs-root"
    rm -rf "$INITROOT"
    mkdir -p "$INITROOT"/{bin,sbin,dev,proc,sys,newroot,lower,rw}

    # Compile static init.
    #
    # BOOT.md captures why we use the libc variant (initramfs-nos-init.c)
    # rather than the raw-syscall version under initramfs/. The raw
    # version defines its own _start, which collides with crt1.o unless
    # built with -nostartfiles + -nostdlib + -lgcc — and even then it
    # crashes on PPC e500v2 due to missing stack-frame setup.  The libc
    # variant is larger (~767 K) but proven on this hardware.
    powerpc-linux-gnu-gcc -static -Os \
        -o "$INITROOT/init" "$SRCDIR/initramfs-nos-init.c"
    chmod +x "$INITROOT/init"

    # Create cpio archive
    mkdir -p "$OUTDIR/images"
    (cd "$INITROOT" && find . | cpio -o -H newc) | gzip -9 > "$OUTDIR/images/initramfs.cpio.gz"
    log "Initramfs: $OUTDIR/images/initramfs.cpio.gz ($(du -sh "$OUTDIR/images/initramfs.cpio.gz" | cut -f1))"
}

# ── Build Debian rootfs ──────────────────────────────────────
build_rootfs() {
    log "Building Debian jessie rootfs (powerpc)..."

    local STAGING="/build/rootfs-staging"
    rm -rf "$STAGING"
    mkdir -p "$STAGING"

    # Stage 1: debootstrap with key packages included
    log "debootstrap stage 1..."
    # Stage-1 --include= runs natively (fast); use this for everything we
    # actually need.  Don't try to install more later under qemu-ppc — it
    # hangs.  tcpdump/less/ca-certificates/python added here.
    debootstrap --arch=powerpc --foreign --no-check-gpg \
        --include=systemd,systemd-sysv,dbus,openssh-server,iproute2,isc-dhcp-client,ethtool,i2c-tools,pciutils,u-boot-tools,net-tools,nano,kmod,procps,less,ca-certificates \
        jessie "$STAGING" "$JESSIE_MIRROR"

    # Copy qemu for PPC32 execution in chroot
    local QEMU=""
    for q in /usr/bin/qemu-ppc-static /usr/bin/qemu-powerpc-static; do
        [ -x "$q" ] && QEMU="$q" && break
    done
    [ -z "$QEMU" ] && { echo "ERROR: no qemu-ppc-static"; exit 1; }
    cp "$QEMU" "$STAGING/usr/bin/"

    # Stage 2
    log "debootstrap stage 2..."
    DEBIAN_FRONTEND=noninteractive chroot "$STAGING" /debootstrap/debootstrap --second-stage

    # Configure apt - use [trusted=yes] to skip GPG, force IPv4
    cat > "$STAGING/etc/apt/sources.list" <<EOF
deb [trusted=yes] http://archive.debian.org/debian jessie main
EOF
    mkdir -p "$STAGING/etc/apt/apt.conf.d"
    cat > "$STAGING/etc/apt/apt.conf.d/99nosecurity" <<EOF
Acquire::Check-Valid-Until "false";
Acquire::AllowInsecureRepositories "true";
APT::Get::AllowUnauthenticated "true";
Acquire::ForceIPv4 "true";
Acquire::Retries "3";
EOF

    # Chroot mounts
    cp /etc/resolv.conf "$STAGING/etc/resolv.conf"
    mount -t proc proc "$STAGING/proc" 2>/dev/null || true
    mount -o bind /dev "$STAGING/dev" 2>/dev/null || true
    mount -o bind /sys "$STAGING/sys" 2>/dev/null || true
    printf '#!/bin/sh\nexit 101\n' > "$STAGING/usr/sbin/policy-rc.d"
    chmod +x "$STAGING/usr/sbin/policy-rc.d"

    # All required packages are already installed by debootstrap --include
    # (systemd, openssh-server, iproute2, isc-dhcp-client, ethtool, etc).
    #
    # Skipping the in-chroot `apt-get update` + `apt-get install` for
    # optional extras (tcpdump/less/python/ca-certs): on PPC32 those run
    # under qemu-ppc-static and routinely hang during apt's SSL/DNS even
    # though the mirror is fast natively.  If you actually need any of
    # those packages, add them to the debootstrap --include list above
    # (stage-1 install, runs natively, fast).
    log "Skipping in-chroot apt-get extras (debootstrap --include covered the essentials)"

    # Set root password: as5610
    echo 'root:as5610' | chroot "$STAGING" chpasswd 2>/dev/null || true

    # Allow root SSH login
    if [ -f "$STAGING/etc/ssh/sshd_config" ]; then
        sed -i 's/^#*PermitRootLogin.*/PermitRootLogin yes/' "$STAGING/etc/ssh/sshd_config"
        sed -i 's/^#*PasswordAuthentication.*/PasswordAuthentication yes/' "$STAGING/etc/ssh/sshd_config"
    fi

    # Hostname
    echo "edgenos" > "$STAGING/etc/hostname"

    # fstab
    cat > "$STAGING/etc/fstab" <<EOF
# EdgeNOS fstab (overlay root mounted by initramfs)
/dev/sda1  /mnt/persist  ext2  defaults  0  2
EOF

    # Network interfaces
    mkdir -p "$STAGING/etc/network"
    cat > "$STAGING/etc/network/interfaces" <<EOF
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet dhcp
EOF

    # Apply our overlay files
    if [ -d "$SRCDIR/config/rootfs/overlay" ]; then
        cp -a "$SRCDIR/config/rootfs/overlay"/. "$STAGING/"
    fi

    # Install ASIC config
    mkdir -p "$STAGING/etc/edged"
    cp "$SRCDIR/config/bcm/"* "$STAGING/etc/edged/" 2>/dev/null || true

    # Install edged binary if built
    [ -f "$OUTDIR/edged/edged" ] && \
        install -m 755 "$OUTDIR/edged/edged" "$STAGING/usr/sbin/edged"

    # Install platform modules into the kernel-version-stamped directory
    # so depmod / modprobe / systemd-modules-load can find them.
    local KVER="${KVER:-${KVER_DETECTED:-}}"
    if [ -z "$KVER" ]; then
        KVER=$(make -C "$KSRC" -s kernelrelease ARCH=powerpc \
                CROSS_COMPILE=powerpc-linux-gnu- 2>/dev/null \
                || cat "$KSRC/include/config/kernel.release" 2>/dev/null \
                || echo "5.10.224-edgenos")
    fi
    log "Installing kernel modules for KVER=$KVER"
    local KMOD_DIR="$STAGING/lib/modules/$KVER/extra"
    mkdir -p "$KMOD_DIR"
    if [ -d "$OUTDIR/modules" ] && [ "$(ls -A "$OUTDIR/modules" 2>/dev/null)" ]; then
        cp "$OUTDIR/modules/"*.ko "$KMOD_DIR/" 2>/dev/null || true
    else
        find "$SRCDIR/platform" -name "*.ko" -exec cp {} "$KMOD_DIR/" \; 2>/dev/null || true
        find "$SRCDIR/asic/bde"  -name "*.ko" -exec cp {} "$KMOD_DIR/" \; 2>/dev/null || true
        find "$SRCDIR/asic/tmon" -name "*.ko" -exec cp {} "$KMOD_DIR/" \; 2>/dev/null || true
    fi
    # depmod needs at least the empty `modules.builtin` / `modules.order`
    # files; `make modules_install` creates them but we copied .ko's by hand.
    touch "$STAGING/lib/modules/$KVER/modules.builtin"
    touch "$STAGING/lib/modules/$KVER/modules.order"
    chroot "$STAGING" depmod -a "$KVER" 2>&1 | tail -3 || true

    # Boot success service (resets boot_count)
    mkdir -p "$STAGING/usr/sbin"
    cat > "$STAGING/usr/sbin/nos-boot-success.sh" <<'BOOTOK'
#!/bin/sh
MTD_DEV=""
if [ -f /proc/mtd ]; then
    MTD_DEV=$(awk -F: '/"[uU][bB]oot.env|u.boot.env"/ { sub(/^mtd/, "/dev/mtd", $1); print $1; exit }' /proc/mtd)
fi
MTD_DEV="${MTD_DEV:-/dev/mtd1}"
[ -e "$MTD_DEV" ] || exit 0
echo "$MTD_DEV  0x000000  0x010000  0x010000" > /etc/fw_env.config
fw_setenv boot_count 0 2>/dev/null && echo "nos-boot-success: boot_count reset" || true
BOOTOK
    chmod +x "$STAGING/usr/sbin/nos-boot-success.sh"

    cat > "$STAGING/etc/systemd/system/nos-boot-success.service" <<EOF
[Unit]
Description=Reset U-Boot boot_count on successful startup
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/usr/sbin/nos-boot-success.sh
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF

    # Enable services. Cannot run `systemctl enable` against the staged
    # rootfs from outside (the host's systemctl talks to PID 1 here), so
    # symlink the WantedBy=multi-user.target relationships by hand.
    local WANTS="$STAGING/etc/systemd/system/multi-user.target.wants"
    mkdir -p "$WANTS"
    for svc in nos-boot-success platform-init edged; do
        if [ -f "$STAGING/etc/systemd/system/${svc}.service" ]; then
            ln -sf "/etc/systemd/system/${svc}.service" "$WANTS/${svc}.service"
            log "  enabled: ${svc}.service"
        fi
    done

    # Strip AmbientCapabilities= (systemd v229+) for jessie's systemd v215.
    sed -i 's/^AmbientCapabilities=/#AmbientCapabilities=/' \
        "$STAGING/etc/systemd/system/edged.service" 2>/dev/null || true

    # Persist mount point
    mkdir -p "$STAGING/mnt/persist"

    # Cleanup
    rm -f "$STAGING/usr/bin/qemu-ppc-static" "$STAGING/usr/bin/qemu-powerpc-static"
    rm -f "$STAGING/usr/sbin/policy-rc.d"
    for pid in $(ls /proc/ 2>/dev/null | grep -E '^[0-9]+$'); do
        root=$(readlink /proc/$pid/root 2>/dev/null || true)
        [ "$root" = "$STAGING" ] && kill -9 "$pid" 2>/dev/null || true
    done
    umount -l "$STAGING/proc" 2>/dev/null || true
    umount -l "$STAGING/dev" 2>/dev/null || true
    umount -l "$STAGING/sys" 2>/dev/null || true

    # Pack squashfs
    log "Packing squashfs..."
    mkdir -p "$OUTDIR/images"
    mksquashfs "$STAGING" "$OUTDIR/images/rootfs.sqsh" \
        -comp xz -noappend -no-duplicates -no-xattrs
    log "Rootfs: $OUTDIR/images/rootfs.sqsh ($(du -sh "$OUTDIR/images/rootfs.sqsh" | cut -f1))"
}

# ── Build FIT image ──────────────────────────────────────────
build_fit() {
    log "Building FIT image..."

    local FITDIR="$OUTDIR/images/fitbuild"
    mkdir -p "$FITDIR"

    # Extract raw gzip kernel from uImage
    dumpimage -T kernel -p 0 -o "$FITDIR/kernel.gz" "$OUTDIR/kernel/uImage" || \
        dd if="$OUTDIR/kernel/uImage" of="$FITDIR/kernel.gz" bs=64 skip=1 2>/dev/null

    cp "$OUTDIR/kernel/as5610-52x.dtb" "$FITDIR/as5610_52x.dtb"
    cp "$OUTDIR/images/initramfs.cpio.gz" "$FITDIR/"

    cat > "$FITDIR/nos.its" <<'ITS'
/dts-v1/;
/ {
    description = "EdgeNOS for AS5610-52X";
    #address-cells = <1>;
    images {
        kernel {
            description = "PowerPC Kernel";
            data = /incbin/("kernel.gz");
            type = "kernel";
            arch = "powerpc";
            os = "linux";
            compression = "gzip";
            load = <0x00000000>;
            entry = <0x00000000>;
            hash { algo = "crc32"; };
        };
        accton_as5610_52x_dtb {
            description = "AS5610-52X device tree";
            data = /incbin/("as5610_52x.dtb");
            type = "flat_dt";
            arch = "powerpc";
            compression = "none";
            load = <0x00f00000>;
            hash { algo = "crc32"; };
        };
        initramfs {
            description = "initramfs";
            data = /incbin/("initramfs.cpio.gz");
            type = "ramdisk";
            arch = "powerpc";
            os = "linux";
            compression = "none";
            load = <0x01000000>;
            hash { algo = "crc32"; };
        };
    };
    configurations {
        default = "accton_as5610_52x";
        accton_as5610_52x {
            description = "EdgeNOS AS5610-52X";
            kernel = "kernel";
            fdt = "accton_as5610_52x_dtb";
            ramdisk = "initramfs";
        };
    };
};
ITS

    (cd "$FITDIR" && mkimage -f nos.its "$OUTDIR/images/uImage-powerpc.itb")
    rm -rf "$FITDIR"
    log "FIT: $OUTDIR/images/uImage-powerpc.itb ($(du -sh "$OUTDIR/images/uImage-powerpc.itb" | cut -f1))"
}

# ── Build ONIE installer ────────────────────────────────────
build_installer() {
    log "Building ONIE installer..."

    local TMPDIR=$(mktemp -d)
    cp "$OUTDIR/images/uImage-powerpc.itb" "$TMPDIR/"
    cp "$OUTDIR/images/rootfs.sqsh" "$TMPDIR/"
    (cd "$TMPDIR" && tar cf payload.tar uImage-powerpc.itb rootfs.sqsh)

    cp "$SRCDIR/installer/install.sh" "$OUTDIR/images/edgenos-as5610-52x.bin"
    cat "$TMPDIR/payload.tar" >> "$OUTDIR/images/edgenos-as5610-52x.bin"
    chmod +x "$OUTDIR/images/edgenos-as5610-52x.bin"
    rm -rf "$TMPDIR"

    log "ONIE installer: $OUTDIR/images/edgenos-as5610-52x.bin ($(du -sh "$OUTDIR/images/edgenos-as5610-52x.bin" | cut -f1))"
}

# ── Main ─────────────────────────────────────────────────────
log "EdgeNOS full build starting..."
install_deps
build_kernel
build_platform_modules
build_edged
build_initramfs
build_rootfs
build_fit
build_installer

log ""
log "============================================"
log "  BUILD COMPLETE"
log "============================================"
log ""
ls -lh "$OUTDIR/images/"
log ""
log "Install: onie-nos-install http://<server>/edgenos-as5610-52x.bin"
