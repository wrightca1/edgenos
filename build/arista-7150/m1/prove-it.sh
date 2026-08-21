#!/bin/bash
# prove-it.sh - produce a transcript that a skeptic can check, showing the 7150
# is genuinely running EdgeNOS and forwarding in silicon.
#
# It is written to answer the three obvious objections:
#
#   "That's just EOS."          -> show no EOS process exists, busybox PID 1,
#                                  our own version.json, and EOS's own SWI sitting
#                                  unused on flash.
#   "It was pre-configured."    -> HARD POWER-CYCLE the board mid-transcript. That
#                                  drops the FM6000 into reset and wipes every
#                                  table. Everything after it was done from cold
#                                  by our software, with timestamps and uptime to
#                                  prove no gap.
#   "That's a mock-up."         -> the corroborating evidence comes from a
#                                  DIFFERENT switch (an Edgecore AS5610 running
#                                  Cumulus) which independently reports the OSPF
#                                  adjacency, and from a TTL decrement, which only
#                                  a real router produces.
#
# Everything is timestamped. Run it, keep the transcript, publish it.
#
#   Usage: prove-it.sh [-o transcript.txt]
# SPDX-License-Identifier: GPL-2.0-or-later
set -u

SW="${SW:-<switch>}"          # the 7150 under test
PEER="${PEER:-<peer>}"     # the independent neighbour (AS5610 / Cumulus)
OUT="${1:-}"
[ "${1:-}" = "-o" ] && OUT="$2"

M1="sshpass -p arista ssh -T -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=10 -o LogLevel=ERROR -o PubkeyAcceptedKeyTypes=+ssh-ed25519 root@$SW"
P5="sshpass -p as5610 ssh -o StrictHostKeyChecking=no -o HostKeyAlgorithms=+ssh-rsa \
    -o PubkeyAcceptedKeyTypes=+ssh-rsa -o KexAlgorithms=+diffie-hellman-group1-sha1 \
    -o Ciphers=+aes128-cbc,3des-cbc -o ConnectTimeout=10 root@$PEER"

say(){ echo; echo "=============================================================="; \
       echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ')  $*"; \
       echo "=============================================================="; }
sw(){  timeout "${2:-60}" $M1 "sh -s" <<< "$1" 2>&1; }
peer(){ timeout "${2:-60}" $P5 "$1" 2>&1 | tr -d '\r'; }

run() {
# Precondition: the switch must already be reachable AND running EdgeNOS. If it
# is mid-reboot the boot-config write in step 3 silently fails and the board
# comes back on EOS in the middle of the transcript.
if ! sw 'test -f /etc/edgenos/version.json && echo EDGENOS_OK' 15 | grep -q EDGENOS_OK; then
    echo "PRECONDITION FAILED: $SW is unreachable, or is not running EdgeNOS."
    echo "Boot the EdgeNOS image, let it settle, then re-run."
    exit 1
fi

say "1. IDENTITY -- is this EOS, or ours?"
sw '
echo "--- what EdgeNOS says it is ---"
cat /etc/edgenos/version.json 2>/dev/null
echo "--- PID 1 (EOS runs systemd + a large agent set; we run busybox) ---"
cat /proc/1/comm; ls -l /proc/1/exe 2>/dev/null | sed "s/.*-> //"
echo "--- any EOS agent running?  (Sysdb/ProcMgr/FocalPointV2 are EOS) ---"
n=$(ps | grep -cE "[S]ysdb|[P]rocMgr|[F]ocalPointV2|[E]osSdk"); echo "EOS processes found: $n"
echo "--- total processes (EOS runs hundreds) ---"; ps | wc -l
echo "--- EOS is still on flash, simply not booted ---"
mount /dev/sda1 /mnt/flash 2>/dev/null
ls -l /mnt/flash/EOS-4.16.8M.swi 2>/dev/null
echo "boot-config: $(cat /mnt/flash/boot-config 2>/dev/null)"
'

say "2. THE ASIC IS DRIVEN BY OUR CODE, NOT A VENDOR SDK"
sw '
echo "--- the FM6000 on PCIe ---"
lspci -n 2>/dev/null | grep 8086:155b
echo "--- our tools, built from source in the repo ---"
ls /usr/bin/fm6000_* 2>/dev/null | head -12
echo "--- no Arista/Intel SDK anywhere ---"
find / -name "libFocalpoint*" -o -name "*EosSdk*" 2>/dev/null | head -3
echo "(nothing listed above = no vendor SDK present)"
'

say "3. HARD POWER-CYCLE -- everything after this is from cold"
echo "Writing 0xdead to SCD 0x7000 power-cycles the board."
echo "This drops the FM6000 into reset and wipes every table in it."
echo
echo "NOTE: EdgeNOS deliberately rewrites boot-config back to EOS on every boot,"
echo "so an unattended reboot always lands on a known-good EOS. That is why the"
echo "boot-config above reads EOS. We point it back at EdgeNOS here, deliberately."
sw '
mkdir -p /mnt/flash; mount /dev/sda1 /mnt/flash 2>/dev/null
IMG=$(ls /mnt/flash/edgenos-7150-*.swi 2>/dev/null | tail -1)
[ -z "$IMG" ] && IMG=/mnt/flash/edgenos-m1-router.swi
echo "SWI=flash:/$(basename $IMG)" > /mnt/flash/boot-config; sync
echo "will boot: $(cat /mnt/flash/boot-config)"
echo "uptime before: $(cut -d. -f1 /proc/uptime)s"
sync; scdreg 0x7000 0xdead' 40
echo "waiting for the board to come back..."
for i in $(seq 1 12); do
    sleep 20
    R=$(sw 'echo "up=$(cut -d. -f1 /proc/uptime)"' 12 | tr -d "\n")
    case "$R" in *up=*) echo "  -> back after ~$((i*20))s: $R"; break;; esac
    echo "  [$i] still down"
done

say "4. FROM COLD: the ASIC was brought up by our software"
echo "The boot-time bring-up replays ~390k register writes with pacing and takes"
echo "~80 s. Waiting for it to finish before sampling (otherwise we photograph a"
echo "half-configured chip and it looks like a failure)."
sw '
i=1
while [ $i -le 20 ]; do
    rx=$(fm6000reg 0000:02:00.0 0xe3826 2>/dev/null | sed "s/.*= 0x//")
    [ "$rx" = "00000001" ] && { echo "  dataplane ready at uptime $(cut -d. -f1 /proc/uptime)s"; break; }
    sleep 8; i=$((i+1))
done' 200
sw '
echo "uptime: $(cut -d. -f1 /proc/uptime)s  <-- small number = we really did reboot"
R(){ fm6000reg 0000:02:00.0 "$1" 2>/dev/null | sed "s/.*= 0x//"; }
echo "--- link state (0x8c0/0xcc0 with rx=1 means a trained 10G link) ---"
echo "  Et1 PORT_STATUS=$(R 0xe3800) pcsRx=$(R 0xe3826)"
echo "  Et2 PORT_STATUS=$(R 0xe4000) pcsRx=$(R 0xe4026)"
echo "--- thermal: our controller, reading the real die sensor ---"
head -4 /var/log/thermal 2>/dev/null
H=$(for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name 2>/dev/null)" = max6658 ] && echo $h; done)
[ -n "$H" ] && echo "  die=$(( $(cat $H/temp2_input)/1000 ))C  fan pwm=$(cat /sys/class/hwmon/hwmon0/pwm1) rpm=$(cat /sys/class/hwmon/hwmon0/fan1_input)"
' 90

say "5. CONTROL PLANE: start OSPF from cold"
sw 'sh /usr/lib/edgenos/platform/edgenos-up.sh > /tmp/up.log 2>&1; echo "exit=$?"' 300
sw 'grep -E "^\[up\]|programmed|slot " /tmp/up.log | tail -10' 60

say "6. INDEPENDENT CORROBORATION -- from a DIFFERENT switch"
echo "The neighbour is an Edgecore AS5610 running Cumulus Linux + Quagga."
echo "It has no idea what we are; it just sees an OSPF speaker."
peer '( echo zebra; sleep 2; echo "show ip ospf neighbor"; sleep 3; echo exit ) \
      | timeout 15 telnet 127.0.0.1 2604 2>/dev/null | tr -d "\r" \
      | grep -E "Neighbor ID|10\.101\.255\.26|Full"' 90

say "7. HARDWARE FORWARDING -- the TTL proves it is the ASIC, not software"
echo "A /32 via the switch; the chip routes it back out and decrements TTL."
peer '
ip neigh replace 10.101.101.26 lladdr 44:4c:a8:31:5d:ab dev swp6
ip route replace <admin-net-host>/32 via 10.101.101.26 dev swp6
(timeout 14 tcpdump -i swp6 -n -v -c 4 "icmp and host <admin-net-host>" 2>/dev/null > /tmp/p.txt) &
sleep 2; ping -c 1 -W 2 -t 64 <admin-net-host> >/dev/null 2>&1; sleep 13
echo "--- in, then out, captured on the NEIGHBOUR (ttl must drop by 1) ---"
grep ttl /tmp/p.txt | head -2
ip route del <admin-net-host>/32 via 10.101.101.26 dev swp6 2>/dev/null
ip neigh del 10.101.101.26 dev swp6 2>/dev/null' 120

say "8. THE ROUTES IN SILICON CAME FROM OSPF"
sw '
echo "--- kernel routes learned by OSPF ---"
ip route show | grep "via 10.101.101.25" | head -6
echo "--- the same prefixes read back out of the ASIC FIB ---"
/usr/bin/fm6000_route dump 2>/dev/null | awk "\$1>=241 && \$1<=248"
' 90

say "DONE -- $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
}

if [ -n "$OUT" ]; then run 2>&1 | tee "$OUT"; echo; echo "transcript: $OUT"
else run; fi
