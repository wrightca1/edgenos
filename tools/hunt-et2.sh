#!/bin/bash
# hunt-et2.sh - reboot until Et2 comes up, archiving every boot's settle trace.
#
# Et2 links on about half of identical boots (10-boot measurement, see
# PORT3-BRINGUP.md), so "wait for a good boot" is a coin flip, not a fix. Two
# things are collected on the way:
#
#   1. the FULLSEQ settle trace of EVERY boot, good and bad. Diffing a good
#      boot against a bad one is the only affordable route left -- an A/B
#      comparison needs ~32 boots per arm, a within-boot signal needs one.
#   2. a live window on a good boot, where transit-test.sh can settle A4.
#
# On success it leaves the box UP and runs the two-port bring-up, then stops.
set -u
S="$(cd "$(dirname "$0")" && pwd)"
SW="$S/sw.sh"; EG="$S/eg.sh"
LOG="$S/hunt-et2.log"; TR="$S/traces"; mkdir -p "$TR"
MAX="${MAX:-8}"
IMG=flash:/edgenos-7150-0.3.0-alpha10.swi

say(){ echo "$(date -u +%H:%M:%S) $*" | tee -a "$LOG"; }
edge(){ timeout 45 "$EG" "$@" 2>/dev/null; }
eos(){  timeout 60 "$SW" "$@" 2>/dev/null; }

for i in $(seq 1 "$MAX"); do
    say "--- attempt $i/$MAX ---"
    if edge 'echo ok' | grep -q ok; then
        edge 'sync; (sleep 1; reboot -f) >/dev/null 2>&1 &' >/dev/null
    else
        eos 'bash sync' >/dev/null; eos 'reload now' >/dev/null
    fi
    sleep 45
    t=0; until ping -c1 -W2 10.1.1.77 >/dev/null 2>&1 || [ $t -ge 40 ]; do sleep 15; t=$((t+1)); done
    sleep 60
    eos 'show version | include Uptime' >/dev/null || { say "  no EOS, retrying"; continue; }
    eos 'configure' "boot system $IMG" 'end' >/dev/null
    eos 'bash sync' >/dev/null; eos 'reload now' >/dev/null

    sleep 40
    t=0; until edge 'echo ok' | grep -q ok || [ $t -ge 40 ]; do sleep 15; t=$((t+1)); done
    edge 'echo ok' | grep -q ok || { say "  no EdgeNOS, retrying"; continue; }
    t=0; until edge 'grep -q "FULLSEQ DONE" /var/log/fm6000-fullseq && echo d' | grep -q d || [ $t -ge 40 ]; do sleep 20; t=$((t+1)); done

    edge 'cat /var/log/fm6000-fullseq' > "$TR/boot-$i.log" 2>/dev/null
    fin=$(edge 'grep "final et1" /var/log/fm6000-fullseq')
    et2=$(edge '/mnt/flash/fmdump 0xe4000 1' | awk '{print $2}')
    say "  $fin"
    case "$et2" in
        *08c0|*0cc0)
            mv "$TR/boot-$i.log" "$TR/GOOD-boot-$i.log"
            say "  ★ Et2 UP ($et2) -- trace saved as GOOD-boot-$i.log"
            say "  bringing up both ports"
            edge 'sh /usr/lib/edgenos/platform/edgenos-up.sh >/tmp/up.log 2>&1; tail -3 /tmp/up.log'
            say "=== Et2 is up; box left running ==="
            exit 0;;
        *)
            mv "$TR/boot-$i.log" "$TR/dark-boot-$i.log"
            say "  Et2 dark ($et2) -- trace saved as dark-boot-$i.log";;
    esac
done
say "=== gave up after $MAX attempts ==="
exit 1
