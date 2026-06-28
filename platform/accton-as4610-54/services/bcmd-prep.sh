#!/bin/sh
# bcmd-prep.sh — platform prep that must run BEFORE bcmd (ExecStartPre of
# bcmd.service): unbind the built-in CMIC, load the GPL BDE + KNET modules
# (KNET default_mtu=1600 so the netdevs match the 1600-MTU peer network), create
# the char devnodes, and deassert the external-PHY CPLD reset. Idempotent.
set -e
D=/opt/edgenos

[ -e /sys/bus/platform/drivers/iproc_cmic/48000000.iproc_cmicd ] && \
  echo 48000000.iproc_cmicd > /sys/bus/platform/drivers/iproc_cmic/unbind 2>/dev/null || true

load() { m="$1"; shift; lsmod | grep -q "^$(basename "$m" .ko | tr - _)" || insmod "$D/$m" "$@"; }
load linux-kernel-bde.ko dmasize=8M
load linux-user-bde.ko
load linux-bcm-knet.ko default_mtu=1600

for n in linux-kernel-bde linux-user-bde linux-bcm-knet; do
    maj=$(awk -v x="$n" '$2==x{print $1}' /proc/devices)
    [ -n "$maj" ] && { rm -f "/dev/$n"; mknod "/dev/$n" c "$maj" 0; chmod 644 "/dev/$n"; }
done

# external-PHY CPLD reset deassert (84758 SFP+ + 54282 copper) before bcmd init all
i2cset -f -y 0 0x30 0x07 0x02 2>/dev/null || true
i2cset -f -y 0 0x30 0x08 0x02 2>/dev/null || true
i2cset -f -y 0 0x30 0x0d 0x01 2>/dev/null || true
i2cset -f -y 0 0x30 0x19 0x00 2>/dev/null || true
i2cset -f -y 0 0x30 0x1b 0x00 2>/dev/null || true
exit 0
