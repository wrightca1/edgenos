#!/bin/bash
# et2-goodboot-measure.sh -- reboot until Et2 comes up ON ITS OWN, then measure whether a GOOD
# BOOT's Et2 holds lock over minutes, with Et1 sampled in the same sweep as control.
#
# Two things the first attempt got wrong, both now fixed:
#
#  1. `reboot` DOES NOTHING on this box. PID 1 is `/bin/busybox sh`, a plain shell with
#     no reboot handler, so busybox reboot signals it and returns 0 having done nothing.
#     Six "boot attempts" ran against one box that had been up 8 hours. Use `reboot -f`
#     (direct syscall) -- after sync + remount ro, because /mnt/flash is unjournaled vfat.
#  2. EdgeNOS resets /mnt/flash/boot-config to EOS on every boot, so an unplanned restart
#     lands on EOS. boot-config must be rewritten to the alpha SWI before EVERY reboot.
set -u
S=$(dirname "$0")
SWI=${SWI:-edgenos-7150-0.3.0-alpha16.swi}
BOOTS=${BOOTS:-6}
LOG=$S/et2-goodboot-measure.log
: > "$LOG"
say(){ echo "$*" | tee -a "$LOG"; }

wait_ssh(){ for i in $(seq 1 60); do
    timeout 8 "$S/eg.sh" 'echo ok' 2>/dev/null | grep -q ok && return 0; sleep 5; done; return 1; }

# ⚠ /mnt/flash/fullseq.log SURVIVES a reboot, and the new boot does not truncate it until
# FULLSEQ actually starts writing. Grepping it straight after a reboot matches the PREVIOUS
# boot's "FULLSEQ DONE" and its stale "final et1=" line, and the sampling then runs mid-replay
# with both ports still dark. The log is deleted before every reboot so a DONE here is fresh.
wait_fullseq(){ for i in $(seq 1 90); do
    timeout 10 "$S/eg.sh" 'test -f /mnt/flash/fullseq.log && grep -q "FULLSEQ DONE" /mnt/flash/fullseq.log && echo yes' 2>/dev/null \
      | grep -q yes && return 0; sleep 5; done; return 1; }

duty(){  # $1 = sample count -> "et1=<n> et2=<n>"
  timeout $(( $1 * 5 + 40 )) "$S/eg.sh" '
B=$((0xe2000000)); rdw(){ devmem $((B + $1 * 4)) 32; }
e1=0; e2=0
for i in $(seq 1 '"$1"'); do
  [ "$(rdw $((0xe3838)))" = "0x00000940" ] && e1=$((e1+1))
  [ "$(rdw $((0xe4038)))" = "0x00000940" ] && e2=$((e2+1))
  sleep 5
done
echo "et1=$e1 et2=$e2"' 2>/dev/null | tr -d "\r" | tail -1
}

for b in $(seq 1 $BOOTS); do
  say "=== boot $b ==="
  # boot-config AND the log wipe first: both need flash still rw
  timeout 30 "$S/eg.sh" "printf 'SWI=flash:/$SWI\n' > /mnt/flash/boot-config; rm -f /mnt/flash/fullseq.log; sync" >/dev/null 2>&1
  timeout 20 "$S/eg.sh" 'sync; mount -o remount,ro /mnt/flash; sync; (sleep 2; reboot -f) >/dev/null 2>&1 &' >/dev/null 2>&1
  sleep 45
  if ! wait_ssh;      then say "  box did not return -- stopping"; exit 1; fi
  if ! wait_fullseq;  then say "  FULLSEQ never finished -- stopping"; exit 1; fi
  FIN=$(timeout 20 "$S/eg.sh" 'grep "final et1" /mnt/flash/fullseq.log | tail -1' 2>/dev/null | tr -d "\r")
  say "  fullseq: $FIN"
  R=$(duty 20)
  say "  duty over 100s: $R   (et1 is the control)"
  # The control must pass or the data point means nothing: et1 has read 20/20 and 22/22 on
  # every valid sweep, so anything less says the sweep ran mid-replay or the box is unhealthy.
  case "$R" in
    et1=20*) ;;
    *) say "  !! CONTROL FAILED (et1 not 20/20) -- discarding this boot, not counting it"
       continue ;;
  esac
  case "$R" in
    *et2=0) say "  -> dark boot, retrying" ;;
    *) say ""
       say "  >>> GOOD BOOT. Measuring stability over 5 minutes."
       R2=$(duty 60)
       say "  duty over 300s: $R2"
       say ""
       say "=== final chip state ==="
       timeout 30 "$S/eg.sh" 'cat > /tmp/fm6000-status.sh' \
         < /home/smiley/projects/arista/Arista/tools/fm6000-status.sh 2>/dev/null
       timeout 30 "$S/eg.sh" 'sh /tmp/fm6000-status.sh 1 2' 2>&1 | tee -a "$LOG"
       exit 0 ;;
  esac
done
say "no good boot in $BOOTS tries"
exit 2
