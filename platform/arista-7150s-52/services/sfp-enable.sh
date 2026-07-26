#!/bin/sh
# sfp-enable.sh - turn on all SFP+ lasers on the 7150.
#
# Under our own scd-xcvr there is NO unsupported-transceiver gate (unlike EOS,
# where the enable3px/phyTxEnable policy kept 3rd-party optics dark) — clearing
# txdisable turns any SFP's TX on. See arista edgenos/PLATFORM.md.
#
# The scd driver (scd-xcvr.c) creates the transceiver GPIOs as FLAT attributes on
# the scd PCI device kobj, named "sfp<id>_<gpio>" (verified: name = "%s%u_%s",
# txdisable = bit 6, RW). So the nodes are e.g.
#   /sys/bus/pci/drivers/scd/0000:02:00.0/sfp1_txdisable ... sfp52_txdisable
# NOT a per-cage scd-xcvr*/sfp*/ subdirectory. Cages must be declared first by
# scd-setup.sh (`sfp <addr> <id>`); without that these nodes don't exist.
# SPDX-License-Identifier: GPL-2.0-or-later
set -u

# Locate the scd PCI device directory (same discovery as scd-setup.sh).
SCD=""
for d in /sys/bus/pci/drivers/scd/0000:* /sys/devices/pci*/*/scd; do
    [ -e "$d/new_object" ] && { SCD="$d"; break; }
done
if [ -z "$SCD" ]; then
    echo "sfp-enable: scd PCI device not found (scd+scd-hwmon loaded?)" >&2
    exit 1
fi

n=0
for node in "$SCD"/sfp*_txdisable ; do
    [ -w "$node" ] || continue
    echo 0 > "$node" && n=$(( n + 1 ))   # txdisable=0 => laser on
done

if [ "$n" -eq 0 ]; then
    echo "sfp-enable: no sfp*_txdisable nodes (cages declared? see scd-setup.sh)" >&2
else
    echo "sfp-enable: enabled TX on $n cages"
fi
exit 0
