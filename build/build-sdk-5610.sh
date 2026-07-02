#!/bin/bash
# Build the OpenBCM full SDK (bcm.user) for the AS5610 — PowerPC / BCM56840 (Trident+),
# user-mode. First milestone of the full-SDK port: get the SDK to compile for PPC/56840.
# Mirrors the 4610 build-bcmd.sh but targets the `gto` PPC user-mode platform. Once this
# produces a PPC bcm.user, next milestones: user-mode BDE over the 5610 PCIe, chip attach
# + soc_init (the correct FP init), then the datapath + bcm_field ACLs.
set -uo pipefail
SDK=${SDK:-/home/smiley/edgecore/OpenBCM/sdk-6.5.16}
PLAT=${PLAT:-gto}
export DOCKER_HOST=${DOCKER_HOST:-unix:///run/user/1000/docker.sock}

docker run --rm --network host -v "$SDK":/sdk debian:bullseye bash -c '
  set -e
  echo "== toolchain =="
  command -v powerpc-linux-gnu-gcc >/dev/null 2>&1 || {
    apt-get update -qq >/dev/null 2>&1
    apt-get install -y -qq gcc-powerpc-linux-gnu make perl >/dev/null 2>&1; }
  powerpc-linux-gnu-gcc --version | head -1
  # SDK rejects make >=4.2; debian:bullseye ships 4.3 — allow it.
  sed -i "s/^ALLOWED_MAKE_VERSIONS :=.*/ALLOWED_MAKE_VERSIONS :=3.81 3.82 4.0 4.1 4.2 4.3 4.4/" /sdk/make/Make.config
  cd /sdk/systems/linux/user/'"$PLAT"'
  echo "== make bcm (user-mode only, skip kernel modules) =="
  make SDK=/sdk PPC_CROSS_COMPILE=powerpc-linux-gnu- CROSS_COMPILE=powerpc-linux-gnu- \
       LINUX_MAKE_USER=1 NO_LOCAL_TARGETS=1 -j"$(nproc)" bcm 2>&1 | tail -50
'
