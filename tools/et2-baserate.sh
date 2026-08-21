#!/bin/bash
# et2-baserate.sh -- measure how often Et2 comes up under EdgeNOS.
#
# One arm, held constant: EOS -> EdgeNOS, unspliced replay. Every earlier Et2
# claim was one boot per condition on a port that does not come up reliably,
# so this establishes the base rate that future claims must be tested against.
#
# Per iteration: reboot (boot-config self-reverts, so this lands on EOS) ->
# arm alpha9 -> reload -> wait for FULLSEQ -> sample Et2 for SAMPLE_S.
#
# A boot is TAINTED if the box reset again on the way, because that makes it an
# EdgeNOS -> EdgeNOS boot, a different arm. Detected by comparing the switch's
# own uptime against our elapsed wall time.
set -u
S="$(cd "$(dirname "$0")" && pwd)"
SW="$S/sw.sh"; EG="$S/eg.sh"
LOG="$S/et2-baserate.log"
N="${N:-10}"
SAMPLE_S="${SAMPLE_S:-180}"
IMG=flash:/edgenos-7150-0.3.0-alpha9.swi

say(){ echo "$(date -u +%H:%M:%S) $*" | tee -a "$LOG"; }
edge(){ timeout 45 "$EG" "$@" 2>/dev/null; }
eos(){  timeout 60 "$SW" "$@" 2>/dev/null; }

up=0; dark=0; tainted=0; failed=0
say "=== Et2 base rate: N=$N, sample ${SAMPLE_S}s, arm = EOS->EdgeNOS unspliced ==="

for i in $(seq 1 "$N"); do
    say "--- boot $i/$N ---"

    # 1. get to EOS. From EdgeNOS a reboot lands there (boot-config reverts).
    if edge 'echo ok' | grep -q ok; then
        edge 'sync; (sleep 1; reboot -f) >/dev/null 2>&1 &' >/dev/null
    else
        eos 'bash sync' >/dev/null; eos 'reload now' >/dev/null
    fi
    sleep 45
    t=0; until ping -c1 -W2 <switch> >/dev/null 2>&1 || [ $t -ge 40 ]; do sleep 15; t=$((t+1)); done
    sleep 60          # EOS needs ~4 min total before ports and CLI settle
    if ! eos 'show version | include Uptime' | grep -q Uptime; then
        say "  boot $i: FAILED to reach EOS"; failed=$((failed+1)); continue
    fi

    md5=$(eos 'bash md5sum /mnt/flash/fwd4.txt' | grep -oE '^[0-9a-f]{32}')
    say "  fwd4 md5 $md5"

    # 2. arm and reload, remembering when so we can spot an extra reset
    eos 'configure' "boot system $IMG" 'end' >/dev/null
    if ! eos 'show boot-config' | grep -q alpha9; then
        say "  boot $i: FAILED to arm"; failed=$((failed+1)); continue
    fi
    eos 'bash sync' >/dev/null
    t0=$(date +%s)
    eos 'reload now' >/dev/null

    # 3. wait for EdgeNOS, then for the sequence
    sleep 40
    t=0; until edge 'echo ok' | grep -q ok || [ $t -ge 40 ]; do sleep 15; t=$((t+1)); done
    if ! edge 'echo ok' | grep -q ok; then
        say "  boot $i: FAILED to reach EdgeNOS"; failed=$((failed+1)); continue
    fi
    t=0; until edge 'grep -q "FULLSEQ DONE" /var/log/fm6000-fullseq && echo d' | grep -q d || [ $t -ge 40 ]; do sleep 20; t=$((t+1)); done
    if ! edge 'grep -q "FULLSEQ DONE" /var/log/fm6000-fullseq && echo d' | grep -q d; then
        say "  boot $i: FAILED, FULLSEQ never completed"; failed=$((failed+1)); continue
    fi

    # 4. taint check: switch uptime should account for our whole elapsed time
    upt=$(edge 'cut -d. -f1 /proc/uptime' | tr -dc 0-9)
    elapsed=$(( $(date +%s) - t0 ))
    [ -z "$upt" ] && upt=0
    slip=$(( elapsed - upt ))
    parser=$(edge '/mnt/flash/fmdump 0x1082a4 1' | awk '{print $2}')

    # 5. sample
    best=dark; series=""
    n=$(( SAMPLE_S / 30 )); [ $n -lt 1 ] && n=1
    for k in $(seq 1 $n); do
        v=$(edge '/mnt/flash/fmdump 0xe4000 1' | awk '{print $2}')
        series="$series ${v:-????}"
        case "$v" in *08c0|*0cc0) best=UP;; esac
        sleep 30
    done
    et1=$(edge '/mnt/flash/fmdump 0xe3800 1' | awk '{print $2}')

    if [ "$slip" -gt 120 ]; then
        say "  boot $i: TAINTED (extra reset: elapsed ${elapsed}s vs uptime ${upt}s)  Et2:$best"
        tainted=$((tainted+1))
    elif [ "$best" = UP ]; then
        say "  boot $i: Et2 UP    Et1=$et1 parser=$parser uptime=${upt}s"; up=$((up+1))
    else
        say "  boot $i: Et2 dark  Et1=$et1 parser=$parser uptime=${upt}s"; dark=$((dark+1))
    fi
    say "     series:$series"
    say "     running total: up=$up dark=$dark tainted=$tainted failed=$failed"
done

say "=== RESULT: Et2 up $up / $((up+dark)) valid boots  (tainted=$tainted failed=$failed) ==="
