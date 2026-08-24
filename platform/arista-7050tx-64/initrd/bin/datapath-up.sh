#!/bin/busybox sh
# datapath-up.sh -- bring up the ASIC datapath and routing from a config file.
#
# Everything this does was previously typed by hand after every boot: start the
# SDK agent, wait for its tap interfaces, address them, start the routing
# daemons. That made a reboot an event. It is now declarative, and the config
# lives on FLASH rather than in the image so it can be changed without a
# rebuild:
#
#     /mnt/flash/datapath.conf
#
# ⚠ RUNS IN THE BACKGROUND AND MUST NEVER BLOCK BOOT. bcm_init can take twenty
# minutes with the copper PHY bus enabled, and sometimes stalls outright (see
# docs/COPPER-PORT-RX-DEAD.md). init launches this detached; every wait here is
# bounded and reports rather than hanging.
#
# ⚠ The SDK agent is NOT in the image. It is ~170 MB and the initrd is a ramdisk;
# putting it there would cost that much RAM on every boot including the ones that
# never touch the ASIC. It lives on flash and the config points at it.
set -u

CONF="${1:-/mnt/flash/datapath.conf}"
LOG=/tmp/datapath.log

log() { echo "$(cut -d. -f1 /proc/uptime)s  $*" >> $LOG; }

exec >>$LOG 2>&1
echo "=== datapath-up $(date 2>/dev/null || echo '') ==="

[ -r "$CONF" ] || { log "no $CONF -- datapath not started"; exit 0; }

# --- read the config -------------------------------------------------------
# Simple key=value, plus repeatable "iface <name> <mac> <cidr> <mtu>" lines.
enable=no; agent=""; config=""; phybus=no; ports=""; portmode=routed
leds=no
fibsync=yes; frr=no; rx=start; loopback=""; loopback6=""
IFACES=""; POLICIES=""

while read -r k v rest; do
    case "$k" in
        ''|\#*) continue ;;
        iface)  IFACES="$IFACES|$v $rest" ;;
        policy) POLICIES="$POLICIES|$v $rest" ;;
        *)      key=$(echo "$k" | cut -d= -f1); val=$(echo "$k" | cut -s -d= -f2)
                [ -n "$val" ] || val="$v"
                case "$key" in
                    enable)   enable=$val ;;   agent)    agent=$val ;;
                    config)   config=$val ;;   phybus)   phybus=$val ;;
                    leds)     leds=$val ;;
                    ports)    ports=$val ;;    portmode) portmode=$val ;;
                    fibsync)  fibsync=$val ;;  frr)      frr=$val ;;
                    rx)       rx=$val ;;   loopback) loopback=$val ;;
                    loopback6) loopback6=$val ;;
                esac ;;
    esac
done < "$CONF"

case "$enable" in yes|1|on) ;; *) log "enable=$enable -- datapath not started"; exit 0 ;; esac

[ -x "$agent" ] || { log "** agent '$agent' missing or not executable -- stopping"; exit 1; }
[ -r "$config" ] || { log "** SDK config '$config' unreadable -- stopping"; exit 1; }

# --- the retimer, before the agent -----------------------------------------
# An unprogrammed DS100KR800 passes nothing, so the 40G port sits at
# Fault(Remote) with the far end transmitting. Values come from the board's own
# description; see /etc/edgenos/retimer.conf.
if [ -x /bin/retimer-program.sh ] && [ -r /etc/edgenos/retimer.conf ]; then
    log "programming the retimer"
    /bin/retimer-program.sh 3 --program >/dev/null 2>&1 \
        && log "  retimer programmed" || log "  ** retimer programming failed"
else
    log "** no retimer script or config -- the 40G port will stay dark"
fi

# --- the SDK agent ---------------------------------------------------------
ENVS="SDKPOC_CONFIG=$config SDKPOC_BCM=1 SDKPOC_DAEMON=1 SDKPOC_RX=$rx"
ENVS="$ENVS SDKPOC_PORTMODE=$portmode"
[ -n "$ports" ] && ENVS="$ENVS SDKPOC_TAP=$ports"
case "$phybus" in yes|1|on) ENVS="$ENVS SDKPOC_PHYBUS=1" ;; esac
# Front-panel copper LEDs. Off unless asked for: driving them once cost the
# copper RECEIVE path across all 48 BCM84848 ports, because the first version
# polled bcm_port_speed_get() per PHY every 2 s and starved the MDIO bus the
# datapath depends on. It now runs off cached linkscan state, but the datapath
# is what matters and the LEDs are decoration -- so they stay opt-in.
case "$leds" in yes|1|on) ENVS="$ENVS SDKPOC_PHYLED=on" ;; esac
case "$fibsync" in yes|1|on) ENVS="$ENVS SDKPOC_FIBSYNC=1" ;; esac

log "starting agent: $ENVS $agent"
# ⚠ SDKPOC_BCM=1 is not optional. Without it every bcm_port_* call returns -3
# and the run looks like it worked (docs/LINK-UP-40G.md).
( cd /tmp && env $ENVS "$agent" > /tmp/sdkpoc.log 2>&1 ) &

# --- wait for the taps, bounded --------------------------------------------
# ⚠ Strip a range suffix as well as taking the first element. "1-48,49" cut on
# "," alone yields "1-48", and "xe$((1-48 - 1))" is "xe-48" -- an interface that
# can never appear, so the wait ran its full 45 minutes and reported failure
# while the taps had been up the whole time. Arithmetic on an unvalidated string
# fails quietly and looks like a hardware stall.
FIRST=$(echo "$ports" | cut -d, -f1 | cut -d- -f1)
[ -n "$FIRST" ] || { log "no ports configured -- agent left running, nothing to address"; exit 0; }
WANT="xe$((FIRST - 1))"
# ⚠ TEN minutes, not the 45 this used to allow. A 45 minute wait cannot tell
# "slow" from "broken", so it just delayed the bad news by three quarters of an
# hour. Measured on this board, with the copper PHY bus enabled and everything
# healthy:
#
#     t+9s    PHY bus self-test passes
#     t+54s   48 PHYs initialised   (first pass)
#     t+191s  96                    (both passes)
#     t+282s  52 taps created
#
# Under five minutes end to end. Ten is generous cover for a slower boot; past
# that, a run has hit the bcm_init stall documented in
# docs/COPPER-PORT-RX-DEAD.md and waiting longer has never once helped.
log "waiting up to 10 min for $WANT (healthy is ~5 min with the PHY bus)"

n=0
while [ $n -lt 60 ]; do
    ip link show "$WANT" >/dev/null 2>&1 && break
    n=$((n + 1)); sleep 10
done
if ! ip link show "$WANT" >/dev/null 2>&1; then
    log "** $WANT never appeared after $((n * 10))s -- see /tmp/sdkpoc.log"
    log "   healthy is ~5 min, so this is the known bcm_init stall; the agent is"
    log "   still running. Killing and restarting it has worked where waiting"
    log "   has not."
    exit 1
fi
log "taps up after $((n * 10))s"

# --- address the interfaces ------------------------------------------------
# ⚠ MTU BEFORE the routing daemons. OSPF carries the interface MTU in its
# Database Description packets and refuses to leave Exchange on a mismatch --
# both neighbours here advertise 1600, and a 1500 interface silently never forms
# an adjacency (docs/OSPF-CONTROL-PLANE.md).
echo "$IFACES" | tr '|' '\n' | while read -r name mac cidr mtu; do
    [ -n "${name:-}" ] || continue
    ip link show "$name" >/dev/null 2>&1 || { log "** $name absent, skipped"; continue; }
    [ -n "${mac:-}" ]  && ip link set "$name" address "$mac"
    [ -n "${mtu:-}" ]  && ip link set "$name" mtu "$mtu"
    ip link set "$name" up
    [ -n "${cidr:-}" ] && ip addr add "$cidr" dev "$name" 2>/dev/null
    log "  $name mac=${mac:-} mtu=${mtu:-} addr=${cidr:-}"
done

# --- loopback --------------------------------------------------------------
# Added BEFORE the daemons start, because ospfd picks its router-id at startup
# and can only use an address that already exists.
if [ -n "$loopback6" ]; then
    # IPv6 forwarding first: setting it flips accept_ra off, and doing that
    # after addresses are up can strip a SLAAC address out from under a daemon.
    sysctl -w net.ipv6.conf.all.forwarding=1 >/dev/null 2>&1
    ip -6 addr add "$loopback6" dev lo 2>/dev/null
    log "loopback6 $loopback6 on lo"
fi
if [ -n "$loopback" ]; then
    ip addr add "$loopback" dev lo 2>/dev/null
    log "loopback $loopback on lo"
fi

# --- source-based policy routes --------------------------------------------
# Applied AFTER the interfaces are addressed and BEFORE the daemons start.
# Metric increases per entry, so the first policy line for a given pair is the
# preferred path and later ones are fallbacks.
if [ -n "$POLICIES" ]; then
    ip route flush table 100 2>/dev/null
    metric=10
    echo "$POLICIES" | tr '|' '\n' | while read -r from to via dev; do
        [ -n "${from:-}" ] || continue
        ip route add "$to" via "$via" dev "$dev" table 100 metric $metric 2>/dev/null \
            && log "  policy route $to via $via dev $dev (metric $metric)"
        metric=$((metric + 10))
    done
    # One rule per distinct from/to pair; adding it twice is harmless but noisy.
    echo "$POLICIES" | tr '|' '\n' | awk 'NF {print $1, $2}' | sort -u \
    | while read -r from to; do
        ip rule del from "$from" to "$to" lookup 100 2>/dev/null
        ip rule add from "$from" to "$to" lookup 100 priority 100 2>/dev/null \
            && log "  policy rule from $from to $to -> table 100"
    done
fi

# --- routing ---------------------------------------------------------------
case "$frr" in
  yes|1|on)
    if [ -x /usr/lib/frr/zebra ] && [ -r /etc/frr/frr.conf ]; then
        mkdir -p /var/run/frr /var/log/frr /var/tmp/frr
        log "starting FRR"
        # ⚠ Do NOT discard their stderr. An earlier version sent it to
        # /dev/null and logged only "FRR daemons: 0", which said that it failed
        # but not why -- the reason was one line, "could not lookup vty group
        # frrvty", and throwing it away cost a whole boot cycle to recover.
        /usr/lib/frr/zebra -d -f /etc/frr/frr.conf -u root -g root >> $LOG 2>&1
        sleep 4
        /usr/lib/frr/ospfd -d -f /etc/frr/frr.conf -u root -g root >> $LOG 2>&1
        # OSPFv3. Started unconditionally alongside ospfd: it does nothing
        # without "router ospf6" in frr.conf, and having the process there
        # means vtysh can see it -- configuring ospf6d while vtysh does not
        # know it exists silently fails to save.
        [ -x /usr/lib/frr/ospf6d ] && \
            /usr/lib/frr/ospf6d -d -f /etc/frr/frr.conf -u root -g root >> $LOG 2>&1
        sleep 2
        n=$(ps | grep -cE '[z]ebra|[o]spfd')
        log "  FRR daemons: $n"
        [ "$n" -ge 2 ] || log "  ** FRR did not start -- see the lines above"
    else
        log "** FRR requested but not installed"
    fi ;;
  *) log "frr=$frr -- routing daemons not started" ;;
esac

# --- retire the bootstrap default ------------------------------------------
# init installs "default via <mgmt gw> dev <mgmt nic>" so the box can be
# reached before the fabric exists. It must step aside once OSPF has a default,
# and nothing does that automatically: zebra will NOT install its own default
# while a kernel route for the same prefix exists, because a kernel route is
# administrative distance 0 and OSPF is 110.
#
# ⚠ The failure this causes is invisible from the switch. The box itself still
# reaches everything -- it has the management path. But the FIB sync skips any
# route leaving by the management NIC, because that is not a switch port, so
# the CHIP ends up with no default at all. Transit traffic to the internet then
# misses in hardware, gets punted to the CPU, and Linux sends it out management
# where a fabric source address is unroutable. A host behind the switch sees
# its first hop answer and everything after it time out, while internal
# destinations work perfectly because they have specific routes.
case "$frr" in
  yes|1|on)
    i=0
    while [ $i -lt 40 ]; do
        if vtysh -c "show ip route 0.0.0.0/0" 2>/dev/null | grep -q 'Known via "ospf"'; then
            # Any default NOT leaving by a front port is the bootstrap one.
            for d in $(ip route show default 2>/dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p'); do
                case "$d" in
                    xe*) ;;
                    *) ip route del default dev "$d" 2>/dev/null &&
                       log "retired bootstrap default via $d -- OSPF default now programmed" ;;
                esac
            done
            break
        fi
        i=$((i + 1))
        sleep 2
    done
    ;;
esac

log "datapath-up complete"
