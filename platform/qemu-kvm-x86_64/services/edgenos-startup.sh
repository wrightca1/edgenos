#!/bin/sh
# edgenos-startup.sh — apply the node's startup config from /etc/edgenos/startup/ at boot.
#
#   netconf.sh   (optional) shell: interfaces/bridges/VXLAN/VRF/bonds/addresses (iproute2),
#                run every boot, must be idempotent
#   frr.conf     (optional) FRR integrated config -> installed as /etc/frr/frr.conf
#   daemons      (optional) FRR daemons file -> /etc/frr/daemons
#   sysctl.conf  (optional) extra sysctls -> applied with sysctl -p
# Pushed in by the lab tooling (vrnetlab launcher: /config -> /etc/edgenos/startup/) or by hand.
D=/etc/edgenos/startup
log() { logger -t edgenos-startup "$*" 2>/dev/null; echo "edgenos-startup: $*"; }
[ -d "$D" ] || { log "no $D, nothing to apply"; exit 0; }
if [ -f "$D/sysctl.conf" ]; then sysctl -q -p "$D/sysctl.conf" && log "sysctl applied"; fi
if [ -f "$D/daemons" ] && ! cmp -s "$D/daemons" /etc/frr/daemons; then
    install -m 0640 -o frr -g frr "$D/daemons" /etc/frr/daemons && log "frr daemons installed"
fi
if [ -f "$D/frr.conf" ] && ! cmp -s "$D/frr.conf" /etc/frr/frr.conf; then
    install -m 0640 -o frr -g frr "$D/frr.conf" /etc/frr/frr.conf && log "frr.conf installed"
fi
if [ -x "$D/netconf.sh" ] || [ -f "$D/netconf.sh" ]; then
    sh "$D/netconf.sh" >/run/edgenos/netconf.log 2>&1 && log "netconf.sh applied" || log "WARNING: netconf.sh returned $? (see /run/edgenos/netconf.log)"
fi
if [ -n "$1" ] && [ "$1" = "--restart-frr" ]; then systemctl restart frr.service; fi
exit 0
