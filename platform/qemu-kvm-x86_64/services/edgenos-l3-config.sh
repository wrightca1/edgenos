#!/bin/sh
# edgenos-l3-config.sh — apply persistent L3 config from /etc/edged/addrs.conf and
# /etc/edged/routes.conf on the virtual switch. Same file format and semantics as the
# AS4610/AS5610 scripts, minus the datapath-readiness wait: the kernel IS the datapath.
# Runs once at boot after port naming (edgenos-l3.service). Idempotent.
ETC="${EDGENOS_ETC:-/etc/edged}"
ACONF="$ETC/addrs.conf"
RCONF="$ETC/routes.conf"
log() { logger -t edgenos-l3 "$*" 2>/dev/null; echo "edgenos-l3: $*"; }

[ -r "$ACONF" ] || { log "no $ACONF, nothing to do"; exit 0; }

apply_one() {
    iface="$1"; cidr="$2"; mtu="$3"
    i=0
    while [ ! -e "/sys/class/net/$iface" ]; do
        i=$((i + 1)); [ "$i" -gt 30 ] && { log "$iface never appeared, skipping"; return 1; }
        sleep 0.5
    done
    [ -n "$mtu" ] && ip link set "$iface" mtu "$mtu" 2>/dev/null
    if ip -o addr show "$iface" 2>/dev/null | grep -qw "${cidr%/*}"; then
        log "$iface already has $cidr, skip"
    else
        ip addr add "$cidr" dev "$iface" 2>/dev/null && log "$iface += $cidr (mtu ${mtu:-default})"
    fi
    ip link set "$iface" up 2>/dev/null
}

while read -r iface cidr mtu _rest; do
    case "$iface" in ''|\#*) continue ;; esac
    [ -n "$cidr" ] || continue
    apply_one "$iface" "$cidr" "$mtu"
done < "$ACONF"

if [ -r "$RCONF" ]; then
    while read -r dst gwdev _rest; do
        case "$dst" in ''|\#*) continue ;; esac
        [ -n "$gwdev" ] || continue
        gw="${gwdev%%:*}"; dev="${gwdev##*:}"
        [ -n "$gw" ] && [ -n "$dev" ] || continue
        ip route replace "$dst" via "$gw" dev "$dev" 2>/dev/null \
            && log "route $dst via $gw dev $dev" || log "route $dst FAILED"
    done < "$RCONF"
fi
log "done"
