#!/usr/bin/env bash
# Build (or rebuild) every EdgeNOS builder image.
#
# The images the build scripts reference used to exist only as built images on the
# old build host. When that host was lost, so were they — the build scripts
# survived in git but had nothing to run in. These Dockerfiles are the fix: the
# builders are now reproducible from the repo.
#
# Usage:
#   docker/build-images.sh              # build all three
#   docker/build-images.sh builder9     # build one (builder9 | sdk5610 | quagga)
#
# Host setup (rootless docker) is a separate, one-time step: docker/setup-host.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)

# Match the tags the build scripts default to. Changing a tag here means changing
# the corresponding IMG default in build/*.sh, so don't.
IMG_BUILDER9="edgenos/builder9:1.8-rootless"   # build/build-{bcmd,fit-4610,fit-5610,onie-installer-4610}.sh
IMG_SDK5610="sdk5610build:1"                   # build/build-{sdk,bcmd}-5610.sh
IMG_QUAGGA="edgenos-builder"                   # core/control-plane/build-quagga.sh

# Rootless docker socket, as on the old host (the build scripts assume this too).
export DOCKER_HOST=${DOCKER_HOST:-unix:///run/user/$(id -u)/docker.sock}

# --network host for the build steps: on the LXC build host the rootless daemon's
# slirp4netns namespace has no working egress, so apt-get inside a private build
# netns cannot reach the archives. Harmless on a normal host. See docker/README.md.
NETWORK=${NETWORK:-host}

build() {
  local tag=$1 dockerfile=$2
  echo "== building $tag =="
  docker build --network "$NETWORK" -t "$tag" -f "$HERE/$dockerfile" "$HERE"
  echo "== ok: $tag =="
}

what=${1:-all}
case "$what" in
  builder9) build "$IMG_BUILDER9" Dockerfile.builder9 ;;
  sdk5610)  build "$IMG_SDK5610"  Dockerfile.sdk5610 ;;
  quagga)   build "$IMG_QUAGGA"   Dockerfile.edgenos-builder ;;
  all)
    build "$IMG_BUILDER9" Dockerfile.builder9
    build "$IMG_SDK5610"  Dockerfile.sdk5610
    build "$IMG_QUAGGA"   Dockerfile.edgenos-builder
    ;;
  *) echo "usage: $0 [all|builder9|sdk5610|quagga]" >&2; exit 2 ;;
esac

echo
docker images --format 'table {{.Repository}}:{{.Tag}}\t{{.Size}}\t{{.CreatedSince}}' \
  | grep -E "REPOSITORY|edgenos|sdk5610build" || true
