#!/bin/bash
# build-onie-kvm-iso.sh — build ONIE's own kvm_x86_64 recovery ISO from source (no public
# prebuilt exists any more), so the onie-x86 installer can be exercised end to end in qemu
# with tools/onie/onie-install-test.py. Needs docker (rootless or root). ~25 min on 32 cores.
#
# What it patches vs upstream ONIE (all build-env rot, nothing functional):
#   - contrib/build-env Dockerfile: Debian 9 apt sources -> archive.debian.org; +autopoint/
#     gettext (grub autoreconf), +locales en_US.UTF-8 (uClibc locale tables), misc -dev libs
#   - kvm_x86_64: Secure Boot off (SECURE_BOOT_ENABLE/EXT/GRUB=no) and its kernel/config
#     swapped for the shipped kernel/config-insecure (as machine.make documents)
set -euo pipefail
WORK=${WORK:-$PWD/output/onie}
JOBS=${JOBS:-32}
mkdir -p "$WORK"; cd "$WORK"
[ -d onie ] || git clone -q --depth 1 https://github.com/opencomputeproject/onie.git onie
cd onie
# build-env image
df=contrib/build-env/Dockerfile.edgenos
cp contrib/build-env/Dockerfile "$df"
sed -i 's|^FROM debian:9|FROM debian:9\nRUN printf "deb http://archive.debian.org/debian stretch main\\ndeb http://archive.debian.org/debian-security stretch/updates main\\n" > /etc/apt/sources.list \&\& echo '"'"'Acquire::Check-Valid-Until "false";'"'"' > /etc/apt/apt.conf.d/99archive|' "$df"
sed -i '/^# Create build user/i RUN apt-get update \&\& apt-get install -y autopoint gettext libfreetype6-dev libfuse-dev python3 quilt libelf-dev libgmp-dev libmpc-dev libmpfr-dev rsync locales \&\& sed -i "s/^# *en_US.UTF-8 UTF-8/en_US.UTF-8 UTF-8/" /etc/locale.gen \&\& locale-gen \&\& rm -rf /var/lib/apt/lists/*\nENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 LANGUAGE=en_US:en\n' "$df"
docker build -q -t edgenos/onie-build-env:stretch -f "$df" contrib/build-env
# insecure kernel config (Secure Boot off)
[ -f machine/kvm_x86_64/kernel/config.secure-orig ] || cp machine/kvm_x86_64/kernel/config machine/kvm_x86_64/kernel/config.secure-orig
cp machine/kvm_x86_64/kernel/config-insecure machine/kvm_x86_64/kernel/config
chmod -R a+rwX "$WORK"
docker run --rm -v "$WORK:/home/build/src" --user build edgenos/onie-build-env:stretch bash -lc \
  "cd /home/build/src/onie/build-config && make -j$JOBS MACHINE=kvm_x86_64 SECURE_BOOT_ENABLE=no SECURE_BOOT_EXT=no SECURE_GRUB=no all recovery-iso"
ls -l "$WORK/onie/build/images/"
echo "ISO: $WORK/onie/build/images/onie-recovery-x86_64-kvm_x86_64-r0.iso"
