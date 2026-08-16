#!/bin/bash
# a4-driver.sh - run the MOD slice sweep to completion across the reboots it needs.
#
# The sweep exits with RESUME_FROM whenever a slice wedges forwarding (restore is
# not recovery -- see tools/README-7150-harnesses.md). Each resume needs a reboot
# AND Et2 up, which is a coin at ~50%, so this drives the whole loop.
set -u
S="$(cd "$(dirname "$0")" && pwd)"
SW="$S/sw.sh"; EG="$S/eg.sh"
IMG=flash:/edgenos-7150-0.3.0-alpha12.swi
FROM="${RESUME_FROM:-0}"
MAXBOOT="${MAXBOOT:-10}"
edge(){ timeout 90 "$EG" "$@" 2>/dev/null; }
eos(){  timeout 60 "$SW" "$@" 2>/dev/null; }
say(){ echo "$(date -u +%H:%M:%S) $*"; }

boot_edgenos(){
    if edge 'echo ok' | grep -q ok; then edge 'sync; (sleep 1; reboot -f) >/dev/null 2>&1 &' >/dev/null
    else eos 'bash sync' >/dev/null; eos 'reload now' >/dev/null; fi
    sleep 45
    t=0; until ping -c1 -W2 10.1.1.77 >/dev/null 2>&1 || [ $t -ge 40 ]; do sleep 15; t=$((t+1)); done
    sleep 60
    eos 'show version | include Uptime' | grep -q Uptime || return 1
    eos 'configure' "boot system $IMG" 'end' >/dev/null
    eos 'bash sync' >/dev/null; eos 'reload now' >/dev/null
    sleep 40
    t=0; until edge 'echo ok' | grep -q ok || [ $t -ge 40 ]; do sleep 15; t=$((t+1)); done
    edge 'echo ok' | grep -q ok || return 1
    t=0; until edge 'grep -q "FULLSEQ DONE" /var/log/fm6000-fullseq && echo d' | grep -q d || [ $t -ge 40 ]; do sleep 20; t=$((t+1)); done
    return 0
}

for attempt in $(seq 1 "$MAXBOOT"); do
    say "--- boot attempt $attempt, resuming sweep at slice $FROM ---"
    boot_edgenos || { say "  boot failed, retrying"; continue; }
    st=$(edge '/mnt/flash/fmdump 0xe4000 1' | awk '{print $2}')
    case "$st" in
        *08c0|*0cc0|*0ac0|*0ec0) say "  Et2 up ($st)" ;;
        *) say "  Et2 dark ($st) -- rebooting"; continue ;;
    esac
    edge 'sh /usr/lib/edgenos/platform/edgenos-up.sh >/tmp/up.log 2>&1; tail -1 /tmp/up.log' >/dev/null
    # Warm the ARP both ways before probing. The sweep's baseline needs the peer
    # to resolve 10.101.101.34, which needs et2 RX; probing immediately after
    # bring-up fails on a cold neighbour table and looks like a broken box.
    edge 'ping -c3 -W1 -I et2 10.101.101.33 >/dev/null 2>&1'
    timeout 60 "$S/p5.sh" 'ping -c3 -W1 -I 10.101.101.33 10.101.101.34 >/dev/null 2>&1' >/dev/null 2>&1
    say "  brought up; running sweep from slice $FROM"
    out=$(RESUME_FROM=$FROM timeout 1500 "$S/a4-slice-sweep.sh" 2>&1)
    echo "$out"
    nxt=$(echo "$out" | grep -oE "RESUME_FROM=[0-9]+" | tail -1 | cut -d= -f2)
    if [ -n "${nxt:-}" ]; then
        FROM="$nxt"
        say "  wedged; will reboot and resume at slice $FROM"
        continue
    fi
    # ⚠ Absence of RESUME_FROM is NOT success. The sweep also exits without it
    # when it REFUSES to start ("baseline is already broken"), and the first
    # version of this driver reported that as "completed without wedging" --
    # a refusal dressed up as a result. Require positive evidence instead.
    if echo "$out" | grep -q "baseline is already broken"; then
        say "  sweep refused: transit not working on this boot. Rebooting."
        continue
    fi
    if ! echo "$out" | grep -q "^slice 19"; then
        say "  sweep ended early without reaching slice 19 -- treating as failure"
        continue
    fi
    say "=== sweep completed through slice 19 ==="
    exit 0
done
say "=== gave up after $MAXBOOT boots (reached slice $FROM) ==="
