#!/bin/sh
# swp-l3-config.sh - apply persistent swp L3 addresses from /etc/edged/swp-addrs.conf
#
# Runs once at boot (swp-l3.service, After=edged.service). edged creates the swpN
# TUN interfaces during its startup; this waits for them, assigns the configured
# addresses, and brings the links up.
#
# We do NOT restart edged here.  Two reasons (both verified 2026-06-03 on a real
# reboot):
#   1. edged's swpN interfaces are TUN devices owned by the edged process. When
#      edged stops they are DESTROYED, taking their IP addresses with them. A
#      restart-after-assign therefore *wipes* the very addresses we just set, and
#      the oneshot service has already exited so nothing re-adds them.
#   2. It isn't needed: edged's live RTM_NEWADDR handler programs the L3
#      local-host CPU-punt on each `ip addr add` (confirmed: "L3_HOST lookup OK"
#      in the edged log right after the add). It runs the same l3_local_host_add()
#      the startup RTM_GETADDR dump uses.
#
# Idempotent: re-running skips addresses already present.
CONF=/etc/edged/swp-addrs.conf
added=0
log() { logger -t swp-l3 "$*"; echo "swp-l3: $*"; }

[ -r "$CONF" ] || { log "no $CONF, nothing to do"; exit 0; }

apply_one() {
    iface="$1"; cidr="$2"; mtu="$3"
    # wait up to ~15s for the interface to exist (edged creates swpN at startup)
    i=0
    while [ ! -e "/sys/class/net/$iface" ]; do
        i=$((i + 1)); [ "$i" -gt 30 ] && { log "$iface never appeared, skipping"; return 1; }
        sleep 0.5
    done
    [ -n "$mtu" ] && ip link set "$iface" mtu "$mtu" 2>/dev/null
    if ip -o addr show "$iface" 2>/dev/null | grep -qw "${cidr%/*}"; then
        log "$iface already has ${cidr}, skip"
    else
        ip addr add "$cidr" dev "$iface" 2>/dev/null && { log "$iface += $cidr (mtu ${mtu:-default})"; added=1; }
    fi
    ip link set "$iface" up 2>/dev/null
}

while read -r iface cidr mtu _rest; do
    case "$iface" in ''|\#*) continue ;; esac
    [ -n "$cidr" ] || continue
    apply_one "$iface" "$cidr" "$mtu"
done < "$CONF"

[ "$added" = 1 ] && log "addresses added (edged live RTM_NEWADDR programs the L3 punt)"

# ── ECMP / multipath transit routes ──────────────────────────────────────────
# /etc/edged/swp-routes.conf, one route per line:
#     <dst/prefixlen>   <gw1>:<dev1>  [<gw2>:<dev2> ...]
# Multiple gw:dev pairs => ECMP. We ping each gateway first so the kernel ARP
# resolves it and edged programs that next-hop in the chip — only then does the
# `ip route add` (which edged turns into the chip ECMP/DEFIP entry) find every
# next-hop. Without the pre-resolve the route would install with missing paths.
RCONF=/etc/edged/swp-routes.conf
if [ -r "$RCONF" ]; then
    while read -r dst paths; do
        case "$dst" in ''|\#*) continue ;; esac
        [ -n "$paths" ] || continue
        nhargs=""
        for p in $paths; do
            gw="${p%%:*}"; dev="${p##*:}"
            [ -n "$gw" ] && [ -n "$dev" ] || continue
            # resolve the gateway so edged programs its chip next-hop
            ping -c1 -W2 "$gw" >/dev/null 2>&1
            nhargs="$nhargs nexthop via $gw dev $dev"
        done
        [ -n "$nhargs" ] || continue
        ip route replace "$dst" $nhargs 2>/dev/null \
            && log "route $dst ->$nhargs" \
            || log "route $dst FAILED (gateways reachable?)"
    done < "$RCONF"
fi

log "done"
