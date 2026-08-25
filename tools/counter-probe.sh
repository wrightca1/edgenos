#!/bin/sh
# counter-probe.sh - measure which chip counters respond to which traffic, with
# the controls that three hand-rolled attempts got wrong.
#
# Runs ON the switch. Emits raw before/after samples; analyse with
# tools/counter-probe.py.
#
# WHY THIS EXISTS
#
# Hand-rolled counter experiments in this project produced three clean-looking
# results that did not survive a control:
#
#   - "0 of 65,792 counters changed"   -- the diff used `join`, which silently
#                                         emits nothing on these files. 78 had moved.
#   - "these five counters are ARP-only" -- true against one other class, false as
#                                         soon as a third was added.
#   - "these counters halved with frame size" -- the two windows contained
#                                         different amounts of background traffic.
#
# So this script enforces: an idle baseline around EVERY run, N repetitions, and
# raw output that lets the analyser discard anything that also moves at idle.
#
#   counter-probe.sh <addrlist> <reps> <label>=<command> [<label>=<command> ...]
#
# Example:
#   counter-probe.sh /mnt/flash/stats_full.txt 3 \
#       ipv4="ping -c 20 -W 1 10.0.0.1" \
#       arp="arping -c 20 -I et1 10.0.0.1"
#
# ⚠ Takes no addresses of its own: every target comes from the command you pass,
# so nothing site-specific lives in this file.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
DUMP=/mnt/flash/csrdump
LIST=${1:?usage: counter-probe.sh <addrlist> <reps> <label>=<cmd> ...}
REPS=${2:?}
shift 2
[ -x "$DUMP" ] || { echo "need $DUMP" >&2; exit 1; }

sample() { "$DUMP" "$LIST" 2>/dev/null; }

# IDLE_SETTLE is deliberately as long as the traffic runs: a background source
# (OSPF hellos every few seconds here) must get the same chance to appear in the
# baseline window as in the measurement window, or its packets look like signal.
IDLE=${IDLE_SETTLE:-6}

r=1
while [ "$r" -le "$REPS" ]; do
    for spec in "$@"; do
        label=${spec%%=*}
        cmd=${spec#*=}

        sample > /tmp/cp.a
        sleep "$IDLE"
        sample > /tmp/cp.b
        echo "=== rep$r $label IDLE"
        awk 'NR==FNR{v[$1]=$2;next} ($1 in v)&&v[$1]!=$2 {print $1, v[$1], $2}' /tmp/cp.a /tmp/cp.b

        sample > /tmp/cp.a
        _t0=$(date +%s)
        eval "$cmd" >/dev/null 2>&1
        sleep 2
        _el=$(( $(date +%s) - _t0 ))
        sample > /tmp/cp.b
        # An idle window shorter than the run window gives background traffic
        # less chance to appear in the baseline than in the measurement, which
        # makes steady background look like signal. Say so rather than let it
        # pass silently -- this is the easiest way to get a wrong answer here.
        if [ "$IDLE" -lt "$_el" ]; then
            echo "# WARN $label: idle ${IDLE}s < run ${_el}s -- baseline under-samples background"
        fi
        echo "=== rep$r $label RUN"
        awk 'NR==FNR{v[$1]=$2;next} ($1 in v)&&v[$1]!=$2 {print $1, v[$1], $2}' /tmp/cp.a /tmp/cp.b
    done
    r=$((r + 1))
done
