#!/bin/sh
# sfp-enable.sh - turn on all SFP+ lasers on the 7150.
#
# Under our own scd-xcvr there is NO unsupported-transceiver gate (unlike EOS,
# where the enable3px/phyTxEnable policy kept 3rd-party optics dark) — clearing
# txdisable turns any SFP's TX on. See arista edgenos/PLATFORM.md.
# SPDX-License-Identifier: GPL-2.0-or-later
set -u

n=0
for node in /sys/bus/platform/devices/scd-xcvr*/sfp* ; do
    [ -w "$node/txdisable" ] || continue
    echo 0 > "$node/txdisable" && n=$(( n + 1 ))
done

if [ "$n" -eq 0 ]; then
    echo "sfp-enable: no scd-xcvr txdisable nodes (cages declared? see scd-setup.sh)" >&2
else
    echo "sfp-enable: enabled TX on $n cages"
fi
exit 0
