#!/bin/bash
# Build the full OpenBCM SDK for the AS5610 — PowerPC / BCM56840 (Trident+), user-mode.
# PROVEN WORKING (2026-07-02): produces 159 libs (libbcm.a w/ bcm_field, libsoc.a w/ the
# correct soc_init/FP init) + a 137MB static PPC bcm.user (ELF 32-bit MSB PowerPC).
#
# This build is long (all-chip SDK compile, ~40 min). It is INCREMENTAL — make resumes from
# the mounted /sdk volume, so in a sandbox that caps command time you can re-run this script
# repeatedly until it finishes; on a normal host it completes in one shot.
#
# Next milestones (see docs/full-sdk-port-5610.md): get bcm.user on the 5610, wire the 5610's
# user-mode BDE (mmap /dev/mem BAR0 + PAXB) into the SDK's bde_create, attach + soc_init
# (the correct IFP init), then the datapath + bcm_field ACLs.
set -uo pipefail
SDK=${SDK:-/home/smiley/edgecore/OpenBCM/sdk-6.5.16}
IMG=${IMG:-sdk5610build:1}
export DOCKER_HOST=${DOCKER_HOST:-unix:///run/user/1000/docker.sock}

# 1. Toolchain image (once) — debian:bullseye + PPC cross toolchain, baked so build chunks skip apt.
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  echo "== building toolchain image $IMG =="
  printf 'FROM debian:bullseye\nRUN apt-get update -qq && apt-get install -y -qq gcc-powerpc-linux-gnu g++-powerpc-linux-gnu make perl file >/dev/null 2>&1 && rm -rf /var/lib/apt/lists/*\n' > /tmp/sdk5610.dockerfile
  docker build -q -t "$IMG" -f /tmp/sdk5610.dockerfile /tmp
fi

# CFLAGS: -Wno-error* (gcc-10 is stricter than the SDK's era) + the BDE include paths the gto
# user build omits.  Linker: drop -lnsl (its fns are in modern libc) + -z muldefs (gcc-10
# -fno-common makes tentative defs strong -> the UNUSED chip files trident3/helix5 collide;
# our chip is trident.o, so taking the first def is harmless).
CF="-Wno-error -Wno-cpp -Wno-address-of-packed-member -Wno-stringop-overflow -Wno-stringop-truncation -Wno-array-bounds -Wno-format-truncation"
CF="$CF -I/sdk/systems/bde/linux/include -I/sdk/systems/linux/kernel/modules/include"

docker run --rm -v "$SDK":/sdk "$IMG" bash -c '
  set -e
  # SDK gate patches (idempotent):
  sed -i "s/^ALLOWED_MAKE_VERSIONS :=.*/ALLOWED_MAKE_VERSIONS :=3.81 3.82 4.0 4.1 4.2 4.3 4.4/" /sdk/make/Make.config
  sed -i "s/-Wall -Werror/-Wall -Wno-error/g; s/-Werror=format-security/-Wno-error/g" /sdk/make/Makefile.unix-user
  sed -i "s/^LIBS =-lnsl -pthread -lm -lrt/LIBS =-Wl,-z,muldefs -pthread -lm -lrt/" /sdk/make/Makefile.unix-user
  mkdir -p /sdk/build/linux/user/common
  cd /sdk/systems/linux/user/common
  MV="SDK=/sdk platform=gto bldroot_suffix=/gto kernel_version=4_4 \
      PPC_CROSS_COMPILE=powerpc-linux-gnu- CROSS_COMPILE=powerpc-linux-gnu- \
      LINUX_MAKE_USER=1 LINUX_MAKE_SHARED_LIB=0 SHAREDLIBVER=1 ADD_TO_CFLAGS=\"'"$CF"'\""
  echo "== user_libs (libbcm.a, libsoc.a, ... for PPC/56840) =="
  eval make $MV -j"$(nproc)" user_libs
  echo "== bcm (link bcm.user) =="
  eval make $MV -j"$(nproc)" bcm
'
echo "== result =="
ls -la "$SDK"/build/linux/user/common/bcm.user 2>/dev/null && \
  file "$SDK"/build/linux/user/common/bcm.user
