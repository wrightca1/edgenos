#!/bin/bash
# a4-slice-sweep.sh - find which MOD slice performs a given frame edit, by
# disabling one whole slice at a time.
#
# WHY SLICE-LEVEL. Leave-one-out on individual commands needs to know which CAM
# entry fires, which needs the frame's full 48-bit key (MOD_FLAGS alone is 24
# bits). Three hand-picked guesses in a row perturbed nothing. Disabling a slice
# sidesteps the question: whatever would have fired, cannot.
#
# ⚠⚠ THE 2026-08-15 VERSION OF THIS SCRIPT CORRUPTED THE MOD TABLE AND WEDGED
# FORWARDING, then reported five more slices as "no frame" that were really just
# the same wedge. Three rules came out of that, and all three are implemented
# below. Do not remove them:
#
#   1. VERIFY THE SAVE. The old check was [ -n "$saved" ] -- non-empty. A short
#      capture passes that and the restore then writes zeros over a live table.
#      Now: exactly 32 values or the bank is skipped.
#   2. HEALTH-CHECK BETWEEN TESTS. Probe with nothing perturbed. If the frame
#      does not emit, the box is wedged and every later reading is noise. Stop.
#   3. RESTORE IS NOT RECOVERY. A restored table does not un-wedge a dataplane
#      mid-flight. Treat "frame stops" as terminal for the run: reboot, then
#      resume at the next slice with RESUME_FROM.
set -u
S="$(cd "$(dirname "$0")" && pwd)"
EG="$S/eg.sh"; P5="$S/p5.sh"
FROM="${RESUME_FROM:-0}"
edge(){ timeout 90 "$EG" "$@" 2>/dev/null; }

probe(){
  timeout 90 "$P5" 'ip route replace 10.102.1.1/32 via 10.101.101.34 dev swp7 2>/dev/null
     rm -f /tmp/a4.pcap; (tcpdump -i swp6 -s0 -w /tmp/a4.pcap host 10.102.1.1 >/dev/null 2>&1 &)
     sleep 2; ping -c 3 -I 10.101.101.33 10.102.1.1 >/dev/null 2>&1
     sleep 2; killall tcpdump 2>/dev/null; sleep 1
     tcpdump -r /tmp/a4.pcap -nnvv -c 1 2>/dev/null | grep -oE "ttl [0-9]+|bad cksum" | head -2
     ip route del 10.102.1.1/32 via 10.101.101.34 dev swp7 2>/dev/null' 2>/dev/null
}

base_ok=$(probe | grep -c "ttl 63")
[ "$base_ok" -ge 1 ] || { echo "⛔ baseline is already broken -- reboot before starting"; exit 1; }
echo "baseline ok (ttl 63)"

for bank in $(seq "$FROM" 19); do
    base=$((0x159000 + 0x20*bank))
    saved=$(edge "for s in \$(seq 0 31); do printf '%s ' \$(/mnt/flash/fmdump \$(printf 0x%x \$(( $base + \$s ))) 1 | awk '{print \$2}'); done")
    cnt=$(echo $saved | wc -w)
    if [ "$cnt" -ne 32 ]; then
        echo "slice $bank: SKIPPED -- save returned $cnt values, not 32"
        continue
    fi
    edge "i=0; for v in $saved; do fm6000reg 0000:02:00.0 \$(printf 0x%x \$(( $base + \$i ))) \$(printf '0x%x' \$(( 0x\$v & ~0x4000 ))) >/dev/null 2>&1; i=\$((\$i+1)); done" >/dev/null
    r=$(probe | tr '\n' ' ')
    edge "i=0; for v in $saved; do fm6000reg 0000:02:00.0 \$(printf 0x%x \$(( $base + \$i ))) 0x\$v >/dev/null 2>&1; i=\$((\$i+1)); done" >/dev/null

    case "$r" in
      *"ttl 63"*) echo "slice $bank -> $r" ;;
      *"ttl "*)   echo "slice $bank -> $r   <<< TTL CHANGED -- this slice decrements" ;;
      *)          echo "slice $bank -> no frame"
                  if [ "$(probe | grep -c 'ttl 63')" -eq 0 ]; then
                      echo "⛔ WEDGED and did not recover on restore."
                      echo "   Reboot, then: RESUME_FROM=$((bank+1)) $0"
                      exit 2
                  fi ;;
    esac
    case "$r" in *"bad cksum"*) echo "     <<< BAD CKSUM -- this slice fixes the checksum";; esac
done
