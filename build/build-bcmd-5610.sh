#!/bin/bash
# Build bcm.user for the AS5610 WITH the custom BDE adapter (bcm5610_bde.c), so the SDK
# attaches the chip through edged's proven /dev/linux-kernel-bde (PAXB sub-window + PPC
# barriers). Run build-sdk-5610.sh first to build the 159 libs; this is incremental —
# only socdiag.o + the final link rerun (~1 min).
#
# On the box:  stop edged (frees the chip), ensure the kernel BDE module is loaded, then
#   BCM5610_BDE=1 ./bcm.user
# and in the diag shell:  init all   (runs soc_init = the correct FP init),  then  fp show.
set -uo pipefail
SDK=${SDK:-/home/smiley/edgecore/OpenBCM/sdk-6.5.16}
IMG=${IMG:-sdk5610build:1}
ADAPTER=${ADAPTER:-/home/smiley/edgecore/edgenos/asic/bcm56846/sdk_bde/bcm5610_bde.c}
export DOCKER_HOST=${DOCKER_HOST:-unix:///run/user/1000/docker.sock}

cp "$ADAPTER" "$SDK/systems/linux/user/common/bcm5610_bde.c"

CF="-Wno-error -Wno-cpp -Wno-address-of-packed-member -Wno-stringop-overflow -Wno-stringop-truncation -Wno-array-bounds -Wno-format-truncation -I/sdk/systems/bde/linux/include -I/sdk/systems/linux/kernel/modules/include"

docker run --rm -v "$SDK":/sdk "$IMG" bash -c '
  set -e
  sed -i "s/^ALLOWED_MAKE_VERSIONS :=.*/ALLOWED_MAKE_VERSIONS :=3.81 3.82 4.0 4.1 4.2 4.3 4.4/" /sdk/make/Make.config
  sed -i "s/-Wall -Werror/-Wall -Wno-error/g; s/-Werror=format-security/-Wno-error/g" /sdk/make/Makefile.unix-user
  sed -i "s/^LIBS =-lnsl -pthread -lm -lrt/LIBS =-Wl,-z,muldefs -pthread -lm -lrt/" /sdk/make/Makefile.unix-user
  mkdir -p /sdk/build/linux/user/common
  cd /sdk/systems/linux/user/common
  # One-time: patch bde_create() to branch to our adapter, and append the adapter source.
  if ! grep -q bcm5610_bde_create socdiag.c; then
    perl -0pi -e "s/return linux_bde_create\(&bus, &bde\);/{ extern int bcm5610_bde_create(ibde_t **); if (getenv(\"BCM5610_BDE\")) return bcm5610_bde_create(\&bde); }\n    return linux_bde_create(\&bus, \&bde);/" socdiag.c
    cat bcm5610_bde.c >> socdiag.c
  fi
  MV="SDK=/sdk platform=gto bldroot_suffix=/gto kernel_version=4_4 \
      PPC_CROSS_COMPILE=powerpc-linux-gnu- CROSS_COMPILE=powerpc-linux-gnu- \
      LINUX_MAKE_USER=1 LINUX_MAKE_SHARED_LIB=0 SHAREDLIBVER=1 ADD_TO_CFLAGS=\"'"$CF"'\""
  rm -f /sdk/build/unix-user/gto/socdiag.o /sdk/build/linux/user/common/bcm.user*
  eval make $MV -j"$(nproc)" bcm 2>&1 | tail -20
'
echo "== result =="
ls -la "$SDK"/build/linux/user/common/bcm.user 2>/dev/null && file "$SDK"/build/linux/user/common/bcm.user
