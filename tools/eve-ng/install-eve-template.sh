#!/bin/bash
# install-eve-template.sh — install the EdgeNOS image + node template on an EVE-NG host.
#
#   sudo tools/eve-ng/install-eve-template.sh EdgeNOS-<ver>-x86_64-kvm_x86_64-r0.qcow2
#
# What it does (EVE-NG Pro/CE 5.x-7.x layout):
#   /opt/unetlab/addons/qemu/edgenos-<ver>/virtioa.qcow2   the disk (virtio-blk)
#   /opt/unetlab/html/templates/{intel,amd}/edgenos.yml     the node template (same file: the
#                                                            image is generic x86-64 for both)
#   /opt/unetlab/html/includes/custom_templates.yml          registers "edgenos" in the node list
#   /opt/unetlab/wrappers/unl_wrapper -a fixpermissions
# Run it on the EVE host as root. Re-running updates the template in place.
set -euo pipefail
QCOW=${1:?usage: $0 <EdgeNOS-...qcow2>}
HERE=$(cd "$(dirname "$0")" && pwd)
TPL=$HERE/edgenos.yml
[ -f "$QCOW" ] || { echo "no such file: $QCOW"; exit 1; }
[ -f "$TPL" ] || { echo "template missing: $TPL"; exit 1; }
[ -d /opt/unetlab ] || { echo "this is not an EVE-NG host (/opt/unetlab missing)"; exit 1; }

ver=$(basename "$QCOW" | sed -E 's/^EdgeNOS-([^-]+)-.*/\1/')
[ -n "$ver" ] || ver=dev
dest=/opt/unetlab/addons/qemu/edgenos-$ver
echo "==> image  -> $dest/virtioa.qcow2"
mkdir -p "$dest"
cp -f "$QCOW" "$dest/virtioa.qcow2"

for vendor in intel amd; do
    d=/opt/unetlab/html/templates/$vendor
    [ -d "$d" ] || continue
    echo "==> template -> $d/edgenos.yml"
    cp -f "$TPL" "$d/edgenos.yml"
done

ct=/opt/unetlab/html/includes/custom_templates.yml
if [ ! -f "$ct" ]; then
    printf -- "---\ncustom_templates:\n" > "$ct"
fi
if ! grep -q "name: edgenos$" "$ct"; then
    echo "==> registering edgenos in $ct"
    printf "  - name: edgenos\n    listname: EdgeNOS (x86_64 virtual switch)\n" >> "$ct"
fi

echo "==> fixpermissions"
/opt/unetlab/wrappers/unl_wrapper -a fixpermissions >/dev/null 2>&1 || true
echo "done. In the EVE-NG GUI: Add node -> EdgeNOS (image edgenos-$ver). Console: telnet (ttyS0). Login root/edgenos."
