#!/bin/bash
# build-clab-image.sh — package the EdgeNOS qcow2 as a vrnetlab container for containerlab.
#
#   tools/containerlab/build-clab-image.sh output/images/EdgeNOS-<ver>-x86_64-kvm_x86_64-r0.qcow2
#   -> docker image vrnetlab/edgenos_vswitch:<ver>   (use with kind: generic_vm)
#
# Clones hellt/vrnetlab (pinned) into output/vrnetlab if needed, drops tools/containerlab/
# vrnetlab/edgenos in as a vendor dir, and runs its Makefile. Needs docker.
set -euo pipefail
QCOW=${1:?usage: $0 <EdgeNOS-...qcow2>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
VRNETLAB_DIR=${VRNETLAB_DIR:-$TOP/output/vrnetlab}
VRNETLAB_REF=${VRNETLAB_REF:-master}
[ -f "$QCOW" ] || { echo "no such file: $QCOW"; exit 1; }
command -v docker >/dev/null || { echo "docker not found"; exit 1; }

if [ ! -d "$VRNETLAB_DIR/.git" ]; then
    echo "==> cloning hellt/vrnetlab ($VRNETLAB_REF) -> $VRNETLAB_DIR"
    git clone -q --depth 1 --branch "$VRNETLAB_REF" https://github.com/hellt/vrnetlab.git "$VRNETLAB_DIR"
fi
rm -rf "$VRNETLAB_DIR/edgenos"
cp -r "$HERE/vrnetlab/edgenos" "$VRNETLAB_DIR/edgenos"
cp -f "$QCOW" "$VRNETLAB_DIR/edgenos/"
echo "==> make (vrnetlab)"
make -C "$VRNETLAB_DIR/edgenos"
echo "==> images:"; docker images | grep -E "vrnetlab/edgenos" || true
