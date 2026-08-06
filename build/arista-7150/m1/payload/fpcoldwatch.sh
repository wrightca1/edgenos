#!/bin/bash
# Cold-trace watcher (v2). Arms fmPlatformTraceRegOps at the FIRST fmPlatformWriteCSR so the whole
# cold boot is traced to /var/log/agents/FocalPointV2-<pid>.
#
# v1 TRUNCATED THE CAPTURE and that cost a lot of wasted analysis:
#   sleep 25            -> snapshotted long before port bring-up ran
#   grep ... | head -50000 -> hard 50k-line cap
# The resulting trace had ZERO EPL (0x000exxxx) writes and ended mid-MAPPER, i.e. it covered only the
# first half of the boot. v2 does NOT snapshot at all - we leave the agent log in place and pull it
# over ssh once the box is fully up and Et1 has linked, so nothing is cut short.
OUT=/mnt/flash/fpcold-gdb.out
: > "$OUT"
P=""
for i in $(seq 1 30000); do
  P=$(pgrep -x FocalPointV2); [ -n "$P" ] && break; sleep 0.02
done
if [ -z "$P" ]; then echo "no FPV2 seen" >> "$OUT"; exit 0; fi
echo "caught FPV2 pid=$P at $(cat /proc/uptime)" >> "$OUT"
cat > /tmp/fpc.gdb <<'GDB'
set pagination off
set breakpoint pending on
break fmPlatformWriteCSR
commands
  silent
  printf "ARMING TRACE at first fmPlatformWriteCSR\n"
  call (void)fmPlatformTraceRegOps(1)
  delete
  detach
  quit
end
continue
GDB
gdb -batch -p "$P" -x /tmp/fpc.gdb >> "$OUT" 2>&1
echo "gdb done pid=$P at $(cat /proc/uptime)" >> "$OUT"
# leave tracing ON and the log in /var/log/agents; record where it is for the collector
ls -t /var/log/agents/FocalPointV2-* 2>/dev/null | head -1 >> "$OUT"
echo "TRACE ARMED - collect /var/log/agents/FocalPointV2-* AFTER the link is up" >> "$OUT"
