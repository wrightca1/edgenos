#!/bin/sh
set -u
D=/mnt/flash/csrdump; L=/tmp/cp2.txt
printf '00205230\n0020b3c2\n' > $L
rd1() { $D $L 2>/dev/null | sed -n "${1}p" | awk '{print $2}'; }
probe() {
    a1=$(rd1 1); a2=$(rd1 2)
    loss=$(eval "$2" 2>&1 | grep -oE '[0-9]+% packet loss' | head -1)
    sleep 2
    b1=$(rd1 1); b2=$(rd1 2)
    printf "  %-16s b5.280 %+4d  b11.481 %+4d   %s\n" "$1" \
        $(( 0x$b1 - 0x$a1 )) $(( 0x$b2 - 0x$a2 )) "$loss"
}
probe "IDLE-before" "sleep 12"
for d in "$@"; do probe "$d" "ping -c 10 -W 1 $d"; done
probe "IDLE-after" "sleep 12"
