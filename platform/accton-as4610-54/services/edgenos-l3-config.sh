#!/bin/sh
# edgenos-l3-config.sh — apply persistent L3 config to the AS4610 datapath from
# /etc/edged/addrs.conf and /etc/edged/routes.conf. Modeled on the AS5610
# swp-l3-config.sh. Runs once at boot (edgenos-l3.service, After=bcmd.service):
# bcmd creates the ge*/xe* KNET netdevs during init, this waits for it, assigns
# the configured addresses (+MTU) and static routes, and brings the links up.
#
# We do NOT restart bcmd here: bcmd's live RTM_NEWADDR handler programs the L3
# local-host CPU-punt on each `ip addr add`, and restarting bcmd recreates the
# netdevs (wiping the IPs we just set). Idempotent: re-running skips what's set.
#
# Config dir can be overridden (EDGENOS_ETC) so the same script works whether the
# confs live in the image at /etc/edged or staged in /opt/edgenos/edgenos.
ETC="${EDGENOS_ETC:-/etc/edged}"
ACONF="$ETC/addrs.conf"
RCONF="$ETC/routes.conf"
log() { logger -t edgenos-l3 "$*" 2>/dev/null; echo "edgenos-l3: $*"; }

[ -r "$ACONF" ] || { log "no $ACONF, nothing to do"; exit 0; }

# ── Wait for bcmd readiness (race-free boot) ─────────────────────────────────
# bcmd prints "datapath UP" to /tmp/bcmd.log once it has created every front-port
# netdev and opened its netlink handler (~20-25s into init). Gate on that.
LOG=/tmp/bcmd.log
i=0
while ! grep -q "datapath UP" "$LOG" 2>/dev/null; do
    i=$((i + 1))
    if [ "$i" -gt 120 ]; then log "WARNING: bcmd 'datapath UP' absent after 120s — best-effort"; break; fi
    sleep 1
done
grep -q "datapath UP" "$LOG" 2>/dev/null && log "bcmd ready — applying L3 config"

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

# ── static routes (bcmd mirrors each into the chip via RTM_NEWROUTE) ──────────
if [ -r "$RCONF" ]; then
    while read -r dst gwdev _rest; do
        case "$dst" in ''|\#*) continue ;; esac
        [ -n "$gwdev" ] || continue
        gw="${gwdev%%:*}"; dev="${gwdev##*:}"
        [ -n "$gw" ] && [ -n "$dev" ] || continue
        ping -c1 -W2 "$gw" >/dev/null 2>&1     # resolve gw so bcmd programs its egress
        ip route replace "$dst" via "$gw" dev "$dev" 2>/dev/null \
            && log "route $dst via $gw dev $dev" || log "route $dst FAILED (gw reachable?)"
    done < "$RCONF"
fi
log "done"
