#!/bin/bash
# mod-value-sweep.sh - sweep MOD_VALUE_RAM, the operand side MOD_COMMAND_RAM's
# sweep never touched.
#
# The command byte gives an operation SHAPE (0x05 = a 6-byte edit) and the value
# operands supply what gets written. MOD_VALUE_RAM(i1,i0,word) = 0x40*i1 +
# 2*i0 + 0x9400, i.e. 14 banks of 32 entries x 2 words. Data slices 17-30 drive
# banks 0-13 and slice 31 drives bank 14 (mod_decode.value_bank()).
#
# Zeroing a bank sets every operand's Type to 0, so no value bytes are supplied.
# Whichever bank's removal changes the emitted frame is the operand source for
# that edit.
#
# Same three rules as the command sweep, and they are not optional:
#   verify the save (64 words, not "non-empty") -- health-check between tests --
#   a wedge is terminal, reboot and RESUME_FROM.
set -u
S="$(cd "$(dirname "$0")" && pwd)"
EG="$S/eg.sh"; P5="$S/p5.sh"
FROM="${RESUME_FROM:-0}"
edge(){ timeout 120 "$EG" "$@" 2>/dev/null; }

probe(){
  timeout 90 "$P5" 'ip route replace 10.102.1.1/32 via 10.101.101.34 dev swp7 2>/dev/null
     rm -f /tmp/v.pcap; (tcpdump -i swp6 -s0 -w /tmp/v.pcap host 10.102.1.1 >/dev/null 2>&1 &)
     sleep 2; ping -c 3 -I 10.101.101.33 10.102.1.1 >/dev/null 2>&1
     sleep 2; killall tcpdump 2>/dev/null; sleep 1
     tcpdump -r /tmp/v.pcap -nnx -c1 2>/dev/null | sed -n "2p"
     ip route del 10.102.1.1/32 via 10.101.101.34 dev swp7 2>/dev/null' 2>/dev/null
}

# ⚠ Mask the per-packet fields before comparing. The dump line is
#     0x0000:  4500 0054 91b9 4000 3f01 2f03 0a65 6521
# where field 4 is the IP ID and field 7 the header checksum -- both differ on
# every ping. An exact match would report all 15 banks as CHANGED and the run
# would be worthless (observed: bank 0 "changed" when only id and cksum moved).
mask(){ awk '{print $2, $3, $5, $6, $8, $9}'; }

base_line=$(probe | mask)
echo "baseline: $base_line"
echo "$base_line" | grep -q "4500" || { echo "⛔ baseline broken -- reboot first"; exit 1; }

for bank in $(seq "$FROM" 14); do
    base=$((0x159400 + 0x40*bank))
    saved=$(edge "for w in \$(seq 0 63); do printf '%s ' \$(/mnt/flash/fmdump \$(printf 0x%x \$(( $base + \$w ))) 1 | awk '{print \$2}'); done")
    cnt=$(echo $saved | wc -w)
    if [ "$cnt" -ne 64 ]; then echo "value bank $bank: SKIPPED -- save gave $cnt words, not 64"; continue; fi
    edge "for w in \$(seq 0 63); do fm6000reg 0000:02:00.0 \$(printf 0x%x \$(( $base + \$w ))) 0x0 >/dev/null 2>&1; done" >/dev/null
    r=$(probe | mask)
    edge "i=0; for v in $saved; do fm6000reg 0000:02:00.0 \$(printf 0x%x \$(( $base + \$i ))) 0x\$v >/dev/null 2>&1; i=\$((\$i+1)); done" >/dev/null
    if [ "$r" = "$base_line" ]; then
        echo "value bank $bank -> unchanged"
    elif [ -z "$r" ]; then
        echo "value bank $bank -> NO FRAME"
        [ -z "$(probe | mask)" ] && { echo "⛔ WEDGED. Reboot, then RESUME_FROM=$((bank+1))"; exit 2; }
    else
        echo "value bank $bank -> CHANGED"
        echo "     baseline: $base_line"
        echo "     with it zeroed: $r"
    fi
done
